#include "RobotMerger.h"
#include "ANIParser.h"
#include "Asset/Definitions/Material/MaterialDesc.h"
#include "Asset/IO/Writer/DxMeshWriter.h"
#include "TextureConverter.h"

#include <DirectXMath.h>

#include <assimp/Exporter.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace AssetTool {

// ==========================================================================
// Mat4x4 实现
// ==========================================================================

RobotMerger::Mat4x4 RobotMerger::Mat4x4::Identity() {
    Mat4x4 r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

RobotMerger::Mat4x4 RobotMerger::Mat4x4::operator*(const Mat4x4 &rhs) const {
    Mat4x4 r = {};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            for (int k = 0; k < 4; ++k)
                r.m[row * 4 + col] += m[row * 4 + k] * rhs.m[k * 4 + col];
    return r;
}

void RobotMerger::Mat4x4::TransformPoint(float &x, float &y, float &z) const {
    float nx = x * m[0] + y * m[4] + z * m[8] + m[12];
    float ny = x * m[1] + y * m[5] + z * m[9] + m[13];
    float nz = x * m[2] + y * m[6] + z * m[10] + m[14];
    float nw = x * m[3] + y * m[7] + z * m[11] + m[15];
    if (nw != 0.0f) {
        x = nx / nw;
        y = ny / nw;
        z = nz / nw;
    } else {
        x = nx;
        y = ny;
        z = nz;
    }
}

void RobotMerger::Mat4x4::TransformDirection(float &x, float &y, float &z) const {
    float nx = x * m[0] + y * m[4] + z * m[8];
    float ny = x * m[1] + y * m[5] + z * m[9];
    float nz = x * m[2] + y * m[6] + z * m[10];
    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-8f) {
        x = nx / len;
        y = ny / len;
        z = nz / len;
    }
}

void RobotMerger::Mat4x4::TransformPointRaw(float &x, float &y, float &z) const {
    float nx = x * m[0] + y * m[4] + z * m[8] + m[12];
    float ny = x * m[1] + y * m[5] + z * m[9] + m[13];
    float nz = x * m[2] + y * m[6] + z * m[10] + m[14];
    float nw = x * m[3] + y * m[7] + z * m[11] + m[15];
    if (nw != 0.0f) {
        x = nx / nw;
        y = ny / nw;
        z = nz / nw;
    } else {
        x = nx;
        y = ny;
        z = nz;
    }
}

// ── 4×4 矩阵求逆（高斯消元） ──
RobotMerger::Mat4x4 RobotMerger::Mat4x4::Inverse() const {
    float a[4][8];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j)
            a[i][j] = m[i * 4 + j];
        for (int j = 4; j < 8; ++j)
            a[i][j] = (i == (j - 4)) ? 1.0f : 0.0f;
    }
    for (int col = 0; col < 4; ++col) {
        int sel = col;
        for (int i = col + 1; i < 4; ++i)
            if (fabsf(a[i][col]) > fabsf(a[sel][col]))
                sel = i;
        if (fabsf(a[sel][col]) < 1e-10f)
            return Identity();
        if (sel != col)
            for (int j = 0; j < 8; ++j)
                std::swap(a[col][j], a[sel][j]);
        float div = a[col][col];
        for (int j = 0; j < 8; ++j)
            a[col][j] /= div;
        for (int i = 0; i < 4; ++i) {
            if (i == col)
                continue;
            float mul = a[i][col];
            for (int j = 0; j < 8; ++j)
                a[i][j] -= mul * a[col][j];
        }
    }
    Mat4x4 r = {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i * 4 + j] = a[i][j + 4];
    return r;
}

// ==========================================================================
// IsRenderBone
// ==========================================================================

bool RobotMerger::IsRenderBone(const std::string &name) {
    std::string lower;
    for (char c : name)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("hit") != std::string::npos)
        return false;
    if (lower.find("root") != std::string::npos)
        return false;
    if (lower.find("weapon_point") != std::string::npos)
        return false;
    if (lower.find("fannel") != std::string::npos)
        return false;
    if (lower.find("gun") != std::string::npos)
        return false;
    if (lower.find("sword") != std::string::npos)
        return false;
    if (lower.find("shield") != std::string::npos)
        return false;
    if (lower.find("missile") != std::string::npos)
        return false;
    return true;
}

// ==========================================================================
// Merge 实现
// ==========================================================================

RobotMergeResult RobotMerger::Merge(const std::string &hodPath, const std::string &outputDir,
                                    const RobotMergeOptions &options) {
    return MergeWithCallback(hodPath, outputDir, options, nullptr);
}

