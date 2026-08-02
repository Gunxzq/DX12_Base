#include "FbxMeshConverter.h"
#include "Asset/Definitions/Material/MaterialDesc.h"
#include "Asset/Definitions/Mesh/DxMeshFormat.h"
#include "Asset/IO/Writer/DxMeshWriter.h"
#include "TextureConverter.h"

#include <DirectXMath.h>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

namespace fs = std::filesystem;

namespace AssetTool {

namespace {

// ==========================================================================
// 骨骼名处理（FBX 命名 → 引擎命名）
// ==========================================================================

/// 去 _bone 后缀："Body_d_bone" → "Body_d"
std::string StripBoneSuffix(const std::string &name) {
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, "_bone") == 0)
        return name.substr(0, name.size() - 5);
    return name;
}

/// 是否为 _end 末端节点（Blender 为显示骨骼方向加的辅助节点，引擎不需要）
bool IsEndNode(const std::string &name) { return name.size() >= 4 && name.compare(name.size() - 4, 4, "_end") == 0; }

// ==========================================================================
// 骨架收集
// ==========================================================================

struct BoneInfo {
    std::string name;                // 引擎名（去 _bone 后缀）
    int parentIndex = -1;            // 父骨骼索引（-1 = 根）
    DirectX::XMFLOAT4X4 worldMatrix; // 世界矩阵（节点树累积，相对场景根）
};

/// 递归遍历节点树：收集骨骼层级（去 _bone 后缀、过滤 _end）
/// 骨骼父节点 = 节点树中最近的骨骼祖先；worldMatrix 累积全部祖先变换
/// （父子骨骼间可能存在非骨骼节点如 Armature/空节点，故先算世界矩阵，
///  局部矩阵在 WriteBoneJSON 中由 world[child] × inv(world[parent]) 推出）
void CollectBones(const aiNode *node, const std::set<std::string> &boneNameSet, int nearestBoneParent,
                  const DirectX::XMMATRIX &parentWorld, std::vector<BoneInfo> &bones,
                  std::map<std::string, int> &boneIndexByName) {
    if (!node)
        return;

    DirectX::XMFLOAT4X4 nodeLocal;
    memcpy(&nodeLocal, &node->mTransformation, sizeof(DirectX::XMFLOAT4X4));
    // 行主序 post-multiply（与 RobotMerger 的 local × parent 约定一致）
    DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&nodeLocal) * parentWorld;

    std::string nodeName = node->mName.length ? node->mName.C_Str() : "";
    std::string stripped = StripBoneSuffix(nodeName);
    bool isBone = !nodeName.empty() && !IsEndNode(nodeName) && boneNameSet.count(stripped) > 0;

    int thisBoneIndex = nearestBoneParent;
    if (isBone) {
        BoneInfo info;
        info.name = stripped;
        info.parentIndex = nearestBoneParent;
        DirectX::XMStoreFloat4x4(&info.worldMatrix, world);
        thisBoneIndex = static_cast<int>(bones.size());
        boneIndexByName[stripped] = thisBoneIndex;
        bones.push_back(info);
    }

    for (unsigned int ci = 0; ci < node->mNumChildren; ++ci)
        CollectBones(node->mChildren[ci], boneNameSet, thisBoneIndex, world, bones, boneIndexByName);
}

// ==========================================================================
// 材质：aiMaterial → 引擎 .mat
// ==========================================================================

/// 从 aiMaterial 提取 PBR 参数（Blender Principled BSDF）
void ExtractMaterial(const aiMaterial *mat, DX12Engine::Resource::MaterialDesc &desc) {
    desc.shader = "PBR/Standard";

    aiColor4D diffuse(0.8f, 0.8f, 0.8f, 1.0f);
    if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS) {
        desc.params.baseColor[0] = diffuse.r;
        desc.params.baseColor[1] = diffuse.g;
        desc.params.baseColor[2] = diffuse.b;
        desc.params.baseColor[3] = diffuse.a;
    }
    float metallic = 0.0f;
    if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS)
        desc.params.metallic = metallic;
    float roughness = 0.5f;
    if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS)
        desc.params.roughness = roughness;

    // 自发光（Blender Principled BSDF Emission）：颜色 + 强度两步
    // FBX 导出为 EmissiveColor（RGB）+ EmissiveFactor（标量），assimp 分别映射为
    // AI_MATKEY_COLOR_EMISSIVE / AI_MATKEY_EMISSIVE_INTENSITY（见 vcpkg assimp material.h）
    aiColor3D emissive(0.0f, 0.0f, 0.0f);
    if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
        desc.params.emissive[0] = emissive.r;
        desc.params.emissive[1] = emissive.g;
        desc.params.emissive[2] = emissive.b;
    }
    float emissiveIntensity = 1.0f;
    if (mat->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissiveIntensity) == AI_SUCCESS && emissiveIntensity != 1.0f) {
        desc.params.emissive[0] *= emissiveIntensity;
        desc.params.emissive[1] *= emissiveIntensity;
        desc.params.emissive[2] *= emissiveIntensity;
    }
}

