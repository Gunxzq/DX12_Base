// ========================================================================
// RobotMergerFBX.cpp — 动画 FBX 导出（B2.5）
// 拆分自 RobotMerger.cpp（2026-08-01，适度拆分以缓解 C1060 编译堆不足）
// 解析 Script.ani 各动画组帧数据（标准 HOD 9847B，HODParser 直解），
// 每帧每骨骼 TRS → aiNodeAnim 关键帧 → aiAnimation，
// 输出 {outputDir}/{stem}_anim.fbx（独立动画文件，含骨骼节点树 + 动画通道）
// ========================================================================

#include "ANIParser.h"
#include "RobotMerger.h"
#include "RobotMergerUtil.h"

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
#include <iterator>
#include <set>
#include <utility>

namespace fs = std::filesystem;

using namespace AssetTool;

RobotMergeResult RobotMerger::ExportAnimationsFBX(const std::string &aniPath, const std::string &outputDir,
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

    // ── 2. HOD 骨骼来源：仅 ANI 首帧提取（2026-08-01 用户定案：不依赖同目录 Robo.hod）──
    std::string hodPath;
    std::string hodErr;
    hodPath = AssetTool::WriteFirstFrameHOD(aniPath, stem, hodErr);
    if (hodPath.empty()) {
        result.error = "ANI first frame HOD extraction failed: " + hodErr;
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
    std::vector<RobotMerger::Mat4x4> bindWorld(hod.BoneCount());
    for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
        RobotMerger::Mat4x4 local;
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
        for (int j = 0; j < 4; ++j)
            bw.m[j] = -bw.m[j];
        for (int j = 8; j < 12; ++j)
            bw.m[j] = -bw.m[j];
    }

    // ── 4. 构建 aiScene + 加载部件网格（IsRenderBone 过滤：排除武器/发射口/root/hit 等无实体骨骼）──
    aiScene *scene = new aiScene();
    scene->mRootNode = new aiNode("root");
    std::string hodDir = fs::path(hodPath).parent_path().string();
    std::vector<RobotMerger::PartData> parts;
    for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
        const auto &bone = hod.bones[bi];
        if (!RobotMerger::IsRenderBone(bone.name))
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
        // assimp 按材质把 .x 拆成多个子网格：保留拆分（不再合并），每个子网格
        // 独立成部件并保留各自材质。命名 {stem}_{序号:03d}（00x 模式）保证唯一，
        // 网格节点/材质名均基于此，避免 Blender 命名冲突。
        std::string boneStem = fs::path(bone.name).stem().string();
        int subIdx = 0;
        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            if (meshes[mi].name.find("_mShape") != std::string::npos)
                continue;
            ++subIdx;
            RobotMerger::PartData pd;
            pd.name = bone.name;
            pd.boneIndex = bi;
            pd.originalBoneIndex = bi;
            pd.mesh = meshes[mi];
            // 子网格唯一名：{stem}_{序号:03d}
            std::string seq = std::to_string(subIdx);
            if (seq.size() < 3)
                seq = std::string(3 - seq.size(), '0') + seq;
            pd.mesh.name = boneStem + "_" + seq;
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
        // 子网格拆分后同一骨骼有多个 part：按 stem 收集左右两组索引，成对交换，
        // 避免只交换最后一个匹配 part 导致漏交换
        auto swapLR = [&](const std::string &l, const std::string &r) {
            std::vector<int> li, ri;
            for (size_t i = 0; i < parts.size(); ++i) {
                std::string nm = fs::path(parts[i].name).stem().string();
                if (nm == l)
                    li.push_back(static_cast<int>(i));
                else if (nm == r)
                    ri.push_back(static_cast<int>(i));
            }
            size_t n = std::min(li.size(), ri.size());
            for (size_t k = 0; k < n; ++k) {
                std::swap(parts[li[k]].name, parts[ri[k]].name);
                std::swap(parts[li[k]].mesh, parts[ri[k]].mesh);
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
        for (char c : hod.bones[bi].name)
            nm += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (nm.find("root") != std::string::npos)
            allRenderBones.erase(bi);
    }

    // ── 4.5 骨骼 FBX 显示名：去 .x 后缀并加 _bone，与网格名（stem）区分 ──
    //    骨骼节点/蒙皮绑定/动画通道三处必须用同一名字，Blender 按名匹配。
    //    不能与网格同名：Blender FBX 导入器对骨骼名与网格对象名共享全局
    //    唯一化命名空间，同名会合并节点并丢失骨骼树（leg1_r.001 现象）。
    auto fbxBoneName = [](const std::string &raw) -> std::string {
        std::string stem = fs::path(raw).stem().string();
        if (stem.empty())
            return raw;
        return stem + "_bone";
    };

    // ── 5. 构建骨骼节点树（仅过滤后骨骼）──
    std::vector<aiNode *> boneNodes(hod.BoneCount(), nullptr);
    for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
        if (allRenderBones.find(bi) == allRenderBones.end())
            continue;
        aiNode *node = new aiNode(fbxBoneName(hod.bones[bi].name));
        boneNodes[bi] = node;

        // 向上追溯实际挂载父骨骼（跳过被过滤的中间骨骼/root，与静态 FBX 6a 一致）
        int mountParent = hod.bones[bi].parentIndex;
        while (mountParent >= 0 && allRenderBones.find(mountParent) == allRenderBones.end())
            mountParent = hod.bones[mountParent].parentIndex;

        RobotMerger::Mat4x4 bindLocal;
        if (mountParent >= 0)
            bindLocal = bindWorld[bi] * bindWorld[mountParent].Inverse();
        else
            bindLocal = bindWorld[bi];
        node->mTransformation.a1 = bindLocal.m[0];
        node->mTransformation.a2 = bindLocal.m[4];
        node->mTransformation.a3 = bindLocal.m[8];
        node->mTransformation.a4 = bindLocal.m[12];
        node->mTransformation.b1 = bindLocal.m[1];
        node->mTransformation.b2 = bindLocal.m[5];
        node->mTransformation.b3 = bindLocal.m[9];
        node->mTransformation.b4 = bindLocal.m[13];
        node->mTransformation.c1 = bindLocal.m[2];
        node->mTransformation.c2 = bindLocal.m[6];
        node->mTransformation.c3 = bindLocal.m[10];
        node->mTransformation.c4 = bindLocal.m[14];
        node->mTransformation.d1 = bindLocal.m[3];
        node->mTransformation.d2 = bindLocal.m[7];
        node->mTransformation.d3 = bindLocal.m[11];
        node->mTransformation.d4 = bindLocal.m[15];
        int parent = mountParent;
        if (parent >= 0)
            boneNodes[parent]->addChildren(1, &node);
        else
            scene->mRootNode->addChildren(1, &node);
    }

    // ── 6. 构建 aiMesh（每个部件，蒙皮绑定）──
    scene->mNumMeshes = static_cast<unsigned int>(parts.size());
    scene->mMeshes = new aiMesh *[scene->mNumMeshes];
    // 材质去重（2026-08-01）：同参数材质共享同一 aiMaterial（SameMaterial 判据），
    // 减少 FBX 材质槽数 → Blender 按材质合并子网格更干净（如 38 → ~5~6）
    std::vector<XFileMaterial> usedMaterials; // 已用材质列表（查重基准，循环后决定 mNumMaterials）
    scene->mNumMaterials = static_cast<unsigned int>(parts.size()); // 上限分配，循环后收缩
    scene->mMaterials = new aiMaterial *[scene->mNumMaterials];
    for (size_t pi = 0; pi < parts.size(); ++pi) {
        const auto &part = parts[pi];
        const auto &ms = part.mesh;
        uint32_t vc = static_cast<uint32_t>(ms.VertexCount());
        uint32_t ic = static_cast<uint32_t>(ms.indices.size());
        uint32_t fc = ic / 3;

        aiMesh *mesh = new aiMesh();
        mesh->mName = part.mesh.name; // {stem}_{序号:03d}，子网格唯一名
        mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        mesh->mNumVertices = vc;
        mesh->mVertices = new aiVector3D[vc];
        mesh->mNormals = ms.HasNormals() ? new aiVector3D[vc] : nullptr;
        if (ms.HasTexcoords()) {
            mesh->mTextureCoords[0] = new aiVector3D[vc];
            mesh->mNumUVComponents[0] = 2;
        }
        // 顶点烘焙到绑定世界坐标（bindWorld 含 Body_d 修正 + 层级累乘 + Ry180）：
        // 网格独立挂场景根（节点单位矩阵），必须把部件局部坐标变换到绑定姿势世界位置，
        // 与 offsetMatrix = inverse(bindWorld) 匹配——否则 Blender 蒙皮计算错位（mesh 混乱）。
        RobotMerger::Mat4x4 bindXform = RobotMerger::Mat4x4::Identity();
        if (part.boneIndex >= 0)
            bindXform = bindWorld[static_cast<size_t>(part.boneIndex)];
        for (uint32_t vi = 0; vi < vc; ++vi) {
            float px = ms.positions[vi * 3 + 0], py = ms.positions[vi * 3 + 1], pz = ms.positions[vi * 3 + 2];
            bindXform.TransformPoint(px, py, pz);
            mesh->mVertices[vi] = aiVector3D(px, py, pz);
            if (ms.HasNormals()) {
                float nx = ms.normals[vi * 3 + 0], ny = ms.normals[vi * 3 + 1], nz = ms.normals[vi * 3 + 2];
                bindXform.TransformDirection(nx, ny, nz);
                mesh->mNormals[vi] = aiVector3D(nx, ny, nz);
            }
            if (ms.HasTexcoords())
                mesh->mTextureCoords[0][vi] = aiVector3D(ms.texcoords[vi * 2 + 0], ms.texcoords[vi * 2 + 1], 0);
        }
        // 刚性蒙皮绑定
        if (part.boneIndex >= 0) {
            uint8_t bi = static_cast<uint8_t>(part.boneIndex);
            mesh->mNumBones = 1;
            mesh->mBones = new aiBone *[1];
            aiBone *bone = new aiBone();
            bone->mName = aiString(fbxBoneName(hod.bones[bi].name));
            bone->mNumWeights = vc;
            bone->mWeights = new aiVertexWeight[vc];
            for (uint32_t vi = 0; vi < vc; ++vi) {
                bone->mWeights[vi].mVertexId = vi;
                bone->mWeights[vi].mWeight = 1.0f;
            }
            RobotMerger::Mat4x4 invBind = bindWorld[bi].Inverse();
            bone->mOffsetMatrix.a1 = invBind.m[0];
            bone->mOffsetMatrix.a2 = invBind.m[4];
            bone->mOffsetMatrix.a3 = invBind.m[8];
            bone->mOffsetMatrix.a4 = invBind.m[12];
            bone->mOffsetMatrix.b1 = invBind.m[1];
            bone->mOffsetMatrix.b2 = invBind.m[5];
            bone->mOffsetMatrix.b3 = invBind.m[9];
            bone->mOffsetMatrix.b4 = invBind.m[13];
            bone->mOffsetMatrix.c1 = invBind.m[2];
            bone->mOffsetMatrix.c2 = invBind.m[6];
            bone->mOffsetMatrix.c3 = invBind.m[10];
            bone->mOffsetMatrix.c4 = invBind.m[14];
            bone->mOffsetMatrix.d1 = invBind.m[3];
            bone->mOffsetMatrix.d2 = invBind.m[7];
            bone->mOffsetMatrix.d3 = invBind.m[11];
            bone->mOffsetMatrix.d4 = invBind.m[15];
            mesh->mBones[0] = bone;
            // 网格挂载到场景根节点（不与骨骼节点构成对象父子关系）：
            // 骨骼节点保持无 mesh → FBX 导出为 Null/LimbNode 类型（Blender 识别 Armature）；
            // 网格独立挂根 → Blender 中仅通过 Armature Modifier 蒙皮关联骨骼，
            // Pose 模式旋转骨骼时只驱动绑定顶点（局部变形），不会整体跟随骨骼节点。
            aiNode *meshNode = new aiNode(mesh->mName.C_Str());
            meshNode->mNumMeshes = 1;
            meshNode->mMeshes = new unsigned int[1]{static_cast<unsigned int>(pi)};
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
        // 材质去重：查已有材质（SameMaterial），命中则共享索引，未命中新建
        int matIdx = -1;
        for (size_t ui = 0; ui < usedMaterials.size(); ++ui) {
            if (SameMaterial(usedMaterials[ui], ms.material)) {
                matIdx = static_cast<int>(ui);
                break;
            }
        }
        if (matIdx < 0) {
            matIdx = static_cast<int>(usedMaterials.size());
            usedMaterials.push_back(ms.material);
            // 材质（颜色材质；纹理导出暂缓，见 CreatePartMaterial 注释）；名称 = Material + 去重序号
            scene->mMaterials[matIdx] = CreatePartMaterial("Material" + std::to_string(matIdx), ms.material);
        }
        mesh->mMaterialIndex = static_cast<unsigned int>(matIdx);
        scene->mMeshes[pi] = mesh;
    }
    // 材质表收缩到实际唯一数量（去重后）
    scene->mNumMaterials = static_cast<unsigned int>(usedMaterials.size());
    std::cout << "[fbx] 材质去重: " << parts.size() << " 部件 → " << usedMaterials.size() << " 唯一材质\n";

    // ── 7. 每组 → aiAnimation（仅过滤后骨骼通道）──
    scene->mNumAnimations = static_cast<unsigned int>(groups.size());
    scene->mAnimations = new aiAnimation *[scene->mNumAnimations];

    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto &g = groups[gi];
        // 时间轴：Tail 头速度值 = 帧间隔秒数（docs：0.1f → 10 FPS）；
        // 范围校验：垃圾值（空 Tail 组/定位偏差读出 0 或 1e33）回退默认 30fps
        const double ticksPerSec = (g.tail.speed > 0.001f && g.tail.speed < 1.0f) ? (1.0 / g.tail.speed) : 30.0;
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
            chan->mNodeName = aiString(fbxBoneName(hod.bones[bi].name));
            chan->mNumPositionKeys = static_cast<unsigned int>(g.frames.size());
            chan->mPositionKeys = new aiVectorKey[chan->mNumPositionKeys];
            chan->mNumRotationKeys = static_cast<unsigned int>(g.frames.size());
            chan->mRotationKeys = new aiQuatKey[chan->mNumRotationKeys];
            chan->mNumScalingKeys = static_cast<unsigned int>(g.frames.size());
            chan->mScalingKeys = new aiVectorKey[chan->mNumScalingKeys];

            for (size_t fi = 0; fi < g.frames.size(); ++fi) {
                std::vector<RobotMerger::Mat4x4> frameLocal;
                if (!ParseFrameToLocalMatrices(g.frames[fi].data, hod, frameLocal))
                    continue;
                if (bi >= static_cast<int>(frameLocal.size()))
                    continue;

                std::vector<RobotMerger::Mat4x4> frameWorld(hod.BoneCount());
                for (int bj = 0; bj < static_cast<int>(hod.BoneCount()); ++bj) {
                    RobotMerger::Mat4x4 local =
                        (bj < static_cast<int>(frameLocal.size())) ? frameLocal[bj] : RobotMerger::Mat4x4::Identity();
                    // 帧块数据 = 绝对姿势（与绑定姿势同坐标系，无 Body_d -1.30 修正）
                    if (hod.bones[bj].parentIndex >= 0)
                        frameWorld[bj] = local * frameWorld[hod.bones[bj].parentIndex];
                    else
                        frameWorld[bj] = local;
                }
                for (auto &fw : frameWorld) {
                    for (int j = 0; j < 4; ++j)
                        fw.m[j] = -fw.m[j];
                    for (int j = 8; j < 12; ++j)
                        fw.m[j] = -fw.m[j];
                }

                RobotMerger::Mat4x4 fbxLocal;
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

    // ── 8. 导出（二进制 FBX：Blender 5.2 的 FBX 导入器不支持 ASCII，必须二进制）──
    std::string path = outputDir + "/" + stem + "_anim.fbx";
    Assimp::Exporter exporter;
    if (exporter.Export(scene, "fbx", path) == AI_SUCCESS) {
        result.outputFiles.push_back(path);
        result.success = true;
        result.partCount = static_cast<int>(allRenderBones.size());
        result.vertexCount = static_cast<int>(parts.size());
        result.indexCount = 0;
        std::cout << "[fbx] Exported animation (binary): " << path << " (" << groups.size() << " clips, "
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
                    double fps = (g.tail.speed > 0.001f && g.tail.speed < 1.0f) ? (1.0 / g.tail.speed) : 30.0;
                    ofs << "\n== group " << (gi + 1) << " [" << gn << "] frames=" << g.frames.size()
                        << " tailTime=" << g.tail.time << " tailSpeed=" << g.tail.speed << " fps=" << fps << " ==\n";
                    for (size_t fi = 0; fi < g.frames.size(); ++fi) {
                        std::vector<RobotMerger::Mat4x4> frameLocal;
                        if (!ParseFrameToLocalMatrices(g.frames[fi].data, hod, frameLocal))
                            continue;
                        std::vector<RobotMerger::Mat4x4> frameWorld(hod.BoneCount());
                        for (int bj = 0; bj < static_cast<int>(hod.BoneCount()); ++bj) {
                            RobotMerger::Mat4x4 local = (bj < static_cast<int>(frameLocal.size()))
                                                            ? frameLocal[bj]
                                                            : RobotMerger::Mat4x4::Identity();
                            if (hod.bones[bj].parentIndex >= 0)
                                frameWorld[bj] = local * frameWorld[hod.bones[bj].parentIndex];
                            else
                                frameWorld[bj] = local;
                        }
                        for (auto &fw : frameWorld) {
                            for (int j = 0; j < 4; ++j)
                                fw.m[j] = -fw.m[j];
                            for (int j = 8; j < 12; ++j)
                                fw.m[j] = -fw.m[j];
                        }
                        ofs << "  frame " << fi << " (t=" << (fi / fps) << "s):\n";
                        for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
                            if (allRenderBones.find(bi) == allRenderBones.end())
                                continue;
                            RobotMerger::Mat4x4 mL;
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