RobotMergeResult RobotMerger::MergeWithCallback(const std::string &hodPath, const std::string &outputDir,
                                                const RobotMergeOptions &options, ProgressCallback callback) {
    RobotMergeResult result;

    if (hodPath.empty()) {
        result.error = "HOD path is empty";
        return result;
    }
    if (outputDir.empty()) {
        result.error = "Output directory is empty";
        return result;
    }

    fs::path hodFsPath(hodPath);
    std::string stem = hodFsPath.stem().string();
    result.stem = stem;

    std::string robotOutDir = outputDir;
    fs::create_directories(robotOutDir);
    std::string hodDir = hodFsPath.parent_path().string();

    // ── 1. 解析 HOD ──
    if (callback)
        callback(0, 0, "[robot] Parsing HOD: " + hodPath);
    HODParser hodParser;
    if (!hodParser.ParseFile(hodPath)) {
        result.error = "HOD parse failed: " + hodParser.GetError();
        return result;
    }
    const auto &hod = hodParser.GetResult();

    // ── 2. 筛选部件 + assimp 解析 ──
    std::vector<PartData> parts;
    for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
        const auto &bone = hod.bones[bi];
        if (!IsRenderBone(bone.name))
            continue;

        std::string xPath = hodDir + "/" + bone.name;
        if (!fs::exists(xPath))
            continue;

        XFileParser parser;
        if (!parser.ParseFile(xPath))
            continue;

        const auto &meshes = parser.GetMeshes();
        if (meshes.empty())
            continue;

        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            // 跳过 assimp 拆分出的 _mShape 网格
            if (meshes[mi].name.find("_mShape") != std::string::npos)
                continue;

            // 同骨格合并（assimp 按 material 拆分）
            bool merged = false;
            for (auto &pd : parts) {
                if (pd.boneIndex == bi) {
                    auto &dst = pd.mesh;
                    auto &src = meshes[mi];
                    uint32_t vOff = static_cast<uint32_t>(dst.VertexCount());
                    for (auto v : src.positions)
                        dst.positions.push_back(v);
                    for (auto v : src.normals)
                        dst.normals.push_back(v);
                    for (auto v : src.texcoords)
                        dst.texcoords.push_back(v);
                    for (auto idx : src.indices)
                        dst.indices.push_back(idx + vOff);
                    dst.ComputeBounds();
                    merged = true;
                    break;
                }
            }
            if (merged)
                continue;

            PartData pd;
            pd.name = bone.name;
            pd.boneIndex = bi;
            pd.originalBoneIndex = bi;
            pd.mesh = meshes[mi];
            parts.push_back(std::move(pd));
        }
    }

    if (parts.empty()) {
        result.error = "No renderable parts found";
        return result;
    }
    // ── 3. LR 交换（交换完整部分数据：名字+网格，不交换 boneIndex/矩阵） ──
    if (options.lrSwap) {
        auto swapLR = [&](const std::string &l, const std::string &r) {
            int li = -1, ri = -1;
            for (size_t i = 0; i < parts.size(); ++i) {
                std::string nm = fs::path(parts[i].name).stem().string();
                if (nm == l)
                    li = (int)i;
                if (nm == r)
                    ri = (int)i;
            }
            if (li >= 0 && ri >= 0) {
                std::swap(parts[li].name, parts[ri].name);
                std::swap(parts[li].mesh, parts[ri].mesh);
            }
        };
        swapLR("arm1", "arm1_r");
        swapLR("arm2", "arm2_r");
        swapLR("Hand", "Hand_r");
        swapLR("leg1", "leg1_r");
        swapLR("leg2", "leg2_r");
        swapLR("leg3", "leg3_r");
    }

    // ── 4. 计算骨骼世界矩阵 ──
    std::vector<Mat4x4> boneWorld(hod.BoneCount());
    for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
        Mat4x4 local;
        for (int j = 0; j < 16; ++j)
            local.m[j] = static_cast<float>(hod.bones[bi].transform[j]);

        // Body_d 偏移修正：社区工具 Y=0.062，文件存储 Y=1.362
        if (hod.bones[bi].name == "Body_d.x")
            local.m[13] -= 1.30f;

        // 层级累乘（行向量 local * parent）
        if (hod.bones[bi].parentIndex >= 0)
            boneWorld[bi] = local * boneWorld[hod.bones[bi].parentIndex];
        else
            boneWorld[bi] = local;
    }

    // Ry(180°) 世界翻转
    for (auto &bw : boneWorld) {
        for (int j = 0; j < 4; ++j)
            bw.m[j] = -bw.m[j];
        for (int j = 8; j < 12; ++j)
            bw.m[j] = -bw.m[j];
    }

    // ── 5. .x 嵌套层级导出（从 boneWorld 推导正确局部矩阵 + 骨骼树） ──
    if (options.exportX) {
        std::string xNestPath = robotOutDir + "/" + stem + "_nested.x";
        std::ofstream ofsNest(xNestPath);
        if (ofsNest) {
            std::map<int, const PartData *> bonePartMap;
            for (const auto &p : parts)
                bonePartMap[p.boneIndex] = &p;
            // 嵌套版直接继承全局 LR 交换（已交换 parts[].boneIndex）

            ofsNest << "xof 0302txt 0032\n\n";

            // 递归嵌套：局部矩阵 = this_world × inverse(parent_world)（行主序 post-multiply）
            const auto &xW = boneWorld; // 含 Ry180，正确朝向
            std::function<void(int, int)> writeNBone = [&](int bi, int depth) {
                std::string in(depth > 0 ? depth * 2 : 0, ' ');
                std::string nm = fs::path(hod.bones[bi].name).stem().string();
                ofsNest << in << "Frame " << nm << " {\n";

                Mat4x4 mL;
                if (hod.bones[bi].parentIndex >= 0) {
                    Mat4x4 iP = xW[hod.bones[bi].parentIndex].Inverse();
                    mL = xW[bi] * iP; // this × inv(parent)
                } else {
                    mL = xW[bi];
                }
                ofsNest << in << "  FrameTransformMatrix {\n";
                for (int r = 0; r < 4; ++r) {
                    ofsNest << in << "    ";
                    for (int c = 0; c < 4; ++c)
                        ofsNest << (c > 0 ? "," : "") << mL.m[r * 4 + c];
                    ofsNest << (r < 3 ? ",\n" : ";;\n");
                }
                ofsNest << in << "  }\n";

                auto it = bonePartMap.find(bi);
                if (it != bonePartMap.end()) {
                    const auto &ms = it->second->mesh;
                    uint32_t vc = (uint32_t)ms.VertexCount(), fc = (uint32_t)ms.indices.size() / 3;
                    ofsNest << in << "  Mesh {\n";
                    ofsNest << in << "    " << vc << ";\n" << in << "    ";
                    for (uint32_t vi = 0; vi < vc; ++vi)
                        ofsNest << ms.positions[vi * 3] << ";" << ms.positions[vi * 3 + 1] << ";"
                                << ms.positions[vi * 3 + 2] << ";,";
                    ofsNest.seekp(-1, std::ios::cur);
                    ofsNest << ";\n";
                    ofsNest << in << "    " << fc << ";\n" << in << "    ";
                    for (uint32_t fi = 0; fi < fc; ++fi)
                        ofsNest << "3;" << ms.indices[fi * 3] << ";" << ms.indices[fi * 3 + 1] << ";"
                                << ms.indices[fi * 3 + 2] << ";,\n";
                    ofsNest.seekp(-1, std::ios::cur);
                    ofsNest << ";\n";
                    if (ms.HasNormals()) {
                        ofsNest << in << "    MeshNormals {\n";
                        ofsNest << in << "      " << vc << ";\n" << in << "      ";
                        for (uint32_t vi = 0; vi < vc; ++vi)
                            ofsNest << ms.normals[vi * 3] << ";" << ms.normals[vi * 3 + 1] << ";"
                                    << ms.normals[vi * 3 + 2] << ";,";
                        ofsNest.seekp(-1, std::ios::cur);
                        ofsNest << ";\n";
                        ofsNest << in << "      " << fc << ";\n" << in << "      ";
                        for (uint32_t fi = 0; fi < fc; ++fi)
                            ofsNest << "3;" << ms.indices[fi * 3] << ";" << ms.indices[fi * 3 + 1] << ";"
                                    << ms.indices[fi * 3 + 2] << ";,\n";
                        ofsNest.seekp(-1, std::ios::cur);
                        ofsNest << ";\n";
                        ofsNest << in << "    }\n";
                    }
                    if (ms.HasTexcoords()) {
                        ofsNest << in << "    MeshTextureCoords {\n";
                        ofsNest << in << "      " << vc << ";\n" << in << "      ";
                        for (uint32_t vi = 0; vi < vc; ++vi)
                            ofsNest << ms.texcoords[vi * 2] << ";" << ms.texcoords[vi * 2 + 1] << ";,";
                        ofsNest.seekp(-1, std::ios::cur);
                        ofsNest << ";\n";
                        ofsNest << in << "    }\n";
                    }
                    ofsNest << in << "  }\n";
                }

                for (uint32_t ci : hod.bones[bi].children)
                    writeNBone((int)ci, depth + 1);
                ofsNest << in << "}\n\n";
            };
            for (int bi = 0; bi < (int)hod.BoneCount(); ++bi)
                if (hod.bones[bi].parentIndex < 0)
                    writeNBone(bi, 0);
        }
        result.outputFiles.push_back(xNestPath);
    }

    // ── 6. 导出 FBX（含骨骼层级 + 蒙皮绑定） ──
    if (options.exportFBX) {
        std::cout << "[fbx] Building scene..." << std::endl;
        if (callback)
            callback(0, 0, "[robot] Exporting FBX...");
        aiScene *scene = new aiScene();
        scene->mRootNode = new aiNode("root");
        std::vector<aiNode *> boneNodes(hod.BoneCount(), nullptr);

        // 6a. 为可渲染骨骼创建 aiNode，构建层级（排除武器/喷射口等无实体骨骼）
        {
            // 收集有网格的骨骼索引
            std::set<int> renderBones;
            for (const auto &p : parts)
                if (p.boneIndex >= 0)
                    renderBones.insert(p.boneIndex);
            // 向上追溯祖先，构建完整可渲染骨骼集
            std::set<int> allRenderBones = renderBones;
            std::function<void(int)> addAncestors = [&](int bi) {
                int pi = hod.bones[bi].parentIndex;
                if (pi >= 0 && allRenderBones.find(pi) == allRenderBones.end()) {
                    allRenderBones.insert(pi);
                    addAncestors(pi);
                }
            };
            for (int bi : renderBones)
                addAncestors(bi);
            // 排除 "root" 骨骼（顶层空骨架，所有骨骼都有的公共祖先）
            for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
                std::string nm;
                for (char c : hod.bones[bi].name) nm += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (nm.find("root") != std::string::npos)
                    allRenderBones.erase(bi);
            }

            std::cout << "[fbx] Creating " << allRenderBones.size() << "/" << hod.BoneCount()
                      << " bone nodes (filtered)" << std::endl;

            for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
                if (allRenderBones.find(bi) == allRenderBones.end())
                    continue; // 跳过非渲染骨骼

                aiNode *node = new aiNode(hod.bones[bi].name);
                boneNodes[bi] = node;

                // 计算 FBX 局部矩阵
                // 先找到实际挂载的父骨骼（跳过被过滤的中间骨骼）
                int mountParent = hod.bones[bi].parentIndex;
                while (mountParent >= 0 && allRenderBones.find(mountParent) == allRenderBones.end())
                    mountParent = hod.bones[mountParent].parentIndex;

                Mat4x4 fbxLocal;
                if (mountParent >= 0) {
                    // 相对于实际挂载父骨骼的偏移
                    Mat4x4 invParent = boneWorld[mountParent].Inverse();
                    fbxLocal = boneWorld[bi] * invParent;
                } else {
                    // 挂在场景根节点下，用世界矩阵
                    fbxLocal = boneWorld[bi];
                }

                // assimp 列主序，转置行主序 m → 列主序 aiMatrix4x4
                node->mTransformation.a1 = fbxLocal.m[0];  node->mTransformation.a2 = fbxLocal.m[4];
                node->mTransformation.a3 = fbxLocal.m[8];  node->mTransformation.a4 = fbxLocal.m[12];
                node->mTransformation.b1 = fbxLocal.m[1];  node->mTransformation.b2 = fbxLocal.m[5];
                node->mTransformation.b3 = fbxLocal.m[9];  node->mTransformation.b4 = fbxLocal.m[13];
                node->mTransformation.c1 = fbxLocal.m[2];  node->mTransformation.c2 = fbxLocal.m[6];
                node->mTransformation.c3 = fbxLocal.m[10]; node->mTransformation.c4 = fbxLocal.m[14];
                node->mTransformation.d1 = fbxLocal.m[3];  node->mTransformation.d2 = fbxLocal.m[7];
                node->mTransformation.d3 = fbxLocal.m[11]; node->mTransformation.d4 = fbxLocal.m[15];

                // 挂接到最近的渲染父节点
                int parent = hod.bones[bi].parentIndex;
                while (parent >= 0 && allRenderBones.find(parent) == allRenderBones.end())
                    parent = hod.bones[parent].parentIndex;
                if (parent >= 0)
                    boneNodes[parent]->addChildren(1, &node);
                else
                    scene->mRootNode->addChildren(1, &node);
            }
        }

        // 6b. 为每个部件创建 aiMesh（局部坐标 + 蒙皮绑定到骨骼节点）
        scene->mNumMeshes = static_cast<unsigned int>(parts.size());
        scene->mMeshes = new aiMesh *[scene->mNumMeshes];
        scene->mNumMaterials = static_cast<unsigned int>(parts.size());
        scene->mMaterials = new aiMaterial *[scene->mNumMaterials];

        for (size_t pi = 0; pi < parts.size(); ++pi) {
            const auto &part = parts[pi];
            const auto &ms = part.mesh;
            uint32_t vc = static_cast<uint32_t>(ms.VertexCount());
            uint32_t ic = static_cast<uint32_t>(ms.indices.size());
            uint32_t fc = ic / 3;

            aiMesh *mesh = new aiMesh();
            std::string meshName = fs::path(part.name).stem().string();
            mesh->mName = meshName;
            mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
            mesh->mNumVertices = vc;
            mesh->mVertices = new aiVector3D[vc];
            mesh->mNormals = ms.HasNormals() ? new aiVector3D[vc] : nullptr;
            if (ms.HasTexcoords()) {
                mesh->mTextureCoords[0] = new aiVector3D[vc];
                mesh->mNumUVComponents[0] = 2;
            }

            // 局部坐标（不解烘焙，由蒙皮驱动）
            for (uint32_t vi = 0; vi < vc; ++vi) {
                mesh->mVertices[vi] = aiVector3D(
                    ms.positions[vi * 3 + 0],
                    ms.positions[vi * 3 + 1],
                    ms.positions[vi * 3 + 2]);
                if (ms.HasNormals()) {
                    mesh->mNormals[vi] = aiVector3D(
                        ms.normals[vi * 3 + 0],
                        ms.normals[vi * 3 + 1],
                        ms.normals[vi * 3 + 2]);
                }
                if (ms.HasTexcoords())
                    mesh->mTextureCoords[0][vi] = aiVector3D(
                        ms.texcoords[vi * 2 + 0],
                        ms.texcoords[vi * 2 + 1], 0);
            }

            // 刚性蒙皮绑定：1 根骨骼，weight=1.0
            if (part.boneIndex >= 0) {
                uint8_t bi = static_cast<uint8_t>(part.boneIndex);
                mesh->mNumBones = 1;
                mesh->mBones = new aiBone *[1];
                aiBone *bone = new aiBone();
                bone->mName = hod.bones[bi].name;
                bone->mNumWeights = vc;
                bone->mWeights = new aiVertexWeight[vc];
                for (uint32_t vi = 0; vi < vc; ++vi) {
                    bone->mWeights[vi].mVertexId = vi;
                    bone->mWeights[vi].mWeight = 1.0f;
                }
                // offsetMatrix = inverse(boneWorld) 将顶点从绑定姿势变换到骨空间
                Mat4x4 invBind = boneWorld[bi].Inverse();
                bone->mOffsetMatrix.a1 = invBind.m[0]; bone->mOffsetMatrix.a2 = invBind.m[4];
                bone->mOffsetMatrix.a3 = invBind.m[8]; bone->mOffsetMatrix.a4 = invBind.m[12];
                bone->mOffsetMatrix.b1 = invBind.m[1]; bone->mOffsetMatrix.b2 = invBind.m[5];
                bone->mOffsetMatrix.b3 = invBind.m[9]; bone->mOffsetMatrix.b4 = invBind.m[13];
                bone->mOffsetMatrix.c1 = invBind.m[2]; bone->mOffsetMatrix.c2 = invBind.m[6];
                bone->mOffsetMatrix.c3 = invBind.m[10]; bone->mOffsetMatrix.c4 = invBind.m[14];
                bone->mOffsetMatrix.d1 = invBind.m[3]; bone->mOffsetMatrix.d2 = invBind.m[7];
                bone->mOffsetMatrix.d3 = invBind.m[11]; bone->mOffsetMatrix.d4 = invBind.m[15];
                mesh->mBones[0] = bone;

                // 网格挂载到骨骼节点的子节点：骨骼节点保持无 mesh → FBX 导出为 Null 类型
                // （Blender 识别骨架的必要条件；与动画 FBX 导出一致）
                aiNode *meshNode = new aiNode(meshName);
                meshNode->mNumMeshes = 1;
                meshNode->mMeshes = new unsigned int[1]{static_cast<unsigned int>(pi)};
                if (boneNodes[bi])
                    boneNodes[bi]->addChildren(1, &meshNode);
                else
                    scene->mRootNode->addChildren(1, &meshNode);
            }

            // 索引（三角形）
            mesh->mNumFaces = fc;
            mesh->mFaces = new aiFace[fc];
            for (uint32_t fi = 0; fi < fc; ++fi) {
                mesh->mFaces[fi].mNumIndices = 3;
                mesh->mFaces[fi].mIndices = new unsigned int[3];
                mesh->mFaces[fi].mIndices[0] = ms.indices[fi * 3 + 0];
                mesh->mFaces[fi].mIndices[1] = ms.indices[fi * 3 + 1];
                mesh->mFaces[fi].mIndices[2] = ms.indices[fi * 3 + 2];
            }

            mesh->mMaterialIndex = static_cast<unsigned int>(pi);
            scene->mMeshes[pi] = mesh;

            // 材质（仅颜色，无纹理）
            aiMaterial *mat = new aiMaterial();
            aiString matNameStr(stem + "_" + meshName + "_mat");
            mat->AddProperty(&matNameStr, AI_MATKEY_NAME);
            const auto &xf = ms.material;
            aiColor4D diffuse(xf.faceColor[1], xf.faceColor[2], xf.faceColor[3], xf.faceColor[0]);
            mat->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
            float shininess = (xf.power > 0.0f) ? xf.power : 10.0f;
            mat->AddProperty(&shininess, 1, AI_MATKEY_SHININESS);
            scene->mMaterials[pi] = mat;
        }

        // 6c. 导出所有格式（glTF 主流可靠，FBX 备选）
        struct {
            const char *id;
            const char *ext;
        } formats[] = {
            {"fbx", ".fbx"},
        };
        int exportedCount = 0;
        for (auto &fmt : formats) {
            std::string path = robotOutDir + "/" + stem + fmt.ext;
            Assimp::Exporter exporter;
            if (exporter.Export(scene, fmt.id, path) == AI_SUCCESS) {
                result.outputFiles.push_back(path);
                exportedCount++;
                std::cout << "[fbx] Exported (" << fmt.id << "): " << path << std::endl;
            } else {
                std::cout << "[fbx] " << fmt.id << " failed: " << exporter.GetErrorString() << std::endl;
            }
        }
        if (exportedCount == 0)
            std::cout << "[fbx] All export formats failed" << std::endl;

        delete scene; // assimp 接管所有权后 delete
    }

    // ── 7. 合并网格 ──
    size_t totalVerts = 0, totalIndices = 0;
    for (const auto &p : parts) {
        totalVerts += p.mesh.VertexCount();
        totalIndices += p.mesh.indices.size();
    }

    // ── 7. 合并网格（DxMeshSkinnedVertex 蒙皮格式，局部坐标 + 骨骼绑定） ──
    std::vector<DxMeshSkinnedVertex> allVerts;
    allVerts.reserve(totalVerts);
    std::vector<uint32_t> allIndices;
    allIndices.reserve(totalIndices);
    std::vector<DxMeshSubMesh> subMeshes;
    subMeshes.reserve(parts.size());

    float bMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float bMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    uint32_t vOffset = 0;

    for (const auto &p : parts) {
        const auto &ms = p.mesh;
        uint32_t vc = static_cast<uint32_t>(ms.VertexCount());
        uint32_t ic = static_cast<uint32_t>(ms.indices.size());
        uint8_t boneIdx = (p.boneIndex >= 0 && p.boneIndex < 256) ? (uint8_t)p.boneIndex : 0;

        for (uint32_t i = 0; i < vc; ++i) {
            DxMeshSkinnedVertex v = {};
            v.position[0] = ms.positions[i * 3 + 0];
            v.position[1] = ms.positions[i * 3 + 1];
            v.position[2] = ms.positions[i * 3 + 2];
            // 不解烘焙 - 保持局部坐标，由蒙皮驱动
            if (ms.HasNormals()) {
                v.normal[0] = ms.normals[i * 3 + 0];
                v.normal[1] = ms.normals[i * 3 + 1];
                v.normal[2] = ms.normals[i * 3 + 2];
            } else {
                v.normal[0] = 0;
                v.normal[1] = 1;
                v.normal[2] = 0;
            }
            v.tangentU[0] = 1;
            v.tangentU[1] = 0;
            v.tangentU[2] = 0;
            if (ms.HasTexcoords()) {
                v.texC[0] = ms.texcoords[i * 2 + 0];
                v.texC[1] = ms.texcoords[i * 2 + 1];
            }
            // 刚性绑定：1 根骨骼，weight=1.0
            v.boneWeights[0] = 1.0f;
            v.boneIndices[0] = boneIdx;

            // 右手 Y-up → 左手 Y-up（翻转 Z）
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

        uint32_t sIdx = static_cast<uint32_t>(allIndices.size());
        for (uint32_t i = 0; i < ic; ++i)
            allIndices.push_back(ms.indices[i] + vOffset);

        DxMeshSubMesh sm = {};
        sm.indexOffset = sIdx;
        sm.indexCount = ic;
        sm.vertexOffset = vOffset;
        subMeshes.push_back(sm);
        vOffset += vc;
    }

    // ── 7. 写入 .dxmesh ──
    std::string dxmeshPath = robotOutDir + "/" + stem + ".dxmesh";
    if (!DX12Engine::Asset::DxMeshWriter::Write(
            allVerts.data(), allVerts.size(), sizeof(DxMeshSkinnedVertex), allIndices.data(), allIndices.size(), 4,
            std::wstring(dxmeshPath.begin(), dxmeshPath.end()), bMin, bMax, true, // skinned = true
            subMeshes.data(), static_cast<uint32_t>(subMeshes.size()))) {
        result.error = "Failed to write .dxmesh: " + dxmeshPath;
        return result;
    }
    result.outputFiles.push_back(dxmeshPath);

    // ── 8. 复制原始 .hod ──
    try {
        fs::copy_file(hodPath, fs::path(robotOutDir) / hodFsPath.filename(), fs::copy_options::overwrite_existing);
    } catch (...) {
    }
    result.outputFiles.push_back(robotOutDir + "/" + hodFsPath.filename().string());

    // ── 9. 写入 hod.json ──
    std::string jsonPath = robotOutDir + "/" + stem + ".hod.json";
    if (!hod.WriteJSON(jsonPath)) {
        result.error = "Failed to write hod.json";
        return result;
    }
    // 左手系：骨骼矩阵 Z 列取反（与 dxmesh 顶点转换同步）
    if (options.leftHanded) {
        std::ifstream ifs(jsonPath);
        if (ifs) {
            auto j = nlohmann::json::parse(ifs);
            for (auto &jb : j["bones"]) {
                auto &pos = jb["position"];
                pos[2] = -pos[2].get<double>();     // position.z
                auto &rot = jb["rotation"];
                rot[2] = -rot[2].get<double>();     // rotation.z
                for (auto &row : jb["matrix"])
                    row[2] = -row[2].get<double>(); // matrix Z column
            }
            std::ofstream ofs(jsonPath);
            ofs << j.dump(2);
        }
    }
    result.outputFiles.push_back(jsonPath);

    // ── 9b. 写入 .bone（引擎正式骨架资产，左手系 Y-up） ──
    // 与 hod.json 的差异：hod.json 是 HOD 原始 TRS（Z-up 预览用，仅 Z 翻转）；
    // .bone 应用完整变换（Body_d 修正 + 层级累乘 + Ry180° + 左手系），
    // 骨骼树与 .x/FBX 导出一致，供 SkeletonManager::LoadFromJSON 直接使用。
    // name 保留 HOD 原部件名（含 .x 扩展名），与 ANI 动画通道名一一对应。
    {
        std::string bonePath = robotOutDir + "/" + stem + ".bone";

        // 左手系：boneWorld（含 Ry180）Z 列取反 → 世界空间左手 Y-up（与 dxmesh 顶点翻转一致）
        std::vector<Mat4x4> boneWorldLH(hod.BoneCount());
        for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
            boneWorldLH[bi] = boneWorld[bi];
            if (options.leftHanded) {
                boneWorldLH[bi].m[2] = -boneWorldLH[bi].m[2];  // column Z row0
                boneWorldLH[bi].m[6] = -boneWorldLH[bi].m[6];  // column Z row1
                boneWorldLH[bi].m[10] = -boneWorldLH[bi].m[10]; // column Z row2
                boneWorldLH[bi].m[14] = -boneWorldLH[bi].m[14]; // column Z row3
            }
        }

        nlohmann::json jBoneRoot;
        jBoneRoot["version"] = 1;
        auto &jBones = jBoneRoot["bones"] = nlohmann::json::array();

        for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
            const auto &bone = hod.bones[bi];
            nlohmann::json jBone;
            jBone["name"] = bone.name;
            jBone["parentIndex"] = bone.parentIndex;

            // 局部矩阵 = world[child] × inverse(world[parent])（行主序 post-multiply）
            Mat4x4 mL;
            if (bone.parentIndex >= 0)
                mL = boneWorldLH[bi] * boneWorldLH[bone.parentIndex].Inverse();
            else
                mL = boneWorldLH[bi];

            // TRS 分解（XMMatrixDecompose 按 DirectXMath 行主序约定，从第 4 行提取平移，
            // 不能转置——转置会把平移移到第 4 列，导致 position 恒为 0）
            DirectX::XMFLOAT4X4 m4x4;
            memcpy(&m4x4, mL.m, sizeof(float) * 16);
            DirectX::XMMATRIX mat = DirectX::XMLoadFloat4x4(&m4x4);
            DirectX::XMVECTOR s, r, t;
            if (DirectX::XMMatrixDecompose(&s, &r, &t, mat)) {
                DirectX::XMFLOAT3 sf, tf;
                DirectX::XMFLOAT4 rf;
                DirectX::XMStoreFloat3(&sf, s);
                DirectX::XMStoreFloat3(&tf, t);
                DirectX::XMStoreFloat4(&rf, r);
                jBone["position"] = {tf.x, tf.y, tf.z};
                jBone["rotation"] = {rf.x, rf.y, rf.z, rf.w};
                jBone["scale"] = {sf.x, sf.y, sf.z};
            } else {
                // 分解失败（含镜像矩阵），回退到平移 + 恒等旋转/缩放
                jBone["position"] = {mL.m[12], mL.m[13], mL.m[14]};
                jBone["rotation"] = {0.0f, 0.0f, 0.0f, 1.0f};
                jBone["scale"] = {1.0f, 1.0f, 1.0f};
            }
            jBones.push_back(jBone);
        }

        std::ofstream ofs(bonePath);
        ofs << jBoneRoot.dump(2);
        result.outputFiles.push_back(bonePath);
    }

    // ── 10. 输出材质 + 纹理 ──
    fs::path outMatsDir = fs::path(robotOutDir) / "Materials";
    fs::path outTexDir = fs::path(robotOutDir) / "Textures";
    fs::create_directories(outMatsDir);
    fs::create_directories(outTexDir);

    for (size_t i = 0; i < parts.size(); ++i) {
        const auto &part = parts[i];
        const auto &xMat = part.mesh.material;
        std::string matKey = stem + "_" + fs::path(part.name).stem().string();
        result.materialKeys.push_back(matKey);

        // 纹理查找
        std::string texRef;
        if (!xMat.textureFilename.empty()) {
            auto findTex = [&](const std::string &fname) -> std::string {
                fs::path texPath = fs::path(hodDir) / fname;
                if (fs::exists(texPath)) {
                    try {
                        fs::copy_file(texPath, outTexDir / texPath.filename(), fs::copy_options::overwrite_existing);
                    } catch (...) {
                    }
                    std::string ext = texPath.extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".png") {
                        std::string ddsName = texPath.stem().string() + ".dds";
                        auto r = ConvertPNGToDDS(texPath.string(), (outTexDir / ddsName).string());
                        if (r.success)
                            return ddsName;
                    }
                    return texPath.filename().string();
                }
                return "";
            };
            texRef = findTex(xMat.textureFilename);
            if (texRef.empty()) {
                std::string altName = "IMG" + fs::path(xMat.textureFilename).stem().string() +
                                      fs::path(xMat.textureFilename).extension().string();
                texRef = findTex(altName);
            }
            if (texRef.empty()) {
                for (const auto &fe : fs::directory_iterator(hodDir)) {
                    if (!fe.is_regular_file())
                        continue;
                    std::string fext = fe.path().extension().string();
                    std::transform(fext.begin(), fext.end(), fext.begin(), ::tolower);
                    if (fext == ".png" || fext == ".dds" || fext == ".bmp") {
                        std::string fstem = fe.path().stem().string();
                        if (fstem.find("face") != std::string::npos)
                            continue;
                        if (fstem.find("select") != std::string::npos)
                            continue;
                        texRef = findTex(fe.path().filename().string());
                        if (!texRef.empty())
                            break;
                    }
                }
            }
        }

        // 输出 .mat
        auto matDesc = xMat.ToMaterialDesc();
        nlohmann::json jm;
        jm["shader"] = matDesc.shader;
        jm["params"]["baseColor"] = {matDesc.params.baseColor[0], matDesc.params.baseColor[1],
                                     matDesc.params.baseColor[2], matDesc.params.baseColor[3]};
        jm["params"]["metallic"] = matDesc.params.metallic;
        jm["params"]["roughness"] = matDesc.params.roughness;
        jm["params"]["ao"] = matDesc.params.ao;
        if (!texRef.empty()) {
            jm["textures"]["baseColor"] = fs::path(texRef).stem().string();
        }
        std::ofstream mf((outMatsDir / (matKey + ".mat")).string());
        if (mf)
            mf << jm.dump(2);
    }

    // ── 11. 写入 scene.json ──
    std::string scenePath = robotOutDir + "/" + stem + ".scene.json";
    {
        nlohmann::json scene;
        scene["version"] = 1;
        scene["metadata"]["name"] = stem;

        nlohmann::json deps = nlohmann::json::object();
        nlohmann::json materials = nlohmann::json::array();

        for (size_t i = 0; i < parts.size(); ++i) {
            const auto &part = parts[i];
            const auto &xMat = part.mesh.material;
            std::string matKey = stem + "_" + fs::path(part.name).stem().string();
            materials.push_back(matKey);
            deps[matKey] = nlohmann::json::object();
            deps[matKey]["type"] = "material";
            deps[matKey]["faceColor"] = {xMat.faceColor[1], xMat.faceColor[2], xMat.faceColor[3], xMat.faceColor[0]};
        }

        nlohmann::json entity;
        entity["name"] = stem;
        entity["components"]["transform"]["position"] = {0, 0, 0};
        entity["components"]["transform"]["rotation"] = {0, 0, 0, 1};
        entity["components"]["transform"]["scale"] = {1, 1, 1};
        entity["components"]["mesh"]["geometry"] = stem + ".dxmesh";
        entity["components"]["mesh"]["materials"] = materials;
        entity["components"]["skinned"]["skeleton"] = stem + ".hod.json";

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

    result.success = true;
    result.partCount = static_cast<int>(parts.size());
    result.vertexCount = static_cast<int>(totalVerts);
    result.indexCount = static_cast<int>(totalIndices);
    return result;
}

