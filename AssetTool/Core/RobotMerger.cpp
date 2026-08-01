#include "RobotMerger.h"
#include "ANIParser.h"
#include "Asset/Definitions/Material/MaterialDesc.h"
#include "Asset/IO/Writer/DxMeshWriter.h"
#include "RobotMergerUtil.h"
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
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace AssetTool {

// Mat4x4 实现已拆分至 RobotMergerMath.cpp（2026-08-01 适度拆分）
// IsRenderBone / CreatePartMaterial / ParseFrameToLocalMatrices 已拆分至 RobotMergerUtil.cpp
// ExportAnimationsFBX 已拆分至 RobotMergerFBX.cpp

RobotMergeResult RobotMerger::Merge(const std::string &hodPath, const std::string &outputDir,
                                    const RobotMergeOptions &options) {
    return MergeWithCallback(hodPath, outputDir, options, nullptr);
}

RobotMergeResult RobotMerger::MergeWithCallback(const std::string &hodPath, const std::string &outputDir,
                                                const RobotMergeOptions &options, ProgressCallback callback,
                                                const std::string *stemOverride) {
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
    // stemOverride：显式指定输出名主干（MergeFromANI 用 ANI 首帧临时 .hod 时，
    // 避免临时文件名 {stem}_frame0 泄漏到输出产物名）
    std::string stem = (stemOverride && !stemOverride->empty()) ? *stemOverride : hodFsPath.stem().string();
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

        // assimp 按材质把 .x 拆成多个子网格：保留拆分（不再合并），每个子网格
        // 独立成部件并保留各自材质。命名 {stem}_{序号:03d}（00x 模式）保证唯一。
        std::string boneStem = fs::path(bone.name).stem().string();
        int subIdx = 0;
        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            // 跳过 assimp 拆分出的 _mShape 网格
            if (meshes[mi].name.find("_mShape") != std::string::npos)
                continue;
            ++subIdx;

            PartData pd;
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
        result.error = "No renderable parts found";
        return result;
    }
    // ── 3. LR 交换（交换完整部分数据：名字+网格，不交换 boneIndex/矩阵） ──
    if (options.lrSwap) {
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
                pos[2] = -pos[2].get<double>(); // position.z
                auto &rot = jb["rotation"];
                rot[2] = -rot[2].get<double>(); // rotation.z
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
                boneWorldLH[bi].m[2] = -boneWorldLH[bi].m[2];   // column Z row0
                boneWorldLH[bi].m[6] = -boneWorldLH[bi].m[6];   // column Z row1
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
        std::string matKey = stem + "_" + part.mesh.name; // 子网格唯一名（避免同骨骼多 part 重复）
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
            std::string matKey = stem + "_" + part.mesh.name; // 子网格唯一名（避免同骨骼多 part 重复）
            materials.push_back(matKey);
            deps[matKey] = nlohmann::json::object();
            deps[matKey]["type"] = "material";
            deps[matKey]["faceColor"] = {xMat.faceColor[0], xMat.faceColor[1], xMat.faceColor[2], xMat.faceColor[3]};
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

RobotMergeResult RobotMerger::MergeFromANI(const std::string &aniPath, const std::string &outputDir,
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
    log("  母版骨架: " + std::to_string(master.BoneCount()) + " 部件, isHd2=" + (master.isHd2 ? "是" : "否"));
    if (master.bones.empty()) {
        result.error = "ANI master skeleton is empty (no ANIRobo/AN2Robo master block)";
        log("✗ " + result.error);
        return result;
    }
    {
        std::string names;
        for (size_t i = 0; i < master.bones.size() && i < 8; ++i) {
            if (i)
                names += ", ";
            names += master.bones[i].name;
        }
        if (master.bones.size() > 8)
            names += ", ...";
        log("  部件名(前8): " + names);
    }

    // ── 2. HOD 骨骼来源：仅 ANI 首帧提取（2026-08-01 用户定案：不依赖同目录 Robo.hod）──
    fs::path aniFsPath(aniPath);
    std::string stemName = aniFsPath.stem().string();
    if (stemName.empty())
        stemName = "Robo";
    std::string hodPath;
    std::string hodErr;
    hodPath = AssetTool::WriteFirstFrameHOD(aniPath, stemName, hodErr);
    if (hodPath.empty()) {
        result.error = "ANI first frame HOD extraction failed: " + hodErr;
        log("✗ " + result.error);
        return result;
    }
    log("✓ 绑定矩阵 HOD（ANI 首帧提取）: " + hodPath);

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
    // stemOverride：输出名用 ANI stem（避免首帧临时 {stem}_frame0.hod 的后缀泄漏到产物名）
    log("→ 开始标准合并...");
    RobotMergeResult merged = MergeWithCallback(hodPath, outputDir, options, nullptr, &stemName);
    if (merged.success) {
        log("✓ 合并成功: 部件=" + std::to_string(merged.partCount) + " 顶点=" + std::to_string(merged.vertexCount) +
            " 索引=" + std::to_string(merged.indexCount));
        for (const auto &f : merged.outputFiles)
            log("  输出: " + f);
    } else {
        log("✗ 合并失败: " + merged.error);
    }
    if (mergeLog.is_open())
        mergeLog.close();
    return merged;
}

} // namespace AssetTool