/// 尝试复制材质基础色纹理到 Textures/，返回 Textures/ 下文件名（空 = 无纹理）
std::string CopyMaterialTexture(const aiMaterial *mat, const fs::path &fbxDir, const fs::path &outTexDir) {
    if (!mat)
        return "";
    aiString texPath;
    if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) != AI_SUCCESS)
        return "";

    std::string texFile = texPath.C_Str();
    if (texFile.empty())
        return "";

    fs::path srcPath(texFile);
    // FBX 内嵌纹理引用可能是相对路径或绝对路径；先按相对 fbx 目录找，再按原路径找
    fs::path candidates[] = {
        fbxDir / srcPath,
        fs::path(texFile),
    };
    fs::path found;
    for (const auto &c : candidates) {
        if (fs::exists(c)) {
            found = c;
            break;
        }
    }
    if (found.empty())
        return "";

    std::string ext = found.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // PNG → DDS（与 importrobot 一致）
    if (ext == ".png") {
        std::string ddsName = found.stem().string() + ".dds";
        auto r = ConvertPNGToDDS(found.string(), (outTexDir / ddsName).string());
        if (r.success)
            return ddsName;
    }
    // 其他格式（dds/bmp 等）直接复制
    try {
        fs::copy_file(found, outTexDir / found.filename(), fs::copy_options::overwrite_existing);
        return found.filename().string();
    } catch (...) {
        return "";
    }
}

// ==========================================================================
// .bone JSON 写入（复用 importrobot 的 TRS 分解逻辑）
// ==========================================================================

bool WriteBoneJSON(const std::vector<BoneInfo> &bones, const std::string &bonePath) {
    nlohmann::json root;
    root["version"] = 1;
    auto &jBones = root["bones"] = nlohmann::json::array();

    // 左手系：世界矩阵 Z 列取反（与 dxmesh 顶点翻转同步）→ 局部 = world[child] × inv(world[parent])
    std::vector<DirectX::XMMATRIX> worldLH(bones.size());
    for (size_t bi = 0; bi < bones.size(); ++bi) {
        DirectX::XMMATRIX w = DirectX::XMLoadFloat4x4(&bones[bi].worldMatrix);
        // Z 列取反（行主序：r0.z / r1.z / r2.z / r3.z）
        w.r[0].m128_f32[2] = -w.r[0].m128_f32[2];
        w.r[1].m128_f32[2] = -w.r[1].m128_f32[2];
        w.r[2].m128_f32[2] = -w.r[2].m128_f32[2];
        w.r[3].m128_f32[2] = -w.r[3].m128_f32[2];
        worldLH[bi] = w;
    }

    for (size_t bi = 0; bi < bones.size(); ++bi) {
        nlohmann::json jBone;
        jBone["name"] = bones[bi].name;
        jBone["parentIndex"] = bones[bi].parentIndex;

        // 局部矩阵 = world[child] × inverse(world[parent])（行主序 post-multiply）
        DirectX::XMMATRIX mL;
        if (bones[bi].parentIndex >= 0)
            mL = worldLH[bi] * DirectX::XMMatrixInverse(nullptr, worldLH[bones[bi].parentIndex]);
        else
            mL = worldLH[bi];

        DirectX::XMFLOAT4X4 m4x4;
        DirectX::XMStoreFloat4x4(&m4x4, mL);
        DirectX::XMVECTOR s, r, t;
        if (DirectX::XMMatrixDecompose(&s, &r, &t, mL)) {
            DirectX::XMFLOAT3 sf, tf;
            DirectX::XMFLOAT4 rf;
            DirectX::XMStoreFloat3(&sf, s);
            DirectX::XMStoreFloat3(&tf, t);
            DirectX::XMStoreFloat4(&rf, r);
            jBone["position"] = {tf.x, tf.y, tf.z};
            jBone["rotation"] = {rf.x, rf.y, rf.z, rf.w};
            jBone["scale"] = {sf.x, sf.y, sf.z};
        } else {
            jBone["position"] = {m4x4._41, m4x4._42, m4x4._43};
            jBone["rotation"] = {0.0f, 0.0f, 0.0f, 1.0f};
            jBone["scale"] = {1.0f, 1.0f, 1.0f};
        }
        jBones.push_back(jBone);
    }

    std::ofstream ofs(bonePath);
    if (!ofs.is_open())
        return false;
    ofs << root.dump(2);
    return true;
}

} // namespace