RobotMergeResult RobotMerger::MergeFromANI(const std::string &aniPath,
                                           const std::string &outputDir,
                                           const RobotMergeOptions &options) {
    RobotMergeResult result;

    // 调试日志（落盘到输出目录）
    std::ofstream mergeLog;
    try {
        mergeLog.open((fs::path(outputDir) / "ani_merge_debug.log").string(), std::ios::out | std::ios::trunc);
    } catch (...) {
    }
    auto log = [&](const std::string &msg) {
        if (mergeLog.is_open())
            mergeLog << msg << "\n";
    };

    log("=== ANI 合并调试日志 ===");
    log("ANI 路径: " + aniPath);
    log("输出目录: " + outputDir);

    if (aniPath.empty()) {
        result.error = "ANI path is empty";
        log("✗ " + result.error);
        return result;
    }
    if (outputDir.empty()) {
        result.error = "Output directory is empty";
        log("✗ " + result.error);
        return result;
    }

    // ── 1. 解析 ANI 母版骨架（部件名 + A/B 层级）──
    ANIParser ani;
    if (!ani.ParseFile(aniPath)) {
        result.error = "ANI parse failed: " + ani.GetError();
        log("✗ " + result.error);
        return result;
    }
    const auto &master = ani.GetMaster();
    log("✓ ANI 解析成功，诊断: " + ani.GetDiagnostics());
    log("  母版骨架: " + std::to_string(master.BoneCount()) + " 部件, isHd2=" +
        (master.isHd2 ? "是" : "否"));
    if (master.bones.empty()) {
        result.error = "ANI master skeleton is empty (no ANIRobo/AN2Robo master block)";
        log("✗ " + result.error);
        return result;
    }
    {
        std::string names;
        for (size_t i = 0; i < master.bones.size() && i < 8; ++i) {
            if (i) names += ", ";
            names += master.bones[i].name;
        }
        if (master.bones.size() > 8)
            names += ", ...";
        log("  部件名(前8): " + names);
    }

    // ── 2. 从 ANI 同目录找绑定矩阵 .hod（Robo.hod / robo.hod，排除 hangar）──
    fs::path aniFsPath(aniPath);
    std::string aniDir = aniFsPath.parent_path().string();
    std::string hodPath;
    for (const char *cand : {"Robo.hod", "robo.hod", "ROBO.HOD"}) {
        std::string p = aniDir + "/" + cand;
        if (fs::exists(p)) {
            hodPath = p;
            break;
        }
    }
    if (hodPath.empty()) {
        log("  首选 Robo.hod 未找到，扫描目录 .hod 文件:");
        for (auto &entry : fs::directory_iterator(aniDir)) {
            std::string fn = entry.path().filename().string();
            log("    候选: " + fn + " (ext=" + entry.path().extension().string() + ")");
            if (entry.path().extension() == ".hod" && fn.find("hangar") == std::string::npos &&
                fn.find("Hangar") == std::string::npos) {
                hodPath = entry.path().string();
                break;
            }
        }
    }
    if (hodPath.empty()) {
        result.error = "No Robo.hod found next to ANI for binding matrices";
        log("✗ " + result.error);
        return result;
    }
    log("✓ 绑定矩阵 HOD: " + hodPath);

    // ── 3. 校验：母版骨架部件数与 HOD 一致性（绑定矩阵以 HOD 为准）──
    HODParser hodParser;
    if (hodParser.ParseFile(hodPath)) {
        const auto &hod = hodParser.GetResult();
        log("✓ HOD 解析成功: " + std::to_string(hod.BoneCount()) + " 骨骼");
        if (hod.BoneCount() != master.BoneCount()) {
            result.error = "Master/HOD bone count mismatch: ANI=" + std::to_string(master.BoneCount()) +
                           " HOD=" + std::to_string(hod.BoneCount());
            log("⚠ " + result.error);
        } else {
            log("✓ 部件数一致");
        }
    } else {
        log("✗ HOD 解析失败: " + hodParser.GetError());
    }

    // ── 4. 复用标准合并（矩阵/部件/输出全部走 HOD 管线）──
    log("→ 开始标准合并...");
    RobotMergeResult merged = MergeWithCallback(hodPath, outputDir, options, nullptr);
    if (merged.success) {
        log("✓ 合并成功: 部件=" + std::to_string(merged.partCount) +
            " 顶点=" + std::to_string(merged.vertexCount) + " 索引=" + std::to_string(merged.indexCount));
        for (const auto &f : merged.outputFiles)
            log("  输出: " + f);
    } else {
        log("✗ 合并失败: " + merged.error);
    }
    if (mergeLog.is_open())
        mergeLog.close();
    return merged;
}

