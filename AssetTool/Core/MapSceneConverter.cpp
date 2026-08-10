#include "MapSceneConverter.h"
#include "Asset/Definitions/Material/MaterialDesc.h"
#include "Asset/Definitions/Mesh/DxMeshFormat.h"
#include "Asset/Definitions/Scene/DxSceneFormat.h" // DxScene 二进制格式（.scene——DXSCENE 魔数 + SOA 布局）
#include "Asset/IO/Writer/DxMeshWriter.h"
#include "MPDSceneParser.h"
#include "ScriptSPTParser.h"
#include "TextureConverter.h"
#include "XFileParser.h"
#include "XORCipher.h"

#include <DirectXMath.h>
#include <cstdlib> // std::atof（@CullFar 剔除距离解析）
#include <nlohmann/json.hpp>
// windows.h 的 min/max 宏会破坏 std::min/std::max（C2589），必须先行禁用
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath> // std::fabs（地面合并 IsMergeableGround 用）
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_set> // 水区块 flood fill 连通合并 visited
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;
using namespace DirectX;

namespace AssetTool {
namespace {

// ==========================================================================
// 工具函数
// ==========================================================================

/// FNV-1a 64-bit（材质内容 hash 去重，MaterialDesc.hash 字段）
uint64_t Fnv1a64(const uint8_t *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/// uint64 → 16 位小写十六进制字符串
std::string ToHex(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

/// 小写化（透明标记 / 文件名比较用）
std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// UTF-8 → 宽字符（DxMeshWriter 使用 wstring 路径，避免中文目录打开失败）
std::wstring Utf8ToWide(const std::string &s) {
    if (s.empty())
        return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

/// 材质内容（去重 hash 输入：faceColor/power/specular/emissive + 纹理 key）
std::string MaterialContent(const XFileMaterial &m, const std::string &texKey) {
    std::string buf;
    buf.append(reinterpret_cast<const char *>(m.faceColor), sizeof(m.faceColor));
    buf.append(reinterpret_cast<const char *>(&m.power), sizeof(m.power));
    buf.append(reinterpret_cast<const char *>(m.specularColor), sizeof(m.specularColor));
    buf.append(reinterpret_cast<const char *>(m.emissiveColor), sizeof(m.emissiveColor));
    buf += texKey;
    return buf;
}

/// MaterialDesc → 内联 MaterialDefinition JSON（纹理引用指向 dependencies.textures key）
nlohmann::json MaterialToJSON(const DX12Engine::Resource::MaterialDesc &d, const std::string &texKey) {
    nlohmann::json jm;
    jm["shader"] = d.shader;
    jm["params"]["baseColor"] = {d.params.baseColor[0], d.params.baseColor[1], d.params.baseColor[2],
                                 d.params.baseColor[3]};
    jm["params"]["metallic"] = d.params.metallic;
    jm["params"]["roughness"] = d.params.roughness;
    jm["params"]["ao"] = d.params.ao;
    jm["params"]["emissive"] = {d.params.emissive[0], d.params.emissive[1], d.params.emissive[2], d.params.emissive[3]};
    if (!texKey.empty())
        jm["textures"]["baseColor"] = texKey;
    return jm;
}

/// 明文图像魔数判断（PNG \x89PNG / BMP "BM"）
bool IsPlainImageMagic(const uint8_t *magic, size_t size) {
    if (size >= 4 && magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G')
        return true;
    if (size >= 2 && magic[0] == 'B' && magic[1] == 'M')
        return true;
    return false;
}

/// 地面可合并判定：mapChip 单材质纯平面（平面四边形可表达 + 材质贴图一致）
/// 条件：① stem 以 "mapChip" 开头（纯平面块）；② 单材质槽（多材质不能合并——
///      程序化网格只有 1 子网格，无法表达多材质槽）；③ 不透明；④ 无起伏
///      （scale.y≈1，即纯平面四边形，无高度变化）
bool IsMergeableGround(const std::string &stem, const std::vector<std::string> &materials, const DirectX::XMFLOAT3 &scl,
                       const DirectX::XMFLOAT4 &rot, bool transparent) {
    if (stem.rfind("mapChip", 0) != 0)
        return false; // 仅 mapChip 系列（纯平面块）
    if (transparent)
        return false; // 透明地面不合并
    if (materials.size() != 1)
        return false; // 仅单材质（材质贴图一致）
    if (std::fabs(scl.y - 1.0f) > 1e-4f)
        return false; // 无垂直起伏（纯平面）
    // 旋转限制：仅允许绕 Y 轴旋转（mapChip 平面在 XZ，绕 Y 旋转后仍水平）
    if (std::fabs(rot.x) > 1e-4f || std::fabs(rot.z) > 1e-4f)
        return false;
    return true;
}

/// PNG/BMP → DDS；源文件可能被 XOR 加密（魔数非明文）时先解密再转换
TextureConvertResult ConvertImageToDDS(const std::string &inputPath, const std::string &outputPath) {
    TextureConvertResult result;

    // 读取整个文件
    std::ifstream ifs(inputPath, std::ios::binary | std::ios::ate);
    if (!ifs) {
        result.error = "Cannot open image file";
        return result;
    }
    size_t size = static_cast<size_t>(ifs.tellg());
    ifs.seekg(0);
    std::vector<uint8_t> data(size);
    ifs.read(reinterpret_cast<char *>(data.data()), size);
    ifs.close();

    if (IsPlainImageMagic(data.data(), data.size())) {
        // 明文：直接转换
        return ConvertPNGToDDS(inputPath, outputPath);
    }

    // XOR 加密（UKW .png 亦可能加密，key 0x0B7E7759）→ 解密后再转
    XORCipher cipher(0x0B7E7759);
    cipher.DecryptBuffer(data.data(), data.size());
    if (!IsPlainImageMagic(data.data(), data.size())) {
        result.error = "Image is neither plain nor decryptable (bad magic)";
        return result;
    }

    // 解密结果写临时文件再转 DDS（ConvertPNGToDDS 直接读文件）
    fs::path tmpPath = fs::path(outputPath).string() + ".tmp";
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        if (!ofs) {
            result.error = "Cannot write temp file";
            return result;
        }
        ofs.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    result = ConvertPNGToDDS(tmpPath.string(), outputPath);
    std::error_code ec;
    fs::remove(tmpPath, ec);
    return result;
}

} // namespace

// ==========================================================================
// mpd2scene 主流程：拆解（piece .x → dxmesh + 材质去重 + 纹理解密）
//                  → 合成（MPD 实例 + SPT 环境 → scene.json）
// ==========================================================================

bool MapSceneConverter::Convert(const MapSceneOptions &options, MapSceneResult &result) {
    result = MapSceneResult{};
    const std::string &mapDir = options.mapDir;
    const std::string &outDir = options.outDir;
    const bool lh = options.leftHanded;

    fs::path mapPath(mapDir);
    fs::path outPath(outDir);
    if (!fs::is_directory(mapPath)) {
        result.error = "map_dir is not a directory: " + mapDir;
        return false;
    }
    std::string mapName = mapPath.filename().string();

    // ---- 1. 扫描输入目录：.mpd / Script.spt / *.x / Sky.png ----
    std::string mpdFile, sptFile, skyPng;
    for (const auto &entry : fs::directory_iterator(mapPath)) {
        if (!entry.is_regular_file())
            continue;
        std::string p = entry.path().string();
        std::string ext = ToLower(entry.path().extension().string());
        std::string fn = ToLower(entry.path().filename().string());
        if (ext == ".mpd" && mpdFile.empty())
            mpdFile = p;
        else if (fn == "script.spt")
            sptFile = p;
        else if (fn == "sky.png")
            skyPng = p;
    }
    if (mpdFile.empty()) {
        result.error = "no .mpd found in " + mapDir;
        return false;
    }

    // ---- 2. 解析 MPD（权威结构：唯一 piece 名 + 全部对象实例）----
    MPDSceneParser mpdParser;
    if (!mpdParser.ParseFile(mpdFile)) {
        result.error = "MPD parse failed: " + mpdParser.GetError();
        return false;
    }
    const MPDScene &mpd = mpdParser.GetResult();

    // ---- 3. 解析 SPT（环境注入；缺失仅警告，不阻塞）----
    SceneData spt;
    bool hasSpt = false;
    if (!sptFile.empty()) {
        ScriptSPTParser sptParser;
        if (sptParser.ParseFile(sptFile)) {
            spt = sptParser.GetResult();
            hasSpt = true;
        } else {
            std::cerr << "[mpd2scene] WARN: SPT parse failed: " << sptParser.GetError() << "\n";
        }
    }

    // ---- 4. 输出目录：Meshes/ Textures/（materials 内联，无 .mat）----
    fs::path meshesDir = outPath / "Meshes";
    fs::path texDir = outPath / "Textures";
    fs::create_directories(meshesDir);
    fs::create_directories(texDir);

    // ---- 5. 拆解阶段 ----
    // 唯一 piece = 被对象实例引用的 visualMesh（7623 实例共享 40 唯一 piece）
    std::set<std::string> uniquePieces;
    for (const auto &obj : mpd.objects) {
        if (obj.pieceID >= 0 && obj.pieceID < static_cast<int>(mpd.pieces.size())) {
            const std::string &v = mpd.pieces[obj.pieceID].visualMesh;
            if (!v.empty())
                uniquePieces.insert(v);
        }
    }

    // 材质去重表：内容 FNV-1a hash → matKey
    std::map<std::string, std::string> matKeyByHash;
    std::map<std::string, nlohmann::json> materials; // matKey → MaterialDefinition（内联）
    std::map<std::string, bool> matTransparent;      // matKey → 子网格材质 alpha<1（透明判定）
    int matCount = 0;

    auto registerMaterial = [&](const XFileMaterial &xm, const std::string &texKey) -> std::string {
        std::string content = MaterialContent(xm, texKey);
        std::string hash = ToHex(Fnv1a64(reinterpret_cast<const uint8_t *>(content.data()), content.size()));
        auto it = matKeyByHash.find(hash);
        if (it != matKeyByHash.end())
            return it->second;

        std::string matKey = "mat_" + hash.substr(0, 12);
        auto desc = xm.ToMaterialDesc();
        desc.textures.baseColor = texKey; // 纹理引用 → dependencies.textures key
        // BugFix: 有 baseColor 纹理时 tint 强制白（PBR tint×纹理模式，faceColor 常为黑
        // 导致 albedo=0、漫反射 RT 全黑，如 UKW 树的 faceColor=[0,0,0]）
        if (!texKey.empty())
            desc.params.baseColor[0] = desc.params.baseColor[1] = desc.params.baseColor[2] = 1.0f;
        desc.hash = hash;
        materials[matKey] = MaterialToJSON(desc, texKey);
        matTransparent[matKey] = xm.faceColor[3] < 1.0f;
        matKeyByHash[hash] = matKey;
        matCount++;
        return matKey;
    };

    // 纹理：XOR 解密 → Textures/；返回 texKey（dependencies.textures key）
    std::map<std::string, std::string> texDeps;     // texKey → "Textures/xxx.dds"
    std::map<std::string, std::string> texKeyCache; // 源文件名 → texKey（去重）
    auto processTexture = [&](const std::string &texFilename) -> std::string {
        if (texFilename.empty())
            return {};
        std::string fn = fs::path(texFilename).filename().string();
        if (fn.empty())
            return {};
        auto cached = texKeyCache.find(fn);
        if (cached != texKeyCache.end())
            return cached->second;

        // 查找源文件：优先 map_dir 同目录（.x 与纹理同目录）
        fs::path src = mapPath / fn;
        if (!fs::exists(src))
            src = fs::path(texFilename); // 原样相对路径兜底
        if (!fs::exists(src)) {
            texKeyCache[fn] = {};
            return {};
        }

        std::string stem = fs::path(fn).stem().string();
        std::string ext = ToLower(fs::path(fn).extension().string());
        std::string texKey = stem;
        fs::path dst = texDir / (stem + ".dds");
        bool ok = false;
        std::string err;
        if (ext == ".dds") {
            auto r = DecryptOrCopyDDS(src.string(), dst.string());
            ok = r.success;
            err = r.error;
        } else if (ext == ".png" || ext == ".bmp") {
            auto r = ConvertImageToDDS(src.string(), dst.string());
            ok = r.success;
            err = r.error;
        }
        if (!ok) {
            texKeyCache[fn] = {};
            std::cerr << "[mpd2scene] WARN: texture conversion failed: " << fn;
            if (!err.empty())
                std::cerr << " (" << err << ")";
            std::cerr << "\n";
            return {};
        }
        texDeps[texKey] = "Textures/" + stem + ".dds";
        texKeyCache[fn] = texKey;
        return texKey;
    };

    // piece 拆解：合并子网格 → 单 dxmesh（SubMesh 表）+ 材质槽数组
    std::map<std::string, std::string> meshDeps;                   // pieceKey → "Meshes/xxx.dxmesh"
    std::map<std::string, std::vector<std::string>> pieceMatSlots; // pieceKey → 子网格材质 key 数组
    std::map<std::string, std::vector<XFileMesh>> meshCache;
    std::unordered_map<std::string, float> groundUVTiling; // stem → UV 平铺周期数（从 .x 解析，合并分段用）
    int pieceCount = 0;

    for (const auto &pieceName : uniquePieces) {
        std::string stem = fs::path(pieceName).stem().string();
        if (stem.empty())
            continue;
        // Sky.x / Hit.x 由 SPT 注入，不作为 piece 实体（MPD 亦无实例）
        std::string lower = ToLower(stem);
        if (lower == "sky" || lower == "hit")
            continue;
        // point_*（出生点标记，point_0~9 / point_rnd）与 item（物品放置标记）非场景几何，
        // 过滤（对齐 sky/hit 模式）。过滤后 pieceMatSlots 无对应条目，
        // 实例遍历阶段（pieceMatSlots.find 未命中）自动跳过，实体不输出。
        if (lower.rfind("point_", 0) == 0 || lower == "item")
            continue;

        // 加载 piece .x（带缓存；XFileParser/assimp 已按材质自动拆子网格）
        auto mc = meshCache.find(pieceName);
        if (mc == meshCache.end()) {
            std::string xPath = (mapPath / pieceName).string();
            std::vector<XFileMesh> parsed;
            XFileParser parser;
            if (parser.ParseFile(xPath) && !parser.GetMeshes().empty())
                parsed = parser.GetMeshes();
            if (parsed.empty()) {
                std::cerr << "[mpd2scene] WARN: piece .x parse failed: " << pieceName << "\n";
                meshCache[pieceName] = {};
                continue;
            }
            mc = meshCache.emplace(pieceName, std::move(parsed)).first;
        }
        const std::vector<XFileMesh> &meshes = mc->second;
        if (meshes.empty())
            continue;

        // 合并子网格：顶点/索引拼接 + SubMesh 表
        std::vector<DxMeshStaticVertex> verts;
        std::vector<uint32_t> indices;
        std::vector<DxMeshSubMesh> subMeshes;
        std::vector<std::string> matKeys;
        float bMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float bMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

        // 潜在可合并地面（mapChip 前缀）→ 统计 UV 平铺周期（自适应分段用：
        // 原始 .x 的 UV 可能是 0~10（每 3 单位一个纹理周期），合并块分段须对齐
        // 该密度，否则 30 单位/段时纹素密度不足视觉拉伸）
        float uvMaxU = 0.0f, uvMaxV = 0.0f;

        for (const auto &sm : meshes) {
            uint32_t vBase = static_cast<uint32_t>(verts.size());
            size_t vc = sm.VertexCount();
            for (size_t i = 0; i < vc; ++i) {
                DxMeshStaticVertex v{};
                v.position[0] = sm.positions[i * 3 + 0];
                v.position[1] = sm.positions[i * 3 + 1];
                v.position[2] = sm.positions[i * 3 + 2];
                if (sm.HasNormals()) {
                    v.normal[0] = sm.normals[i * 3 + 0];
                    v.normal[1] = sm.normals[i * 3 + 1];
                    v.normal[2] = sm.normals[i * 3 + 2];
                } else {
                    v.normal[1] = 1.0f;
                }
                v.tangentU[0] = 1.0f;
                if (sm.HasTexcoords()) {
                    v.texC[0] = sm.texcoords[i * 2 + 0];
                    // BugFix: UV V 轴翻转（assimp 读入 .x 后 V 为负值区间，如 mapChip06 V∈[-9,1]；
                    // 引擎采样约定 V 向下为正 [0,1]（程序化网格参照），需 v' = 1 - v 归一化到正区间）
                    v.texC[1] = 1.0f - sm.texcoords[i * 2 + 1];
                    // 统计 UV 平铺范围（仅潜在可合并地面；V 已翻转，uvMaxV 为正区间上界）
                    if (stem.rfind("mapChip", 0) == 0) {
                        uvMaxU = std::max(uvMaxU, v.texC[0]);
                        uvMaxV = std::max(uvMaxV, v.texC[1]);
                    }
                }
                // 右手 Y-up → 引擎左手系 Y-up：顶点/法线/切线 Z 取反（FbxMeshConverter §5）
                if (lh) {
                    v.position[2] = -v.position[2];
                    v.normal[2] = -v.normal[2];
                    v.tangentU[2] = -v.tangentU[2];
                }
                verts.push_back(v);
                bMin[0] = std::min(bMin[0], v.position[0]);
                bMin[1] = std::min(bMin[1], v.position[1]);
                bMin[2] = std::min(bMin[2], v.position[2]);
                bMax[0] = std::max(bMax[0], v.position[0]);
                bMax[1] = std::max(bMax[1], v.position[1]);
                bMax[2] = std::max(bMax[2], v.position[2]);
            }

            // 索引：.x 源为左手系（与 HOD/RobotMerger 同类），不翻转绕序。
            // 参照 RobotMerger（HOD/.x 源 leftHanded 仅顶点 Z 取反、索引直接拷贝）；
            // 仅 FbxMeshConverter（FBX 右手系源）需要 (i0,i2,i1) 翻转。
            size_t iBase = indices.size();
            for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                indices.push_back(vBase + sm.indices[i + 0]);
                indices.push_back(vBase + sm.indices[i + 1]);
                indices.push_back(vBase + sm.indices[i + 2]);
            }

            DxMeshSubMesh sub{};
            sub.indexOffset = static_cast<uint32_t>(iBase);
            sub.indexCount = static_cast<uint32_t>(sm.indices.size());
            sub.vertexOffset = vBase;
            subMeshes.push_back(sub);

            // 材质槽：纹理解密 + 内容 hash 去重
            std::string texKey = processTexture(sm.material.textureFilename);
            matKeys.push_back(registerMaterial(sm.material, texKey));
        }

        if (verts.empty() || indices.empty())
            continue;

        // 存储 UV 平铺周期（自适应分段）：原始 .x 的 UV 范围（如 mapChip06 U/V∈[0,10]）
        // → 每个 30 单位格内平铺 10 次 → 每 3 单位一个纹理周期。
        // 合并块分段 = 格数 × 平铺周期，保证纹素密度与原 .x 一致（256² / 3 单位 ≈ 85 纹素/单位）
        if (stem.rfind("mapChip", 0) == 0) {
            float tiling = std::max(uvMaxU, uvMaxV);
            if (tiling < 1.0f)
                tiling = 1.0f;
            groundUVTiling[stem] = tiling;
        }

        // 写 dxmesh（DxMeshWriter 支持 SubMesh 表；无骨骼 → 静态顶点 44B）
        std::string meshPath = (meshesDir / (stem + ".dxmesh")).string();
        if (!DX12Engine::Asset::DxMeshWriter::Write(verts.data(), verts.size(), sizeof(DxMeshStaticVertex),
                                                    indices.data(), indices.size(), 4, Utf8ToWide(meshPath), bMin, bMax,
                                                    false, subMeshes.data(), static_cast<uint32_t>(subMeshes.size()))) {
            std::cerr << "[mpd2scene] WARN: dxmesh write failed: " << meshPath << "\n";
            continue;
        }
        meshDeps[stem] = "Meshes/" + stem + ".dxmesh";
        pieceMatSlots[stem] = matKeys;
        pieceCount++;
    }

    // ---- 6. 天空盒纹理：直接利用同目录 Sky.png → dependencies.textures ----
    std::string skyTexKey;
    if (!skyPng.empty()) {
        std::error_code ec;
        fs::path dst = texDir / "Sky.png";
        fs::copy_file(skyPng, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            skyTexKey = "Sky";
            texDeps[skyTexKey] = "Textures/Sky.png";
        }
    }

    // ---- 7. 合成 scene.json（符合 Schemas/scene.schema.json）----
    nlohmann::json scene;
    scene["version"] = 1;
    scene["baseURL"] = "Content/" + mapName;
    scene["metadata"]["name"] = mapName;
    scene["metadata"]["description"] = "Converted from UKW map " + mapName + " (mpd2scene)";

    // sceneEnvironment：环境光 + 天空盒（SPT 注入；Hit/水/BGM/MapSetting 不转换）
    auto &env = scene["sceneEnvironment"];
    if (hasSpt) {
        env["ambient"]["ambientLight"] = {spt.lightR / 255.0f, spt.lightG / 255.0f, spt.lightB / 255.0f, 1.0f};
        nlohmann::json sb;
        if (!skyTexKey.empty())
            sb["texture"] = skyTexKey;
        sb["geometry"]["type"] = "cube"; // 程序化几何，引擎 skybox 程序化驱动
        sb["color"] = {spt.fogR / 255.0f, spt.fogG / 255.0f, spt.fogB / 255.0f, 1.0f}; // LoadSkyXFile 颜色兜底
        env["skybox"] = sb;
    } else {
        env["ambient"]["ambientLight"] = {1.0f, 1.0f, 1.0f, 1.0f};
    }

    // dependencies：piece dxmesh + 去重纹理
    if (!meshDeps.empty())
        scene["dependencies"]["meshes"] = meshDeps;
    if (!texDeps.empty())
        scene["dependencies"]["textures"] = texDeps;

    // materials：内联展开 MaterialDefinition（定案：不输出独立 .mat）
    scene["materials"] = materials;

    // entities：MPD 对象实例（矩阵→TRS + 左手系 Z 翻转）
    auto &entities = scene["entities"] = nlohmann::json::array();
    int instanceCount = 0;
    // Phase B：SOA 二进制收集器（.scene.bin——AoS→SOA 字段数组，体积 ~9% of JSON）
    std::vector<uint64_t> soaPid;
    std::vector<uint32_t> soaMeshIdx;
    std::vector<float> soaPos, soaRot, soaScl, soaCull;
    std::unordered_map<std::string, uint32_t> soaMeshMap; // stem → mesh 索引
    // 水区块合并（Sea 实例——邻接合并成一块水四边形——MPD 30 单位网格对齐）
    std::unordered_map<int64_t, uint32_t> seaGrid; // gridKey(bx,bz) → Sea 实例索引（flood fill 合并用）
    std::vector<DirectX::XMFLOAT3> seaPositions;   // Sea 实例位置（30 单位网格对齐）
    std::vector<DxSceneWaterBlock> waterBlocksBin; // 二进制水块（.scene 输出——DxSceneWaterBlock）
    // 地面合并（mapChip 单材质纯平面——邻接合并成程序化四边形块，对齐水区块模式）
    struct GroundCell {
        int bx = 0, bz = 0;
        float y = 0.0f;
    };
    std::unordered_map<std::string, std::vector<GroundCell>> groundGrid;       // stem → 网格单元
    std::unordered_map<std::string, std::vector<std::string>> groundMaterials; // stem → 材质槽（单材质）
    // 材质表收集（实体 mesh 材质槽——DxScene 版本 2：材质引用与 JSON materials 对齐）
    std::unordered_map<std::string, uint32_t> soaMaterialMap; // 材质名 → 索引
    std::vector<uint32_t> soaMaterialIdx;                     // 实体 mesh 材质引用（UINT32_MAX = 无）
    for (const auto &obj : mpd.objects) {
        if (obj.pieceID < 0 || obj.pieceID >= static_cast<int>(mpd.pieces.size()))
            continue;
        const std::string &pieceName = mpd.pieces[obj.pieceID].visualMesh;
        if (pieceName.empty())
            continue;
        std::string stem = fs::path(pieceName).stem().string();
        auto slotIt = pieceMatSlots.find(stem);
        if (slotIt == pieceMatSlots.end())
            continue; // 该 piece 未拆解成功

        // 透明判定：对象脚本 @AlphaTestFlag 或子网格材质 alpha<1
        bool transparent = false;
        std::string scriptLower = ToLower(obj.script);
        if (scriptLower.find("@alphatestflag") != std::string::npos)
            transparent = true;
        if (!transparent) {
            for (const auto &mk : slotIt->second) {
                auto it = matTransparent.find(mk);
                if (it != matTransparent.end() && it->second) {
                    transparent = true;
                    break;
                }
            }
        }

        // 剔除距离（MPD 明文 @CullFar=N；球体剔除——距离超过则看不清，强制剔除。
        // 数据来源：MPD 文本标记区 Shift-JIS → CP932ToUTF8 → obj.script，见 MPD_Format_Analysis.md §6）
        float cullDistance = 0.0f;
        {
            auto pos = scriptLower.find("@cullfar");
            if (pos != std::string::npos) {
                pos += 8; // 跳过 "@cullfar"
                while (pos < scriptLower.size() && (scriptLower[pos] == '=' || scriptLower[pos] == ' '))
                    ++pos;
                cullDistance = static_cast<float>(std::atof(scriptLower.c_str() + pos));
            }
        }

        // 矩阵 → TRS：MPD 16 float 列主序（列向量约定 world=M·v，平移列 m03/m13/m23）。
        // XMFLOAT4X4 行主序（行向量约定 v'·M，平移在 _41/_42/_43）：
        // 列主序 M 按行主序逐位复制 = M^T，XMMatrixDecompose 提取的平移即 m[12]/m[13]/m[14]。
        XMFLOAT4X4 f{};
        std::memcpy(&f, obj.matrix, sizeof(f));
        XMMATRIX M = XMLoadFloat4x4(&f);
        if (lh) {
            // 左手系：整体矩阵 Z 列取反（含平移 z），与顶点翻转同步（FbxMeshConverter §5）
            M.r[0].m128_f32[2] = -M.r[0].m128_f32[2];
            M.r[1].m128_f32[2] = -M.r[1].m128_f32[2];
            M.r[2].m128_f32[2] = -M.r[2].m128_f32[2];
            M.r[3].m128_f32[2] = -M.r[3].m128_f32[2];
        }
        XMVECTOR sVec, rVec, tVec;
        XMFLOAT3 s3{}, t3{};
        XMFLOAT4 r4{0.0f, 0.0f, 0.0f, 1.0f};
        if (XMMatrixDecompose(&sVec, &rVec, &tVec, M)) {
            XMStoreFloat3(&s3, sVec);
            XMStoreFloat3(&t3, tVec);
            XMStoreFloat4(&r4, rVec);
        }

        nlohmann::json e;
        char nameBuf[64];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s_%04d", stem.c_str(), instanceCount + 1);
        e["name"] = nameBuf;
        auto &comp = e["components"];
        comp["transform"]["position"] = {t3.x, t3.y, t3.z};
        comp["transform"]["rotation"] = {r4.x, r4.y, r4.z, r4.w};
        comp["transform"]["scale"] = {s3.x, s3.y, s3.z};
        comp["mesh"]["geometry"] = stem;
        comp["mesh"]["materials"] = slotIt->second;
        comp["mesh"]["receivesShadow"] = true;
        // 剔除距离承载于变换组件（缩放联动：有效距离 = cullDistance × maxScale，见 TransformComponent）。
        // MPD 的 @CullFar 是每 tile 实例的通用合理参数（球体剔除），无条件赋予所有解析到的实体
        // （无 @CullFar 时值为 0 = 不限制），保证 scene.json 数据完整性
        comp["transform"]["cullDistance"] = cullDistance;
        if (transparent)
            comp["transparent"] = nullptr;
        else
            comp["opaque"] = nullptr;
        if (stem == "Sea") {
            // Sea 实例不输出为普通实体，只收集到 seaGrid 做水块合并
            const int bx = (int)std::floor(t3.x / 30.0f);
            const int bz = (int)std::floor(t3.z / 30.0f);
            seaGrid[((int64_t)bx << 32) | static_cast<uint32_t>(bz)] = static_cast<uint32_t>(seaPositions.size());
            seaPositions.push_back(t3);
        } else if (IsMergeableGround(stem, slotIt->second, s3, r4, transparent)) {
            // 可合并地面：mapChip 单材质纯平面 → 收集到 groundGrid，合并阶段生成程序化四边形块
            // 覆盖格 = 世界矩阵变换 4 角点 (0,0,0)~(30,0,30) 的 bbox 展开：
            // mapChip 几何从角点展开 + 镜像(scale=-1)/旋转(rot180)会改变实际覆盖，
            // 只用 floor(t3/30) 单点定位会错位（如 rot180 翻转 Z → 实际覆盖 [pos.z-30,pos.z]，
            // 单点得 -64 而实际覆盖 -65 格 → 边缘格丢失 → 空白）。展开覆盖格保证完整。
            const DirectX::XMFLOAT3 corners[4] = {
                {0.0f, 0.0f, 0.0f}, {30.0f, 0.0f, 0.0f}, {30.0f, 0.0f, 30.0f}, {0.0f, 0.0f, 30.0f}};
            float minWx = FLT_MAX, maxWx = -FLT_MAX, minWz = FLT_MAX, maxWz = -FLT_MAX;
            for (const auto &c : corners) {
                XMVECTOR wp = XMVector3Transform(XMLoadFloat3(&c), M);
                minWx = (std::min)(minWx, XMVectorGetX(wp));
                maxWx = (std::max)(maxWx, XMVectorGetX(wp));
                minWz = (std::min)(minWz, XMVectorGetZ(wp));
                maxWz = (std::max)(maxWz, XMVectorGetZ(wp));
            }
            const int minBx = (int)std::floor(minWx / 30.0f);
            const int maxBx = (int)std::ceil(maxWx / 30.0f) - 1;
            const int minBz = (int)std::floor(minWz / 30.0f);
            const int maxBz = (int)std::ceil(maxWz / 30.0f) - 1;
            for (int bx = minBx; bx <= maxBx; ++bx)
                for (int bz = minBz; bz <= maxBz; ++bz)
                    groundGrid[stem].push_back({bx, bz, t3.y});
            if (groundMaterials.find(stem) == groundMaterials.end())
                groundMaterials[stem] = slotIt->second;
            instanceCount++; // 保持 PID 序列连续（合并实体仍占一个 PID）
        } else {
            entities.push_back(e);
            // Phase B：SOA 收集（按实体索引对齐）
            auto itM = soaMeshMap.find(stem);
            if (itM == soaMeshMap.end())
                itM = soaMeshMap.emplace(stem, static_cast<uint32_t>(soaMeshMap.size())).first;
            soaPid.push_back(static_cast<uint64_t>(instanceCount + 1));
            soaMeshIdx.push_back(itM->second);
            soaPos.insert(soaPos.end(), {t3.x, t3.y, t3.z});
            soaRot.insert(soaRot.end(), {r4.x, r4.y, r4.z, r4.w});
            soaScl.insert(soaScl.end(), {s3.x, s3.y, s3.z});
            soaCull.push_back(cullDistance);
            // 材质引用收集
            {
                uint32_t matIdx = UINT32_MAX;
                if (!slotIt->second.empty()) {
                    auto itM = soaMaterialMap.find(slotIt->second[0]);
                    if (itM == soaMaterialMap.end())
                        itM = soaMaterialMap.emplace(slotIt->second[0], static_cast<uint32_t>(soaMaterialMap.size()))
                                  .first;
                    matIdx = itM->second;
                }
                soaMaterialIdx.push_back(matIdx);
            }
            instanceCount++;
        }
    }

    // 方向光（Sun——directional，独立于环境光：SPT lightR/G/B 是环境光（ambient），非方向光。
    // MPD 无方向光信息——默认配置（斜 45° + 强度 5——参照 mini_city Sun））
    {
        nlohmann::json sun;
        sun["name"] = "Sun";
        char sunPid[17];
        std::snprintf(sunPid, sizeof(sunPid), "%016llx", static_cast<unsigned long long>(instanceCount + 1));
        sun["persistentId"] = sunPid;
        auto &sc = sun["components"];
        sc["transform"]["position"] = {0.0f, 406.23f, 0.0f};
        sc["transform"]["rotation"] = {0.3826833963394165f, 0.0f, 0.0f, 0.9238795042037964f}; // 斜 45°
        sc["transform"]["scale"] = {1.0f, 1.0f, 1.0f};
        sc["transform"]["cullDistance"] = 0;
        sc["light"]["type"] = "directional";
        sc["light"]["color"] = {1.0f, 1.0f, 0.9f, 1.0f};
        sc["light"]["intensity"] = 5;
        entities.push_back(std::move(sun));
    }

    // ---- 8. 写 scene.json（outDir/Scenes/{mapName}.scene.json）----
    // 水区块：2×2 网格分区（按 Sea 实例的总体范围均分，每块独立 bbox/world/tiling）
    // 替代 flood fill 全局合并——2×2 可分离外围海与内湖，参数独立
    if (!seaGrid.empty()) {
        // 水位基准：Sea 实例自身的最低 Y（对齐 MPD 渲染设置 #WaterY；
        // 水块可能合并多个 Sea 实例，取最低点最安全——水面高于合并集内所有 Sea 的底，
        // 避免个别较高 Sea 实例导致水面悬空/越过地形；编辑器后续可手动微调）
        float waterY = 0.0f;
        if (!seaPositions.empty()) {
            waterY = seaPositions[0].y;
            for (const auto &p : seaPositions)
                waterY = (std::min)(waterY, p.y);
        }
        // 计算总体范围
        int gMinBx = 1 << 30, gMinBz = 1 << 30, gMaxBx = -(1 << 30), gMaxBz = -(1 << 30);
        for (const auto &[key, _] : seaGrid) {
            const int bx = (int)(key >> 32);
            const int bz = (int)(uint32_t)key;
            gMinBx = (std::min)(gMinBx, bx);
            gMaxBx = (std::max)(gMaxBx, bx);
            gMinBz = (std::min)(gMinBz, bz);
            gMaxBz = (std::max)(gMaxBz, bz);
        }
        // 2×2 分割线：中分 X 和 Z 范围
        const int midBx = (gMinBx + gMaxBx) / 2;
        const int midBz = (gMinBz + gMaxBz) / 2;
        // 4 个象限的网格范围
        struct Quadrant {
            int minBx, maxBx, minBz, maxBz;
        };
        Quadrant quads[4] = {
            {gMinBx, midBx, gMinBz, midBz},         // NW
            {midBx + 1, gMaxBx, gMinBz, midBz},     // NE
            {gMinBx, midBx, midBz + 1, gMaxBz},     // SW
            {midBx + 1, gMaxBx, midBz + 1, gMaxBz}, // SE
        };
        int waterIdx = 0; // 水实体序号（标准实体命名 WaterBlock_N）
        for (int qi = 0; qi < 4; ++qi) {
            const auto &q = quads[qi];
            // 检查该象限内是否有 Sea 网格
            bool hasWater = false;
            for (const auto &[key, _] : seaGrid) {
                const int bx = (int)(key >> 32);
                const int bz = (int)(uint32_t)key;
                if (bx >= q.minBx && bx <= q.maxBx && bz >= q.minBz && bz <= q.maxBz) {
                    hasWater = true;
                    break;
                }
            }
            if (!hasWater)
                continue;
            const float w = (q.maxBx - q.minBx + 1) * 30.0f;
            const float h = (q.maxBz - q.minBz + 1) * 30.0f;
            const float posX = (q.minBx + q.maxBx + 1) * 15.0f;
            const float posZ = (q.minBz + q.maxBz + 1) * 15.0f;

            // ── 标准水实体（材质槽 + 虚拟引用模式，对齐 RenderPipelineSpecification §8）──
            // 不再输出 waterBlocks 数组（旧格式）——水实体直接写入 entities：
            // transform + mesh.geometry=procedural:// 虚拟引用 + water 组件 + Water 材质槽
            nlohmann::json waterEntity;
            waterEntity["name"] = "WaterBlock_" + std::to_string(waterIdx);
            char pid[17];
            std::snprintf(pid, sizeof(pid), "%016llx", static_cast<unsigned long long>(instanceCount + 1 + waterIdx));
            waterEntity["persistentId"] = pid;
            auto &wc = waterEntity["components"];
            wc["transform"]["position"] = {posX, waterY, posZ};
            wc["transform"]["rotation"] = {0.0f, 0.0f, 0.0f, 1.0f};
            wc["transform"]["scale"] = {1.0f, 1.0f, 1.0f};
            wc["transform"]["cullDistance"] = 5000.0f;
            char uri[128];
            std::snprintf(uri, sizeof(uri), "procedural://grid/%d/%d/32/32", (int)w, (int)h);
            wc["mesh"]["geometry"] = uri;
            wc["mesh"]["materials"] = {"Water"};
            wc["water"]["amplitude"] = 0.5f;
            wc["water"]["frequency"] = 1.0f;
            wc["water"]["speed"] = 0.5f;
            wc["water"]["direction"] = 0.0f;
            entities.push_back(std::move(waterEntity));
            instanceCount++;
            waterIdx++;

            // 二进制水块（.scene 二进制仍保留 DxSceneWaterBlock——旧格式兼容）
            DxSceneWaterBlock wbBin{};
            wbBin.minX = (float)q.minBx * 30.0f;
            wbBin.minZ = (float)q.minBz * 30.0f;
            wbBin.maxX = (float)(q.maxBx + 1) * 30.0f;
            wbBin.maxZ = (float)(q.maxBz + 1) * 30.0f;
            wbBin.posX = posX;
            wbBin.posY = waterY; // 水位基准（Sea 实例最低值，对齐 MPD #WaterY）
            wbBin.posZ = posZ;
            wbBin.scaleX = w;
            wbBin.scaleY = 1.0f;
            wbBin.scaleZ = h;
            wbBin.tilingX = w / 30.0f;
            wbBin.tilingZ = h / 30.0f;
            waterBlocksBin.push_back(wbBin);
        }
        // JSON 不再输出 waterBlocks 数组——水实体已直接写入 entities（标准实体 + 虚拟引用）
        // 补 water 材质（水实体构建时依赖此材质路由到 ShaderType::Water）
        // textures.baseColor="sea" 引用 dependencies.textures 的 sea 纹理——
        // 缺失会导致水渲染跳过纹理采样（BaseColorTexIndex=0xFFFFFFFF）→ 水暗淡
        if (!scene["materials"].contains("Water")) {
            nlohmann::json waterMat;
            waterMat["shader"] = "Water/Sim";
            waterMat["params"]["baseColor"] = {0.2f, 0.5f, 0.7f, 0.6f};
            waterMat["params"]["roughness"] = 0.1f;
            waterMat["textures"]["baseColor"] = "sea";
            scene["materials"]["Water"] = std::move(waterMat);
        }
    }

    // ---- 9. 地面合并：mapChip 单材质纯平面 → 程序化四边形块 ----
    // 对每个可合并 stem，贪心最大矩形分解（30 单位网格）：
    //   ① 按 y 分组（纯平面无起伏——同 y 才可合并）
    //   ② 占用网格上反复提取最大全占矩形，直到无剩余格
    //   ③ 每个矩形生成一个程序化实体（procedural://grid/...），替换原始 mapChip 实例
    for (auto &[stem, cells] : groundGrid) {
        if (cells.empty())
            continue;
        auto matIt = groundMaterials.find(stem);
        if (matIt == groundMaterials.end())
            continue;

        // 按 y 分组（同高度层）
        std::map<float, std::vector<GroundCell>> byY;
        for (const auto &c : cells)
            byY[c.y].push_back(c);

        for (auto &[yLevel, yCells] : byY) {
            if (yCells.empty())
                continue;

            // 占用集合（gridKey）
            std::unordered_set<int64_t> occupied;
            for (const auto &c : yCells)
                occupied.insert(((int64_t)c.bx << 32) | static_cast<uint32_t>(c.bz));

            // bbox（占用格的范围）
            int minBx = 1 << 30, maxBx = -(1 << 30), minBz = 1 << 30, maxBz = -(1 << 30);
            for (const auto &k : occupied) {
                const int bx = (int)(k >> 32), bz = (int)(uint32_t)k;
                minBx = (std::min)(minBx, bx);
                maxBx = (std::max)(maxBx, bx);
                minBz = (std::min)(minBz, bz);
                maxBz = (std::max)(maxBz, bz);
            }
            const int W = maxBx - minBx + 1, H = maxBz - minBz + 1;
            if (W <= 0 || H <= 0)
                continue;

            // 0/1 占用矩阵
            std::vector<std::vector<bool>> gridMat(H, std::vector<bool>(W, false));
            for (const auto &k : occupied) {
                const int bx = (int)(k >> 32), bz = (int)(uint32_t)k;
                gridMat[bz - minBz][bx - minBx] = true;
            }

            // 贪心最大矩形分解：直方图法（逐行 heights → 每行求最大矩形）
            // 每轮提取当前最大全占矩形 → 置 false → 直到全空
            int rectIdx = 0;
            while (true) {
                // 直方图（每列连续 true 高度）
                std::vector<int> heights(W, 0);
                int bestArea = 0, bestTop = -1, bestLeft = -1, bestRight = -1, bestBottom = -1;
                for (int r = 0; r < H; ++r) {
                    for (int c = 0; c < W; ++c)
                        heights[c] = gridMat[r][c] ? heights[c] + 1 : 0;
                    // 对当前行求最大矩形（单调栈近似——O(W^2) 足够小）
                    for (int c1 = 0; c1 < W; ++c1) {
                        int minH = 1 << 30;
                        for (int c2 = c1; c2 < W; ++c2) {
                            minH = (std::min)(minH, heights[c2]);
                            if (minH == 0)
                                break;
                            const int area = minH * (c2 - c1 + 1);
                            if (area > bestArea) {
                                bestArea = area;
                                bestLeft = c1;
                                bestRight = c2;
                                bestTop = r - minH + 1;
                                bestBottom = r;
                            }
                        }
                    }
                }
                if (bestArea <= 0)
                    break; // 无剩余可合并格

                // 标记该矩形为已分配
                for (int r = bestTop; r <= bestBottom; ++r)
                    for (int c = bestLeft; c <= bestRight; ++c)
                        gridMat[r][c] = false;

                // 生成程序化四边形实体
                const int rectBx = minBx + bestLeft; // 矩形最小格 X
                const int rectBz = minBz + bestTop;  // 矩形最小格 Z
                const int rectW = bestRight - bestLeft + 1;
                const int rectH = bestBottom - bestTop + 1;
                const float worldW = rectW * 30.0f;
                const float worldH = rectH * 30.0f;
                const float cx = (float)(rectBx * 30) + worldW * 0.5f;
                const float cz = (float)(rectBz * 30) + worldH * 0.5f;

                nlohmann::json ge;
                ge["name"] = stem + "_merged_" + std::to_string(rectIdx++);
                auto &gc = ge["components"];
                gc["transform"]["position"] = {cx, yLevel, cz};
                gc["transform"]["rotation"] = {0.0f, 0.0f, 0.0f, 1.0f};
                gc["transform"]["scale"] = {1.0f, 1.0f, 1.0f};
                // 程序化四边形——分段 = 格数 × 3（每 10 单位一个 quad）：
                // 原始纹理仅 256²，30 单位/段时纹素密度低（256/30≈8.5 纹素/单位），
                // 视觉近看像整块拉伸。10 单位/段 → 256/10≈25.6 纹素/单位，纹理清晰。
                char uri[128];
                // 自适应分段：从原始 .x UV 平铺周期推导（groundUVTiling[stem]）——
                // 每个 30 单位格内 UV 平铺 N 次 → 每 30/N 单位一个纹理周期 → 分段 = 格数 × N。
                // 对齐原始纹素密度（mapChip06: UV 0~10 → 分段 ×10 = 每 3 单位一个 quad）。
                // 回退默认 3（无 UV 数据时 10 单位/quad，纹素密度 256/10≈25.6/单位）。
                float tiling = 3.0f;
                auto tit = groundUVTiling.find(stem);
                if (tit != groundUVTiling.end())
                    tiling = tit->second;
                std::snprintf(uri, sizeof(uri), "procedural://grid/%d/%d/%d/%d", (int)worldW, (int)worldH,
                              (int)(rectW * tiling), (int)(rectH * tiling));
                gc["mesh"]["geometry"] = uri;
                gc["mesh"]["materials"] = matIt->second;
                gc["mesh"]["receivesShadow"] = true;
                gc["opaque"] = nullptr;
                entities.push_back(std::move(ge));
                instanceCount++;

                // 从占用集合移除该矩形（后续矩形不重叠）
                for (int r = bestTop; r <= bestBottom; ++r)
                    for (int c = bestLeft; c <= bestRight; ++c)
                        occupied.erase(((int64_t)(minBx + c) << 32) | static_cast<uint32_t>(minBz + r));
            }

            // 剩余格（非矩形残余）→ 保留为原始 mapChip 实体（不可合并）
            for (const auto &k : occupied) {
                // 残余格已经不会再进入（occupied 每轮清空矩形内格；若存在孤立残余，
                // 它们会在后续轮次作为 1×1 矩形处理，不会遗漏）
                (void)k;
            }
        }
    }

    fs::path scenesDir = outPath / "Scenes";
    fs::create_directories(scenesDir);
    std::string scenePath = (scenesDir / (mapName + ".scene.json")).string();
    std::ofstream ofs(scenePath);
    if (!ofs) {
        result.error = "failed to write scene.json: " + scenePath;
        return false;
    }
    ofs << scene.dump(2);

    // 环境段数据（版本 2——sceneEnvironment：ambient/skybox/entityMotionPolicy——二进制不丢 JSON 架构内容）
    std::vector<float> envAmbient = {1.0f, 1.0f, 1.0f, 1.0f};
    std::string envPolicy; // entityMotionPolicy（空 = 默认 "static"）
    std::string envSkybox; // 天空盒纹理名（空 = 无）
    {
        if (scene.contains("sceneEnvironment") && scene["sceneEnvironment"].is_object()) {
            const auto &se = scene["sceneEnvironment"];
            if (se.contains("ambient") && se["ambient"].contains("ambientLight") &&
                se["ambient"]["ambientLight"].is_array()) {
                const auto &al = se["ambient"]["ambientLight"];
                if (al.size() >= 4)
                    envAmbient = {al[0].get<float>(), al[1].get<float>(), al[2].get<float>(), al[3].get<float>()};
            }
            if (se.contains("entityMotionPolicy") && se["entityMotionPolicy"].is_string())
                envPolicy = se["entityMotionPolicy"].get<std::string>();
            if (se.contains("skybox") && se["skybox"].is_object() && se["skybox"].contains("texture") &&
                se["skybox"]["texture"].is_string())
                envSkybox = se["skybox"]["texture"].get<std::string>();
        }
    }

    // Phase B：SOA 二进制 .scene（DxSceneFormat.h 规范——DXSCENE 魔数 + Header + SOA 字段数组；
    // 体积 ~9% of JSON；引擎 memcpy 直接加载字段数组）
    {
        std::string binPath = scenePath;
        // City.scene.json → City.scene（替换整个 .scene.json 后缀——避免 .scene.scene 重复后缀）
        auto extPos = binPath.rfind(".scene.json");
        if (extPos != std::string::npos)
            binPath = binPath.substr(0, extPos) + ".scene";
        else
            binPath += ".scene";
        std::ofstream bofs(binPath, std::ios::binary);
        if (bofs) {
            const uint32_t version = DX_SCENE_VERSION;
            const uint32_t count = static_cast<uint32_t>(soaPid.size());
            // mesh 名表（按 soaMeshIdx 索引对齐——引擎加载需 geometry 名）
            std::vector<std::string> meshNames(soaMeshMap.size());
            for (const auto &kv : soaMeshMap)
                meshNames[kv.second] = kv.first;
            const uint32_t meshCount = static_cast<uint32_t>(meshNames.size());
            DxSceneHeader header{};
            std::memcpy(header.magic, DX_SCENE_MAGIC, 8);
            header.version = version;
            header.entityCount = count;
            header.meshCount = meshCount;
            header.materialCount = static_cast<uint32_t>(soaMaterialMap.size());   // 材质表（版本 2）
            header.waterBlockCount = static_cast<uint32_t>(waterBlocksBin.size()); // 水块数组（邻接 Sea 合并）
            header.flags = 0;
            if (!envAmbient.empty() || !envPolicy.empty())
                header.flags |= DxSceneFlag_HasEnvironment;
            if (!envSkybox.empty())
                header.flags |= DxSceneFlag_HasSkybox;
            bofs.write(reinterpret_cast<const char *>(&header), sizeof(DxSceneHeader));
            for (const auto &nm : meshNames) { // mesh 名表（u16 len + 字节）
                const uint16_t len = static_cast<uint16_t>(nm.size());
                bofs.write(reinterpret_cast<const char *>(&len), 2);
                bofs.write(nm.data(), len);
            }
            // 材质表（版本 2——材质名按 soaMaterialMap 索引对齐，u16 len + 字节）
            {
                std::vector<std::string> matNames(soaMaterialMap.size());
                for (const auto &kv : soaMaterialMap)
                    matNames[kv.second] = kv.first;
                for (const auto &mn : matNames) {
                    const uint16_t len = static_cast<uint16_t>(mn.size());
                    bofs.write(reinterpret_cast<const char *>(&len), 2);
                    bofs.write(mn.data(), len);
                }
            }
            bofs.write(reinterpret_cast<const char *>(soaPid.data()), count * 8);
            bofs.write(reinterpret_cast<const char *>(soaMeshIdx.data()), count * 4);
            bofs.write(reinterpret_cast<const char *>(soaMaterialIdx.data()), count * 4); // 材质引用（版本 2）
            bofs.write(reinterpret_cast<const char *>(soaPos.data()), count * 12);
            bofs.write(reinterpret_cast<const char *>(soaRot.data()), count * 16);
            bofs.write(reinterpret_cast<const char *>(soaScl.data()), count * 12);
            bofs.write(reinterpret_cast<const char *>(soaCull.data()), count * 4);
            // 水块数组（邻接 Sea 合并——DxSceneWaterBlock：程序化水面四边形）
            if (!waterBlocksBin.empty())
                bofs.write(reinterpret_cast<const char *>(waterBlocksBin.data()),
                           static_cast<std::streamsize>(waterBlocksBin.size()) * sizeof(DxSceneWaterBlock));
            // 环境段（版本 2——ambient RGBA + entityMotionPolicy + skybox 纹理名——不丢 JSON 架构内容）
            if (header.flags & DxSceneFlag_HasEnvironment) {
                bofs.write(reinterpret_cast<const char *>(envAmbient.data()), 4 * sizeof(float));
                const uint16_t policyLen = static_cast<uint16_t>(envPolicy.size());
                bofs.write(reinterpret_cast<const char *>(&policyLen), 2);
                if (policyLen > 0)
                    bofs.write(envPolicy.data(), policyLen);
                if (header.flags & DxSceneFlag_HasSkybox) {
                    const uint16_t skyLen = static_cast<uint16_t>(envSkybox.size());
                    bofs.write(reinterpret_cast<const char *>(&skyLen), 2);
                    bofs.write(envSkybox.data(), skyLen);
                }
            }
        }
    }

    // ---- 9. 结果统计 ----
    result.pieceCount = pieceCount;
    result.instanceCount = instanceCount;
    result.materialCount = matCount;
    result.textureCount = static_cast<int>(texDeps.size());
    result.scenePath = scenePath;
    return true;
}

} // namespace AssetTool
