#include "RobotMerger.h"
#include "Asset/Definitions/Material/MaterialDesc.h"
#include "Asset/IO/Writer/DxMeshWriter.h"
#include "TextureConverter.h"

#include <assimp/Exporter.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
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

    // ── 5. .x 平铺导出（boneWorld 正确朝向 + 世界矩阵） ──
    if (options.exportX) {
        if (callback)
            callback(0, 0, "[robot] Exporting .x for DE verification...");
        std::string xPath = robotOutDir + "/" + stem + ".x";
        std::ofstream ofs(xPath);
        if (ofs) {
            ofs << "xof 0302txt 0032\n\n";
            for (const auto &part : parts) {
                const auto &ms = part.mesh;
                // 用原始 boneIndex（LR 交换前）取世界矩阵，保持 Frame 名与社区版一致
                Mat4x4 wm = (part.originalBoneIndex >= 0 && part.originalBoneIndex < (int)boneWorld.size())
                                ? boneWorld[part.originalBoneIndex]
                                : Mat4x4::Identity();
                std::string nm = fs::path(part.name).stem().string();
                uint32_t vc = (uint32_t)ms.VertexCount(), fc = (uint32_t)ms.indices.size() / 3;
                ofs << "Frame " << nm << " {\n";
                ofs << "  FrameTransformMatrix {\n";
                for (int r = 0; r < 4; ++r) {
                    ofs << "    ";
                    for (int c = 0; c < 4; ++c)
                        ofs << (c > 0 ? "," : "") << wm.m[r * 4 + c];
                    ofs << (r < 3 ? ",\n" : ";;\n  }\n");
                }
                ofs << "  Mesh {\n    " << vc << ";\n    ";
                for (uint32_t vi = 0; vi < vc; ++vi)
                    ofs << ms.positions[vi * 3] << ";" << ms.positions[vi * 3 + 1] << ";" << ms.positions[vi * 3 + 2]
                        << ";,";
                ofs.seekp(-1, std::ios::cur);
                ofs << ";\n";
                ofs << "    " << fc << ";\n    ";
                for (uint32_t fi = 0; fi < fc; ++fi)
                    ofs << "3;" << ms.indices[fi * 3] << ";" << ms.indices[fi * 3 + 1] << ";" << ms.indices[fi * 3 + 2]
                        << ";,\n";
                ofs.seekp(-1, std::ios::cur);
                ofs << ";\n";
                if (ms.HasNormals()) { /* 省略法线/UV 输出，与之前相同 */
                }
                if (ms.HasTexcoords()) { /* 省略 UV 输出 */
                }
                ofs << "  }\n}\n\n";
            }
        }
        result.outputFiles.push_back(xPath);
    }

    // ── 5b. .x 嵌套层级导出（二阶段：从 boneWorld 推导正确局部矩阵 + 骨骼树） ──
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

                // 网格挂载到对应的骨骼节点
                boneNodes[bi]->mNumMeshes = 1;
                boneNodes[bi]->mMeshes = new unsigned int[1]{static_cast<unsigned int>(pi)};
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

} // namespace AssetTool