// ==========================================================================
// 动画 FBX 导出（B2.5）：ANI 各组帧数据 → aiNodeAnim 关键帧 → aiAnimation
//   1.008 帧 = 标准 HOD（HODParser 直解）；PUK 帧块 = HD2（19B 头 + 每部件
//   179B = 171B TRS + 8B A/B，TRS 布局：f0-3 quat(x,y,z,w) f4-6 scale f7-9 pos）
// ==========================================================================

/// 解析一帧动画数据 → 每骨骼局部矩阵（按 hod 骨骼索引对齐）
/// frameData: 帧原始字节（HOD 魔术起 9847B 或 HD2 魔术起帧块）
/// hod: 基准骨架（层级/命名）
/// outLocal: 输出每骨骼局部矩阵（TRS → 4×4）
static bool ParseFrameToLocalMatrices(const std::vector<uint8_t> &frameData, const HODData &hod,
                                      std::vector<RobotMerger::Mat4x4> &outLocal) {
    outLocal.assign(hod.BoneCount(), RobotMerger::Mat4x4::Identity());
    if (frameData.size() < 4)
        return false;

    const bool isHod = (frameData[0] == 'H' && frameData[1] == 'O' && frameData[2] == 'D');
    const bool isHd2 = (frameData[0] == 'H' && frameData[1] == 'D' && frameData[2] == '2');

    if (isHod) {
        // 1.008：标准 HOD，HODParser 直解
        HODParser frameParser;
        if (!frameParser.Parse(frameData.data(), frameData.size()))
            return false;
        const auto &frameHod = frameParser.GetResult();
        size_t n = std::min(frameHod.BoneCount(), hod.BoneCount());
        for (size_t bi = 0; bi < n; ++bi) {
            for (int j = 0; j < 16; ++j)
                outLocal[bi].m[j] = static_cast<float>(frameHod.bones[bi].transform[j]);
            // Body_d 偏移修正（与静态 HOD 组装 Merge 一致：帧数据 Y 同样含 +1.30 偏移）
            if (frameHod.bones[bi].name == "Body_d.x")
                outLocal[bi].m[13] -= 1.30f;
        }
        return true;
    }

    if (isHd2) {
        // PUK：HD2 帧块 = 19B 头（HD2+类型+部件数+0+1）+ 每部件 179B（171B TRS + 8B A/B）
        // TRS 布局：f0-3 = quat(x,y,z,w)，f4-6 = scale，f7-9 = position
        size_t pos = 19;
        for (size_t bi = 0; bi < hod.BoneCount(); ++bi) {
            if (pos + 179 > frameData.size())
                break;
            const uint8_t *p = frameData.data() + pos;
            float f[10];
            for (int i = 0; i < 10; ++i)
                std::memcpy(&f[i], p + i * 4, 4);

            // quat(x,y,z,w) → 旋转矩阵（行主序）
            float x = f[0], y = f[1], z = f[2], w = f[3];
            float sx = f[4], sy = f[5], sz = f[6];
            float px = f[7], py = f[8], pz = f[9];
            // 归一化四元数
            float len = std::sqrt(x * x + y * y + z * z + w * w);
            if (len > 1e-6f) { x /= len; y /= len; z /= len; w /= len; }
            float m[16] = {
                1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0,
                2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0,
                2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0,
                px, py, pz, 1
            };
            // scale
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    m[r * 4 + c] *= (c == 0 ? sx : (c == 1 ? sy : sz));
            for (int j = 0; j < 16; ++j)
                outLocal[bi].m[j] = m[j];
            // Body_d 偏移修正（与静态 HOD 组装 Merge 一致：帧数据 Y 同样含 +1.30 偏移）
            if (hod.bones[bi].name == "Body_d.x")
                outLocal[bi].m[13] -= 1.30f;
            pos += 179;
        }
        return true;
    }

    return false;
}