// ==========================================================================
// FbxMeshConverter::Convert
// ==========================================================================

FbxConvertResult FbxMeshConverter::Convert(const std::string &fbxPath, const std::string &outputDir,
                                           const FbxConvertOptions &options) {
    FbxConvertResult result;
    if (fbxPath.empty() || outputDir.empty()) {
        result.error = "fbxPath / outputDir is empty";
        return result;
    }

    fs::path fbxFsPath(fbxPath);
    std::string stem = fbxFsPath.stem().string();
    result.stem = stem;

    // ── 1. assimp 读取 FBX ──
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(fbxPath, aiProcess_Triangulate | aiProcess_GenNormals);
    if (!scene || !scene->HasMeshes()) {
        result.error = "Failed to read FBX: " + std::string(importer.GetErrorString());
        return result;
    }
    result.meshCount = static_cast<int>(scene->mNumMeshes);

    // ── 2. 收集骨骼名（从所有网格的 mBones，去 _bone 后缀、过滤 _end）──
    std::set<std::string> boneNameSet;
    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh *m = scene->mMeshes[mi];
        for (unsigned int bi = 0; bi < m->mNumBones; ++bi) {
            std::string bn = m->mBones[bi]->mName.C_Str();
            if (IsEndNode(bn))
                continue;
            boneNameSet.insert(StripBoneSuffix(bn));
        }
    }

    // ── 3. 从节点树构建骨骼层级 ──
    std::vector<BoneInfo> bones;
    std::map<std::string, int> boneIndexByName;
    if (scene->mRootNode)
        CollectBones(scene->mRootNode, boneNameSet, -1, DirectX::XMMatrixIdentity(), bones, boneIndexByName);
    result.boneCount = static_cast<int>(bones.size());

    // ── 4. 合并网格 → DxMeshSkinnedVertex ──
    std::vector<DxMeshSkinnedVertex> allVerts;
    std::vector<uint32_t> allIndices;
    std::vector<DxMeshSubMesh> subMeshes;
    std::vector<std::string> materialKeys;
    std::vector<DX12Engine::Resource::MaterialDesc> materialDescs;

    float bMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float bMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    uint32_t vOffset = 0;

    fs::path robotOutDir(outputDir);
    fs::path outMatsDir = robotOutDir / "Materials";
    fs::path outTexDir = robotOutDir / "Textures";
    fs::create_directories(outMatsDir);
    fs::create_directories(outTexDir);
    fs::path fbxDir = fbxFsPath.parent_path();

    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh *m = scene->mMeshes[mi];
        if (!m->HasPositions())
            continue;

        // 材质（nullptr 防护：ExtractMaterial / CopyMaterialTexture 内部已判空）
        DX12Engine::Resource::MaterialDesc matDesc;
        const aiMaterial *mat =
            (m->mMaterialIndex < scene->mNumMaterials) ? scene->mMaterials[m->mMaterialIndex] : nullptr;
        if (mat)
            ExtractMaterial(mat, matDesc);
        std::string texName = CopyMaterialTexture(mat, fbxDir, outTexDir);

        // 子网格唯一名：{stem}_{mi:03d}，稳定且与子网格顺序一致
        char seqBuf[16];
        snprintf(seqBuf, sizeof(seqBuf), "%03u", mi);
        std::string matKey = stem + "_" + seqBuf;
        materialKeys.push_back(matKey);
        matDesc.textures.baseColor = texName;
        materialDescs.push_back(matDesc);

        // 顶点权重 → boneIndex 映射（每网格每骨骼）
        // FBX 顶点蒙皮：mBones[bi]->mWeights 给出 (vertexId, weight)
        std::vector<int> vertexBoneIndex(m->mNumVertices, -1);
        std::vector<float> vertexBoneWeight(m->mNumVertices, 0.0f);
        for (unsigned int bi = 0; bi < m->mNumBones; ++bi) {
            std::string bn = m->mBones[bi]->mName.C_Str();
            if (IsEndNode(bn))
                continue;
            std::string stripped = StripBoneSuffix(bn);
            auto it = boneIndexByName.find(stripped);
            if (it == boneIndexByName.end())
                continue;
            int boneIdx = it->second;
            for (unsigned int wi = 0; wi < m->mBones[bi]->mNumWeights; ++wi) {
                unsigned int vid = m->mBones[bi]->mWeights[wi].mVertexId;
                float w = m->mBones[bi]->mWeights[wi].mWeight;
                if (vid < m->mNumVertices && w > vertexBoneWeight[vid]) {
                    vertexBoneWeight[vid] = w;
                    vertexBoneIndex[vid] = boneIdx;
                }
            }
        }

        // 顶点转换
        uint32_t vc = m->mNumVertices;
        for (unsigned int vi = 0; vi < vc; ++vi) {
            DxMeshSkinnedVertex v = {};
            v.position[0] = m->mVertices[vi].x;
            v.position[1] = m->mVertices[vi].y;
            v.position[2] = m->mVertices[vi].z;
            if (m->HasNormals()) {
                v.normal[0] = m->mNormals[vi].x;
                v.normal[1] = m->mNormals[vi].y;
                v.normal[2] = m->mNormals[vi].z;
            } else {
                v.normal[0] = 0.0f;
                v.normal[1] = 1.0f;
                v.normal[2] = 0.0f;
            }
            v.tangentU[0] = 1.0f;
            v.tangentU[1] = 0.0f;
            v.tangentU[2] = 0.0f;
            if (m->HasTextureCoords(0)) {
                v.texC[0] = m->mTextureCoords[0][vi].x;
                v.texC[1] = m->mTextureCoords[0][vi].y;
            }

            // 刚性绑定：取权重最大的骨骼（FBX 每网格通常只有 1 根有权重）
            int boneIdx = vertexBoneIndex[vi];
            if (boneIdx < 0)
                boneIdx = 0; // 无权重兜底：绑到根骨骼
            v.boneWeights[0] = 1.0f;
            v.boneIndices[0] = static_cast<uint8_t>(boneIdx);

            // 右手 Y-up → 左手 Y-up（翻转 Z，与 importrobot 一致）
            if (options.leftHanded) {
                v.position[2] = -v.position[2];
                v.normal[2] = -v.normal[2];
                v.tangentU[2] = -v.tangentU[2];
            }

            allVerts.push_back(v);
            bMin[0] = (std::min)(bMin[0], v.position[0]);
            bMin[1] = (std::min)(bMin[1], v.position[1]);
            bMin[2] = (std::min)(bMin[2], v.position[2]);
            bMax[0] = (std::max)(bMax[0], v.position[0]);
            bMax[1] = (std::max)(bMax[1], v.position[1]);
            bMax[2] = (std::max)(bMax[2], v.position[2]);
        }

        // 索引（绕序：右手 → 左手镜像后需翻转，保持面朝向一致）
        uint32_t sIdx = static_cast<uint32_t>(allIndices.size());
        for (unsigned int fi = 0; fi < m->mNumFaces; ++fi) {
            const aiFace &f = m->mFaces[fi];
            if (f.mNumIndices != 3)
                continue;
            uint32_t i0 = vOffset + f.mIndices[0];
            uint32_t i1 = vOffset + f.mIndices[1];
            uint32_t i2 = vOffset + f.mIndices[2];
            if (options.leftHanded) {
                allIndices.push_back(i0);
                allIndices.push_back(i2);
                allIndices.push_back(i1);
            } else {
                allIndices.push_back(i0);
                allIndices.push_back(i1);
                allIndices.push_back(i2);
            }
        }

        DxMeshSubMesh sm = {};
        sm.indexOffset = sIdx;
        sm.indexCount = static_cast<uint32_t>(allIndices.size() - sIdx);
        sm.vertexOffset = vOffset;
        subMeshes.push_back(sm);
        vOffset += vc;
        result.vertexCount += static_cast<int>(vc);
        result.indexCount += static_cast<int>(sm.indexCount);
    }

    if (allVerts.empty()) {
        result.error = "No vertices extracted from FBX";
        return result;
    }

    // ── 5. 写入 .dxmesh ──
    std::string dxmeshPath = robotOutDir.string() + "/" + stem + ".dxmesh";
    if (!DX12Engine::Asset::DxMeshWriter::Write(allVerts.data(), allVerts.size(), sizeof(DxMeshSkinnedVertex),
                                                allIndices.data(), allIndices.size(), 4,
                                                std::wstring(dxmeshPath.begin(), dxmeshPath.end()), bMin, bMax, true,
                                                subMeshes.data(), static_cast<uint32_t>(subMeshes.size()))) {
        result.error = "Failed to write .dxmesh: " + dxmeshPath;
        return result;
    }
    result.outputFiles.push_back(dxmeshPath);

    // ── 6. 写入 .bone ──
    std::string bonePath = robotOutDir.string() + "/" + stem + ".bone";
    if (!WriteBoneJSON(bones, bonePath)) {
        result.error = "Failed to write .bone: " + bonePath;
        return result;
    }
    result.outputFiles.push_back(bonePath);

    // ── 7. 输出材质 ──
    for (size_t i = 0; i < materialKeys.size(); ++i) {
        const auto &desc = materialDescs[i];
        nlohmann::json jm;
        jm["shader"] = desc.shader;
        jm["params"]["baseColor"] = {desc.params.baseColor[0], desc.params.baseColor[1], desc.params.baseColor[2],
                                     desc.params.baseColor[3]};
        jm["params"]["metallic"] = desc.params.metallic;
        jm["params"]["roughness"] = desc.params.roughness;
        jm["params"]["ao"] = desc.params.ao;
        jm["params"]["emissive"] = {desc.params.emissive[0], desc.params.emissive[1], desc.params.emissive[2],
                                    desc.params.emissive[3]};
        if (!desc.textures.baseColor.empty())
            jm["textures"]["baseColor"] = desc.textures.baseColor;

        std::string matPath = (outMatsDir / (materialKeys[i] + ".mat")).string();
        std::ofstream mf(matPath);
        if (mf)
            mf << jm.dump(2);
        result.outputFiles.push_back(matPath);
    }

    // ── 8. 写入 scene.json（与 importrobot 输出同构；skinned.skeleton 指向 .bone）──
    std::string scenePath = robotOutDir.string() + "/" + stem + ".scene.json";
    {
        nlohmann::json scene;
        scene["version"] = 1;
        scene["metadata"]["name"] = stem;

        nlohmann::json deps = nlohmann::json::object();
        nlohmann::json materials = nlohmann::json::array();
        for (size_t i = 0; i < materialKeys.size(); ++i) {
            materials.push_back(materialKeys[i]);
            deps[materialKeys[i]] = nlohmann::json::object();
            deps[materialKeys[i]]["type"] = "material";
        }

        nlohmann::json entity;
        entity["name"] = stem;
        entity["components"]["transform"]["position"] = {0, 0, 0};
        entity["components"]["transform"]["rotation"] = {0, 0, 0, 1};
        entity["components"]["transform"]["scale"] = {1, 1, 1};
        entity["components"]["mesh"]["geometry"] = stem + ".dxmesh";
        entity["components"]["mesh"]["materials"] = materials;
        entity["components"]["skinned"]["skeleton"] = stem + ".bone";

        scene["entities"] = nlohmann::json::array({entity});
        scene["dependencies"] = deps;

        std::ofstream ofs(scenePath);
        if (!ofs.is_open()) {
            result.error = "Failed to write scene.json";
            return result;
        }
        ofs << scene.dump(2);
    }
    result.outputFiles.push_back(scenePath);

    result.materialKeys = materialKeys;
    result.success = true;
    return result;
}

} // namespace AssetTool