RobotMergeResult RobotMerger::ExportAnimationsFBX(const std::string &aniPath,
                                                   const std::string &outputDir,
                                                   const std::string &stem) {
    RobotMergeResult result;

    if (aniPath.empty()) {
        result.error = "ANI path is empty";
        return result;
    }
    if (outputDir.empty()) {
        result.error = "Output directory is empty";
        return result;
    }

    // ── 1. 解析 ANI（组 = 动作，Tail 标记划分边界）──
    ANIParser ani;
    if (!ani.ParseFile(aniPath)) {
        result.error = "ANI parse failed: " + ani.GetError();
        return result;
    }
    const auto &groups = ani.GetGroups();
    if (groups.empty()) {
        result.error = "ANI has no animation groups";
        return result;
    }

    // ── 2. 解析同目录 Robo.hod（骨骼层级/命名/绑定姿势基准）──
    fs::path aniFsPath(aniPath);
    std::string aniDir = aniFsPath.parent_path().string();
    std::string hodPath;
    for (const char *cand : {"Robo.hod", "robo.hod", "ROBO.HOD"}) {
        std::string p = aniDir + "/" + cand;
        if (fs::exists(p)) { hodPath = p; break; }
    }
    if (hodPath.empty()) {
        for (auto &entry : fs::directory_iterator(aniDir)) {
            std::string fn = entry.path().filename().string();
            if (entry.path().extension() == ".hod" && fn.find("hangar") == std::string::npos &&
                fn.find("Hangar") == std::string::npos) {
                hodPath = entry.path().string();
                break;
            }
        }
    }
    if (hodPath.empty()) {
        result.error = "No Robo.hod found next to ANI for skeleton hierarchy";
        return result;
    }
    HODParser hodParser;
    if (!hodParser.ParseFile(hodPath)) {
        result.error = "HOD parse failed: " + hodParser.GetError();
        return result;
    }
    const auto &hod = hodParser.GetResult();
    if (hod.BoneCount() == 0) {
        result.error = "HOD has no bones";
        return result;
    }

    // ── 3. 绑定姿势 boneWorld（基准：HOD 绑定矩阵，与静态 FBX 完全一致）──
    //    mesh 顶点坐标基于 HOD 绑定坐标系 → offsetMatrix/骨骼节点绑定姿势必须用
    //    inverse(HOD boneWorld)，否则顶点变换错位。Body_d -1.30 修正与静态导出一致。
    std::vector<Mat4x4> bindWorld(hod.BoneCount());
    for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
        Mat4x4 local;
        for (int j = 0; j < 16; ++j)
            local.m[j] = static_cast<float>(hod.bones[bi].transform[j]);
        if (hod.bones[bi].name == "Body_d.x")
            local.m[13] -= 1.30f;
        if (hod.bones[bi].parentIndex >= 0)
            bindWorld[bi] = local * bindWorld[hod.bones[bi].parentIndex];
        else
            bindWorld[bi] = local;
    }
    for (auto &bw : bindWorld) {
        for (int j = 0; j < 4; ++j) bw.m[j] = -bw.m[j];
        for (int j = 8; j < 12; ++j) bw.m[j] = -bw.m[j];
    }

    // ── 4. 构建 aiScene + 加载部件网格（IsRenderBone 过滤：排除武器/发射口/root/hit 等无实体骨骼）──
    aiScene *scene = new aiScene();
    scene->mRootNode = new aiNode("root");
    std::string hodDir = fs::path(hodPath).parent_path().string();
    std::vector<PartData> parts;
    for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
        const auto &bone = hod.bones[bi];
        if (!IsRenderBone(bone.name))
            continue;
        std::string xPath = hodDir + "/" + bone.name;
        if (!fs::exists(xPath))
            continue;
        XFileParser parser;
        if (!parser.ParseFile(xPath))
            continue;
        const auto &meshes = parser.GetMeshes();
        if (meshes.empty())
            continue;
        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            if (meshes[mi].name.find("_mShape") != std::string::npos)
                continue;
            bool merged = false;
            for (auto &pd : parts) {
                if (pd.boneIndex == bi) {
                    auto &dst = pd.mesh;
                    auto &src = meshes[mi];
                    uint32_t vOff = static_cast<uint32_t>(dst.VertexCount());
                    for (auto v : src.positions) dst.positions.push_back(v);
                    for (auto v : src.normals) dst.normals.push_back(v);
                    for (auto v : src.texcoords) dst.texcoords.push_back(v);
                    for (auto idx : src.indices) dst.indices.push_back(idx + vOff);
                    dst.ComputeBounds();
                    merged = true;
                    break;
                }
            }
            if (merged)
                continue;
            PartData pd;
            pd.name = bone.name;
            pd.boneIndex = bi;
            pd.originalBoneIndex = bi;
            pd.mesh = meshes[mi];
            parts.push_back(std::move(pd));
        }
    }
    if (parts.empty()) {
        result.error = "No renderable parts found next to ANI";
        delete scene;
        return result;
    }

    // LR 交换（交换完整部分数据：名字+网格，不交换 boneIndex/矩阵；与静态导出一致）
    {
        auto swapLR = [&](const std::string &l, const std::string &r) {
            int li = -1, ri = -1;
            for (size_t i = 0; i < parts.size(); ++i) {
                std::string nm = fs::path(parts[i].name).stem().string();
                if (nm == l)
                    li = (int)i;
                if (nm == r)
                    ri = (int)i;
            }
            if (li >= 0 && ri >= 0) {
                std::swap(parts[li].name, parts[ri].name);
                std::swap(parts[li].mesh, parts[ri].mesh);
            }
        };
        swapLR("arm1", "arm1_r");
        swapLR("arm2", "arm2_r");
        swapLR("Hand", "Hand_r");
        swapLR("leg1", "leg1_r");
        swapLR("leg2", "leg2_r");
        swapLR("leg3", "leg3_r");
    }

    // 过滤后骨骼集合：有网格的骨骼 + 祖先，排除 root（与静态 FBX 一致）
    std::set<int> renderBones;
    for (const auto &p : parts)
        if (p.boneIndex >= 0)
            renderBones.insert(p.boneIndex);
    std::set<int> allRenderBones = renderBones;
    std::function<void(int)> addAncestors = [&](int bi) {
        int pi = hod.bones[bi].parentIndex;
        if (pi >= 0 && allRenderBones.find(pi) == allRenderBones.end()) {
            allRenderBones.insert(pi);
            addAncestors(pi);
        }
    };
    for (int bi : renderBones)
        addAncestors(bi);
    for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
        std::string nm;
        for (char c : hod.bones[bi].name) nm += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (nm.find("root") != std::string::npos)
            allRenderBones.erase(bi);
    }

    // ── 5. 构建骨骼节点树（仅过滤后骨骼）──
    std::vector<aiNode *> boneNodes(hod.BoneCount(), nullptr);
    for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
        if (allRenderBones.find(bi) == allRenderBones.end())
            continue;
        aiNode *node = new aiNode(hod.bones[bi].name);
        boneNodes[bi] = node;

        // 向上追溯实际挂载父骨骼（跳过被过滤的中间骨骼/root，与静态 FBX 6a 一致）
        int mountParent = hod.bones[bi].parentIndex;
        while (mountParent >= 0 && allRenderBones.find(mountParent) == allRenderBones.end())
            mountParent = hod.bones[mountParent].parentIndex;

        Mat4x4 bindLocal;
        if (mountParent >= 0)
            bindLocal = bindWorld[bi] * bindWorld[mountParent].Inverse();
        else
            bindLocal = bindWorld[bi];
        node->mTransformation.a1 = bindLocal.m[0]; node->mTransformation.a2 = bindLocal.m[4];
        node->mTransformation.a3 = bindLocal.m[8]; node->mTransformation.a4 = bindLocal.m[12];
        node->mTransformation.b1 = bindLocal.m[1]; node->mTransformation.b2 = bindLocal.m[5];
        node->mTransformation.b3 = bindLocal.m[9]; node->mTransformation.b4 = bindLocal.m[13];
        node->mTransformation.c1 = bindLocal.m[2]; node->mTransformation.c2 = bindLocal.m[6];
        node->mTransformation.c3 = bindLocal.m[10]; node->mTransformation.c4 = bindLocal.m[14];
        node->mTransformation.d1 = bindLocal.m[3]; node->mTransformation.d2 = bindLocal.m[7];
        node->mTransformation.d3 = bindLocal.m[11]; node->mTransformation.d4 = bindLocal.m[15];
        int parent = mountParent;
        if (parent >= 0)
            boneNodes[parent]->addChildren(1, &node);
        else
            scene->mRootNode->addChildren(1, &node);
    }

    // ── 6. 构建 aiMesh（每个部件，蒙皮绑定）──
    scene->mNumMeshes = static_cast<unsigned int>(parts.size());
    scene->mMeshes = new aiMesh *[scene->mNumMeshes];
    scene->mNumMaterials = static_cast<unsigned int>(parts.size());
    scene->mMaterials = new aiMaterial *[scene->mNumMaterials];
    for (size_t pi = 0; pi < parts.size(); ++pi) {
        const auto &part = parts[pi];
        const auto &ms = part.mesh;
        uint32_t vc = static_cast<uint32_t>(ms.VertexCount());
        uint32_t ic = static_cast<uint32_t>(ms.indices.size());
        uint32_t fc = ic / 3;

        aiMesh *mesh = new aiMesh();
        mesh->mName = fs::path(part.name).stem().string();
        mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        mesh->mNumVertices = vc;
        mesh->mVertices = new aiVector3D[vc];
        mesh->mNormals = ms.HasNormals() ? new aiVector3D[vc] : nullptr;
        if (ms.HasTexcoords()) {
            mesh->mTextureCoords[0] = new aiVector3D[vc];
            mesh->mNumUVComponents[0] = 2;
        }
        for (uint32_t vi = 0; vi < vc; ++vi) {
            mesh->mVertices[vi] = aiVector3D(ms.positions[vi * 3 + 0], ms.positions[vi * 3 + 1], ms.positions[vi * 3 + 2]);
            if (ms.HasNormals())
                mesh->mNormals[vi] = aiVector3D(ms.normals[vi * 3 + 0], ms.normals[vi * 3 + 1], ms.normals[vi * 3 + 2]);
            if (ms.HasTexcoords())
                mesh->mTextureCoords[0][vi] = aiVector3D(ms.texcoords[vi * 2 + 0], ms.texcoords[vi * 2 + 1], 0);
        }
        // 刚性蒙皮绑定
        if (part.boneIndex >= 0) {
            uint8_t bi = static_cast<uint8_t>(part.boneIndex);
            mesh->mNumBones = 1;
            mesh->mBones = new aiBone *[1];
            aiBone *bone = new aiBone();
            bone->mName = hod.bones[bi].name;
            bone->mNumWeights = vc;
            bone->mWeights = new aiVertexWeight[vc];
            for (uint32_t vi = 0; vi < vc; ++vi) {
                bone->mWeights[vi].mVertexId = vi;
                bone->mWeights[vi].mWeight = 1.0f;
            }
            Mat4x4 invBind = bindWorld[bi].Inverse();
            bone->mOffsetMatrix.a1 = invBind.m[0]; bone->mOffsetMatrix.a2 = invBind.m[4];
            bone->mOffsetMatrix.a3 = invBind.m[8]; bone->mOffsetMatrix.a4 = invBind.m[12];
            bone->mOffsetMatrix.b1 = invBind.m[1]; bone->mOffsetMatrix.b2 = invBind.m[5];
            bone->mOffsetMatrix.b3 = invBind.m[9]; bone->mOffsetMatrix.b4 = invBind.m[13];
            bone->mOffsetMatrix.c1 = invBind.m[2]; bone->mOffsetMatrix.c2 = invBind.m[6];
            bone->mOffsetMatrix.c3 = invBind.m[10]; bone->mOffsetMatrix.c4 = invBind.m[14];
            bone->mOffsetMatrix.d1 = invBind.m[3]; bone->mOffsetMatrix.d2 = invBind.m[7];
            bone->mOffsetMatrix.d3 = invBind.m[11]; bone->mOffsetMatrix.d4 = invBind.m[15];
            mesh->mBones[0] = bone;
            // 网格挂载到骨骼节点的子节点：骨骼节点保持无 mesh → FBX 导出为 Null 类型
            // （Blender 识别骨架的必要条件；网格作为子节点跟随骨骼运动，刚性蒙皮绑定不变）
            aiNode *meshNode = new aiNode(mesh->mName.C_Str());
            meshNode->mNumMeshes = 1;
            meshNode->mMeshes = new unsigned int[1]{static_cast<unsigned int>(pi)};
            if (boneNodes[bi])
                boneNodes[bi]->addChildren(1, &meshNode);
            else
                scene->mRootNode->addChildren(1, &meshNode);
        }
        mesh->mNumFaces = fc;
        mesh->mFaces = new aiFace[fc];
        for (uint32_t fi = 0; fi < fc; ++fi) {
            mesh->mFaces[fi].mNumIndices = 3;
            mesh->mFaces[fi].mIndices = new unsigned int[3];
            mesh->mFaces[fi].mIndices[0] = ms.indices[fi * 3 + 0];
            mesh->mFaces[fi].mIndices[1] = ms.indices[fi * 3 + 1];
            mesh->mFaces[fi].mIndices[2] = ms.indices[fi * 3 + 2];
        }
        mesh->mMaterialIndex = static_cast<unsigned int>(pi);
        scene->mMeshes[pi] = mesh;

        aiMaterial *mat = new aiMaterial();
        aiString matNameStr(stem + "_" + fs::path(part.name).stem().string() + "_mat");
        mat->AddProperty(&matNameStr, AI_MATKEY_NAME);
        const auto &xf = ms.material;
        aiColor4D diffuse(xf.faceColor[1], xf.faceColor[2], xf.faceColor[3], xf.faceColor[0]);
        mat->AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
        float shininess = (xf.power > 0.0f) ? xf.power : 10.0f;
        mat->AddProperty(&shininess, 1, AI_MATKEY_SHININESS);
        scene->mMaterials[pi] = mat;
    }

    // ── 7. 每组 → aiAnimation（仅过滤后骨骼通道）──
    scene->mNumAnimations = static_cast<unsigned int>(groups.size());
    scene->mAnimations = new aiAnimation *[scene->mNumAnimations];

    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto &g = groups[gi];
        // 时间轴：Tail 头速度值 = 帧间隔秒数（docs：0.1f → 10 FPS）；
        // 范围校验：垃圾值（空 Tail 组/定位偏差读出 0 或 1e33）回退默认 30fps
        const double ticksPerSec = (g.tail.speed > 0.001f && g.tail.speed < 1.0f)
                                       ? (1.0 / g.tail.speed)
                                       : 30.0;
        aiAnimation *anim = new aiAnimation();
        std::string groupName;
        if (!g.frames.empty()) {
            groupName = fs::path(g.frames[0].name).stem().string();
            if (groupName.size() > 4 && groupName.substr(groupName.size() - 4) == ".hod")
                groupName = groupName.substr(0, groupName.size() - 4);
        }
        if (groupName.empty())
            groupName = "anim_" + std::to_string(gi + 1);
        // 组名加序号后缀保证唯一：多组首帧同名（如 guard01×4）会导致 Blender 合并 AnimStack，
        // 只剩一个动画组。唯一名 = 动作名 + 组序号。
        anim->mName = aiString(groupName + "_" + std::to_string(gi + 1));
        anim->mDuration = static_cast<double>(g.frames.size());
        anim->mTicksPerSecond = ticksPerSec;
        anim->mNumChannels = static_cast<unsigned int>(allRenderBones.size());
        anim->mChannels = new aiNodeAnim *[anim->mNumChannels];
        unsigned int chanIdx = 0;

        for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
            if (allRenderBones.find(bi) == allRenderBones.end())
                continue;
            aiNodeAnim *chan = new aiNodeAnim();
            chan->mNodeName = aiString(hod.bones[bi].name);
            chan->mNumPositionKeys = static_cast<unsigned int>(g.frames.size());
            chan->mPositionKeys = new aiVectorKey[chan->mNumPositionKeys];
            chan->mNumRotationKeys = static_cast<unsigned int>(g.frames.size());
            chan->mRotationKeys = new aiQuatKey[chan->mNumRotationKeys];
            chan->mNumScalingKeys = static_cast<unsigned int>(g.frames.size());
            chan->mScalingKeys = new aiVectorKey[chan->mNumScalingKeys];

            for (size_t fi = 0; fi < g.frames.size(); ++fi) {
                std::vector<Mat4x4> frameLocal;
                if (!ParseFrameToLocalMatrices(g.frames[fi].data, hod, frameLocal))
                    continue;
                if (bi >= static_cast<int>(frameLocal.size()))
                    continue;

                std::vector<Mat4x4> frameWorld(hod.BoneCount());
                for (int bj = 0; bj < static_cast<int>(hod.BoneCount()); ++bj) {
                    Mat4x4 local = (bj < static_cast<int>(frameLocal.size())) ? frameLocal[bj] : Mat4x4::Identity();
                    // 帧块数据 = 绝对姿势（与绑定姿势同坐标系，无 Body_d -1.30 修正）
                    if (hod.bones[bj].parentIndex >= 0)
                        frameWorld[bj] = local * frameWorld[hod.bones[bj].parentIndex];
                    else
                        frameWorld[bj] = local;
                }
                for (auto &fw : frameWorld) {
                    for (int j = 0; j < 4; ++j) fw.m[j] = -fw.m[j];
                    for (int j = 8; j < 12; ++j) fw.m[j] = -fw.m[j];
                }

                Mat4x4 fbxLocal;
                if (hod.bones[bi].parentIndex >= 0)
                    fbxLocal = frameWorld[bi] * frameWorld[hod.bones[bi].parentIndex].Inverse();
                else
                    fbxLocal = frameWorld[bi];

                DirectX::XMFLOAT4X4 m4x4;
                memcpy(&m4x4, fbxLocal.m, sizeof(float) * 16);
                DirectX::XMMATRIX mat = DirectX::XMLoadFloat4x4(&m4x4);
                DirectX::XMVECTOR s, r, t;
                DirectX::XMFLOAT3 sf{1, 1, 1}, tf{0, 0, 0};
                DirectX::XMFLOAT4 rf{0, 0, 0, 1};
                if (DirectX::XMMatrixDecompose(&s, &r, &t, mat)) {
                    DirectX::XMStoreFloat3(&sf, s);
                    DirectX::XMStoreFloat3(&tf, t);
                    DirectX::XMStoreFloat4(&rf, r);
                } else {
                    tf = {fbxLocal.m[12], fbxLocal.m[13], fbxLocal.m[14]};
                }
                double tsec = static_cast<double>(fi) / ticksPerSec;
                chan->mPositionKeys[fi] = aiVectorKey(tsec, aiVector3D(tf.x, tf.y, tf.z));
                chan->mRotationKeys[fi] = aiQuatKey(tsec, aiQuaternion(rf.w, rf.x, rf.y, rf.z));
                chan->mScalingKeys[fi] = aiVectorKey(tsec, aiVector3D(sf.x, sf.y, sf.z));
            }
            anim->mChannels[chanIdx++] = chan;
        }
        scene->mAnimations[gi] = anim;
    }

    // ── 8. 导出（ASCII FBX：web 工具兼容，二进制 7.5 多数在线查看器不支持）──
    std::string path = outputDir + "/" + stem + "_anim.fbx";
    Assimp::Exporter exporter;
    if (exporter.Export(scene, "fbxa", path) == AI_SUCCESS) {
        result.outputFiles.push_back(path);
        result.success = true;
        result.partCount = static_cast<int>(allRenderBones.size());
        result.vertexCount = static_cast<int>(parts.size());
        result.indexCount = 0;
        std::cout << "[fbx] Exported animation (ASCII): " << path << " (" << groups.size() << " clips, "
                  << allRenderBones.size() << " bones, " << parts.size() << " meshes)" << std::endl;

        // ── 8b. 附带动画帧矩阵 dump（同目录 txt，调试/对比用：逐帧逐骨骼局部矩阵）──
        {
            fs::path txtPath = fs::path(outputDir) / (stem + "_anim_frames.txt");
            std::ofstream ofs(txtPath);
            if (ofs) {
                ofs << "# " << stem << " 动画帧局部矩阵（world × inverse(parent)，行主序，供与社区工具对比）\n";
                for (size_t gi = 0; gi < groups.size(); ++gi) {
                    const auto &g = groups[gi];
                    std::string gn;
                    if (!g.frames.empty()) {
                        gn = fs::path(g.frames[0].name).stem().string();
                        if (gn.size() > 4 && gn.substr(gn.size() - 4) == ".hod")
                            gn = gn.substr(0, gn.size() - 4);
                    }
                    if (gn.empty())
                        gn = "anim_" + std::to_string(gi + 1);
                    double fps = (g.tail.speed > 0.001f && g.tail.speed < 1.0f)
                                     ? (1.0 / g.tail.speed)
                                     : 30.0;
                    ofs << "\n== group " << (gi + 1) << " [" << gn << "] frames=" << g.frames.size()
                        << " tailTime=" << g.tail.time << " tailSpeed=" << g.tail.speed
                        << " fps=" << fps << " ==\n";
                    for (size_t fi = 0; fi < g.frames.size(); ++fi) {
                        std::vector<Mat4x4> frameLocal;
                        if (!ParseFrameToLocalMatrices(g.frames[fi].data, hod, frameLocal))
                            continue;
                        std::vector<Mat4x4> frameWorld(hod.BoneCount());
                        for (int bj = 0; bj < static_cast<int>(hod.BoneCount()); ++bj) {
                            Mat4x4 local = (bj < static_cast<int>(frameLocal.size())) ? frameLocal[bj] : Mat4x4::Identity();
                            if (hod.bones[bj].parentIndex >= 0)
                                frameWorld[bj] = local * frameWorld[hod.bones[bj].parentIndex];
                            else
                                frameWorld[bj] = local;
                        }
                        for (auto &fw : frameWorld) {
                            for (int j = 0; j < 4; ++j) fw.m[j] = -fw.m[j];
                            for (int j = 8; j < 12; ++j) fw.m[j] = -fw.m[j];
                        }
                        ofs << "  frame " << fi << " (t=" << (fi / fps) << "s):\n";
                        for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
                            if (allRenderBones.find(bi) == allRenderBones.end())
                                continue;
                            Mat4x4 mL;
                            if (hod.bones[bi].parentIndex >= 0)
                                mL = frameWorld[bi] * frameWorld[hod.bones[bi].parentIndex].Inverse();
                            else
                                mL = frameWorld[bi];
                            ofs << "    " << hod.bones[bi].name << ":";
                            for (int j = 0; j < 16; ++j)
                                ofs << " " << mL.m[j];
                            ofs << "\n";
                        }
                    }
                }
                ofs.close();
                result.outputFiles.push_back(txtPath.string());
                std::cout << "[fbx] Frame matrix dump: " << txtPath << std::endl;
            }
        }
    } else {
        result.error = "FBX export failed: " + std::string(exporter.GetErrorString());
    }

    delete scene;
    return result;
}

// ==========================================================================
// 每帧 .x 导出（调试/观察用）：每帧帧矩阵组装嵌套 x（含网格）
//   输出 {outDir}/{组号}/{序号}_{帧名}.x —— 从 x 文件逐个观察 ANI 动作姿势
// ==========================================================================

RobotMergeResult RobotMerger::ExportAnimationFramesX(const std::string &aniPath,
                                                      const std::string &outputDir,
                                                      const std::string &stem) {
    RobotMergeResult result;

    if (aniPath.empty()) {
        result.error = "ANI path is empty";
        return result;
    }
    if (outputDir.empty()) {
        result.error = "Output directory is empty";
        return result;
    }

    // ── 1. 解析 ANI（组 = 动作，Tail 标记划分边界）──
    ANIParser ani;
    if (!ani.ParseFile(aniPath)) {
        result.error = "ANI parse failed: " + ani.GetError();
        return result;
    }
    const auto &groups = ani.GetGroups();
    if (groups.empty()) {
        result.error = "ANI has no animation groups";
        return result;
    }

    // ── 2. 解析同目录 Robo.hod（骨骼层级/命名）──
    fs::path aniFsPath(aniPath);
    std::string aniDir = aniFsPath.parent_path().string();
    std::string hodPath;
    for (const char *cand : {"Robo.hod", "robo.hod", "ROBO.HOD"}) {
        std::string p = aniDir + "/" + cand;
        if (fs::exists(p)) { hodPath = p; break; }
    }
    if (hodPath.empty()) {
        for (auto &entry : fs::directory_iterator(aniDir)) {
            std::string fn = entry.path().filename().string();
            if (entry.path().extension() == ".hod" && fn.find("hangar") == std::string::npos &&
                fn.find("Hangar") == std::string::npos) {
                hodPath = entry.path().string();
                break;
            }
        }
    }
    if (hodPath.empty()) {
        result.error = "No Robo.hod found next to ANI for skeleton hierarchy";
        return result;
    }
    HODParser hodParser;
    if (!hodParser.ParseFile(hodPath)) {
        result.error = "HOD parse failed: " + hodParser.GetError();
        return result;
    }
    const auto &hod = hodParser.GetResult();
    if (hod.BoneCount() == 0) {
        result.error = "HOD has no bones";
        return result;
    }

    // ── 3. 加载部件网格（IsRenderBone 过滤）+ LR 交换（与静态/动画导出一致）──
    std::string hodDir = fs::path(hodPath).parent_path().string();
    std::vector<PartData> parts;
    for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
        const auto &bone = hod.bones[bi];
        if (!IsRenderBone(bone.name))
            continue;
        std::string xPath = hodDir + "/" + bone.name;
        if (!fs::exists(xPath))
            continue;
        XFileParser parser;
        if (!parser.ParseFile(xPath))
            continue;
        const auto &meshes = parser.GetMeshes();
        if (meshes.empty())
            continue;
        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            if (meshes[mi].name.find("_mShape") != std::string::npos)
                continue;
            bool merged = false;
            for (auto &pd : parts) {
                if (pd.boneIndex == bi) {
                    auto &dst = pd.mesh;
                    auto &src = meshes[mi];
                    uint32_t vOff = static_cast<uint32_t>(dst.VertexCount());
                    for (auto v : src.positions) dst.positions.push_back(v);
                    for (auto v : src.normals) dst.normals.push_back(v);
                    for (auto v : src.texcoords) dst.texcoords.push_back(v);
                    for (auto idx : src.indices) dst.indices.push_back(idx + vOff);
                    dst.ComputeBounds();
                    merged = true;
                    break;
                }
            }
            if (merged)
                continue;
            PartData pd;
            pd.name = bone.name;
            pd.boneIndex = bi;
            pd.originalBoneIndex = bi;
            pd.mesh = meshes[mi];
            parts.push_back(std::move(pd));
        }
    }
    if (parts.empty()) {
        result.error = "No renderable parts found next to ANI";
        return result;
    }
    {
        auto swapLR = [&](const std::string &l, const std::string &r) {
            int li = -1, ri = -1;
            for (size_t i = 0; i < parts.size(); ++i) {
                std::string nm = fs::path(parts[i].name).stem().string();
                if (nm == l) li = (int)i;
                if (nm == r) ri = (int)i;
            }
            if (li >= 0 && ri >= 0) {
                std::swap(parts[li].name, parts[ri].name);
                std::swap(parts[li].mesh, parts[ri].mesh);
            }
        };
        swapLR("arm1", "arm1_r");
        swapLR("arm2", "arm2_r");
        swapLR("Hand", "Hand_r");
        swapLR("leg1", "leg1_r");
        swapLR("leg2", "leg2_r");
        swapLR("leg3", "leg3_r");
    }

    // ── 4. 每帧组装嵌套 x ──
    std::map<int, const PartData *> bonePartMap;
    for (const auto &p : parts)
        bonePartMap[p.boneIndex] = &p;

    int frameCount = 0;
    for (const auto &g : groups) {
        char groupDirName[16];
        snprintf(groupDirName, sizeof(groupDirName), "%02u", g.index);
        fs::path groupPath = fs::path(outputDir) / groupDirName;
        fs::create_directories(groupPath);

        for (size_t fi = 0; fi < g.frames.size(); ++fi) {
            // 解析帧 → 每骨骼局部矩阵
            std::vector<Mat4x4> frameLocal;
            if (!ParseFrameToLocalMatrices(g.frames[fi].data, hod, frameLocal))
                continue;

            // 帧世界矩阵（层级累乘 + Ry180；帧块数据为绝对姿势，无 Body_d -1.30 修正）
            std::vector<Mat4x4> frameWorld(hod.BoneCount());
            for (int bj = 0; bj < static_cast<int>(hod.BoneCount()); ++bj) {
                Mat4x4 local = (bj < static_cast<int>(frameLocal.size())) ? frameLocal[bj] : Mat4x4::Identity();
                if (hod.bones[bj].parentIndex >= 0)
                    frameWorld[bj] = local * frameWorld[hod.bones[bj].parentIndex];
                else
                    frameWorld[bj] = local;
            }
            for (auto &fw : frameWorld) {
                for (int j = 0; j < 4; ++j) fw.m[j] = -fw.m[j];
                for (int j = 8; j < 12; ++j) fw.m[j] = -fw.m[j];
            }

            // 输出文件名：{序号}_{帧名}.x
            std::string frameBase = fs::path(g.frames[fi].name).stem().string();
            if (frameBase.size() > 4 && frameBase.substr(frameBase.size() - 4) == ".hod")
                frameBase = frameBase.substr(0, frameBase.size() - 4);
            char prefix[16];
            snprintf(prefix, sizeof(prefix), "%03u", static_cast<uint32_t>(fi + 1));
            fs::path xPath = groupPath / (std::string(prefix) + "_" + frameBase + ".x");
            std::ofstream ofsX(xPath);
            if (!ofsX)
                continue;
            ofsX << "xof 0302txt 0032\n\n";

            // 递归写骨骼 Frame + Mesh（局部矩阵 = this_world × inverse(parent_world)）
            const auto &xW = frameWorld;
            std::function<void(int, int)> writeFrame = [&](int bi, int depth) {
                std::string in(depth > 0 ? depth * 2 : 0, ' ');
                std::string nm = fs::path(hod.bones[bi].name).stem().string();
                ofsX << in << "Frame " << nm << " {\n";
                Mat4x4 mL;
                if (hod.bones[bi].parentIndex >= 0) {
                    Mat4x4 iP = xW[hod.bones[bi].parentIndex].Inverse();
                    mL = xW[bi] * iP;
                } else {
                    mL = xW[bi];
                }
                ofsX << in << "  FrameTransformMatrix {\n";
                for (int r = 0; r < 4; ++r) {
                    ofsX << in << "    ";
                    for (int c = 0; c < 4; ++c)
                        ofsX << (c > 0 ? "," : "") << mL.m[r * 4 + c];
                    ofsX << (r < 3 ? ",\n" : ";;\n");
                }
                ofsX << in << "  }\n";

                auto it = bonePartMap.find(bi);
                if (it != bonePartMap.end()) {
                    const auto &ms = it->second->mesh;
                    uint32_t vc = (uint32_t)ms.VertexCount(), fc = (uint32_t)ms.indices.size() / 3;
                    ofsX << in << "  Mesh {\n";
                    ofsX << in << "    " << vc << ";\n" << in << "    ";
                    for (uint32_t vi = 0; vi < vc; ++vi)
                        ofsX << ms.positions[vi * 3] << ";" << ms.positions[vi * 3 + 1] << ";"
                             << ms.positions[vi * 3 + 2] << ";,";
                    ofsX.seekp(-1, std::ios::cur);
                    ofsX << ";\n";
                    ofsX << in << "    " << fc << ";\n" << in << "    ";
                    for (uint32_t fi2 = 0; fi2 < fc; ++fi2)
                        ofsX << "3;" << ms.indices[fi2 * 3] << ";" << ms.indices[fi2 * 3 + 1] << ";"
                             << ms.indices[fi2 * 3 + 2] << ";,\n";
                    ofsX.seekp(-1, std::ios::cur);
                    ofsX << ";\n";
                    if (ms.HasNormals()) {
                        ofsX << in << "    MeshNormals {\n";
                        ofsX << in << "      " << vc << ";\n" << in << "      ";
                        for (uint32_t vi = 0; vi < vc; ++vi)
                            ofsX << ms.normals[vi * 3] << ";" << ms.normals[vi * 3 + 1] << ";"
                                 << ms.normals[vi * 3 + 2] << ";,";
                        ofsX.seekp(-1, std::ios::cur);
                        ofsX << ";\n";
                        ofsX << in << "      " << fc << ";\n" << in << "      ";
                        for (uint32_t fi2 = 0; fi2 < fc; ++fi2)
                            ofsX << "3;" << ms.indices[fi2 * 3] << ";" << ms.indices[fi2 * 3 + 1] << ";"
                                 << ms.indices[fi2 * 3 + 2] << ";,\n";
                        ofsX.seekp(-1, std::ios::cur);
                        ofsX << ";\n";
                        ofsX << in << "    }\n";
                    }
                    if (ms.HasTexcoords()) {
                        ofsX << in << "    MeshTextureCoords {\n";
                        ofsX << in << "      " << vc << ";\n" << in << "      ";
                        for (uint32_t vi = 0; vi < vc; ++vi)
                            ofsX << ms.texcoords[vi * 2] << ";" << ms.texcoords[vi * 2 + 1] << ";,";
                        ofsX.seekp(-1, std::ios::cur);
                        ofsX << ";\n";
                        ofsX << in << "    }\n";
                    }
                    ofsX << in << "  }\n";
                }
                for (uint32_t ci : hod.bones[bi].children)
                    writeFrame((int)ci, depth + 1);
                ofsX << in << "}\n\n";
            };
            for (int bi = 0; bi < (int)hod.BoneCount(); ++bi)
                if (hod.bones[bi].parentIndex < 0)
                    writeFrame(bi, 0);

            result.outputFiles.push_back(xPath.string());
            frameCount++;
        }
    }

    result.success = true;
    result.partCount = frameCount;
    result.vertexCount = static_cast<int>(parts.size());
    result.indexCount = 0;
    std::cout << "[anix] Exported " << frameCount << " frames → " << outputDir << std::endl;
    return result;
}

} // namespace AssetTool
