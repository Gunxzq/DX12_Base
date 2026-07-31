// ========================================================================
// AssetTool — UKW PowerUp Kit 资产离线转换工具
//
// 用法:
//   AssetTool x2mesh <input.x> <output.dxmesh>         — .x → .dxmesh
//   AssetTool x2scene <input.x> <output.scene.json>    — .x → scene.json
//   AssetTool hod2json <input.hod> <output.json>       — .hod → JSON
//   AssetTool hod2txt <input.hod> <output.txt>         — .hod → 可读文本
//   AssetTool spt2json <input.spt> <output.json>       — .spt → 场景JSON
//   AssetTool png2dds <input.png> <output.dds>         — PNG → DDS
//   AssetTool ddsdecrypt <input.dds> <output.dds>      — XOR 解密 DDS
//   AssetTool decrypt <input> <output>                  — XOR 解密
//   AssetTool batch <input_dir> <output_dir>            — 批量处理目录
// ========================================================================

#include "Core/RobotMerger.h"
#include "Core/HODParser.h"
#include "Core/ANIParser.h"
#include "Core/IKSolver.h"
#include "Core/MPDParser.h"
#include "Core/ScriptSPTParser.h"
#include "Core/TextureConverter.h"
#include "Core/XFileParser.h"
#include "Core/XORCipher.h"
#include "Asset/Definitions/Mesh/DxMeshFormat.h"
#include "Asset/IO/Writer/DxMeshWriter.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <cctype>

namespace fs = std::filesystem;

// ==========================================================================
// 辅助函数
// ==========================================================================

/// 确定性伪随机噪声（基于输入坐标返回 0..1 的稳定值，无状态依赖）
static float DeterministicNoise(int x, int y) {
    unsigned h = static_cast<unsigned>(x) * 374761393u + static_cast<unsigned>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return static_cast<float>(h) / 4294967295.0f; // 0..1 range over full uint32
}

static std::string GetExtension(const std::string &path) {
    auto ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

static std::string ReplaceExtension(const std::string &path, const std::string &newExt) {
    auto p = fs::path(path);
    return p.replace_extension(newExt).string();
}

static std::string GetStem(const std::string &path) {
    return fs::path(path).stem().string();
}

static bool ReadFile(const std::string &path, std::vector<uint8_t> &outData) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    outData.resize(size);
    file.read(reinterpret_cast<char *>(outData.data()), size);
    return true;
}

static bool WriteFile(const std::string &path, const void *data, size_t size) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char *>(data), size);
    return true;
}

// ==========================================================================
// 命令：x2mesh — .x → .dxmesh 转换
// ==========================================================================

static int CommandX2Mesh(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool x2mesh <input.x> <output.dxmesh>\n";
        return 1;
    }

    std::string inputPath = args[0];
    std::string outputPath = args[1];

    std::cout << "[x2mesh] Reading: " << inputPath << "\n";

    AssetTool::XFileParser parser;
    if (!parser.ParseFile(inputPath)) {
        std::cerr << "[x2mesh] Error: " << parser.GetError() << "\n";
        return 1;
    }

    const auto &meshes = parser.GetMeshes();
    if (meshes.empty()) {
        std::cerr << "[x2mesh] No meshes found in .x file\n";
        return 1;
    }

    std::cout << "[x2mesh] Found " << meshes.size() << " mesh(es)\n";

    for (size_t i = 0; i < meshes.size(); ++i) {
        const auto &mesh = meshes[i];
        std::string meshOutputPath = outputPath;

        // 多 mesh 时添加序号
        if (meshes.size() > 1) {
            auto p = fs::path(outputPath);
            std::string stem = p.stem().string();
            meshOutputPath = p.parent_path().string() + "/" + stem + "_" + std::to_string(i) + ".dxmesh";
        }

        std::cout << "[x2mesh]   Mesh " << i << ": "
                  << mesh.VertexCount() << " verts, "
                  << (mesh.indices.size() / 3) << " faces, "
                  << "1 material\n";

        if (!mesh.WriteDxMesh(meshOutputPath)) {
            std::cerr << "[x2mesh]   Failed to write: " << meshOutputPath << "\n";
            return 1;
        }
        std::cout << "[x2mesh]   Written: " << meshOutputPath << "\n";
    }

    return 0;
}

// ==========================================================================
// 命令：x2scene — .x → scene.json（含内联材质）
// ==========================================================================

static int CommandX2Scene(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool x2scene <input.x> <output.scene.json>\n";
        return 1;
    }

    std::string inputPath = args[0];
    std::string outputPath = args[1];

    std::cout << "[x2scene] Reading: " << inputPath << "\n";

    AssetTool::XFileParser parser;
    if (!parser.ParseFile(inputPath)) {
        std::cerr << "[x2scene] Error: " << parser.GetError() << "\n";
        return 1;
    }

    const auto &meshes = parser.GetMeshes();
    if (meshes.empty()) {
        std::cerr << "[x2scene] No meshes found\n";
        return 1;
    }

    // 构建 scene.json
    nlohmann::json scene;
    scene["version"] = 1;

    scene["metadata"]["name"] = GetStem(inputPath);
    scene["metadata"]["description"] = "Converted from " + fs::path(inputPath).filename().string();

    // 依赖
    auto &deps = scene["dependencies"];
    auto &depMeshes = deps["meshes"] = nlohmann::json::object();
    auto &depTextures = deps["textures"] = nlohmann::json::object();

    // 材质
    auto &materials = scene["materials"] = nlohmann::json::object();

    // 实体
    auto &entities = scene["entities"] = nlohmann::json::array();

    int textureCounter = 0;

    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto &mesh = meshes[mi];
        std::string meshKey = GetStem(inputPath);
        if (meshes.size() > 1)
            meshKey += "_" + std::to_string(mi);

        // 依赖：mesh路径
        std::string meshFileName = meshKey + ".dxmesh";
        depMeshes[meshKey] = "Models/" + meshFileName;

        // 材质（单个材质）
        {
            const auto &xMat = mesh.material;
            std::string matKey = meshKey + "_mat0";

            // 嵌入材质描述
            auto matDesc = xMat.ToMaterialDesc();
            nlohmann::json jMat;
            jMat["shader"] = matDesc.shader;

            auto &jParams = jMat["params"];
            jParams["baseColor"] = {matDesc.params.baseColor[0], matDesc.params.baseColor[1],
                                     matDesc.params.baseColor[2], matDesc.params.baseColor[3]};
            jParams["metallic"] = matDesc.params.metallic;
            jParams["roughness"] = matDesc.params.roughness;
            jParams["ao"] = matDesc.params.ao;

            // 纹理引用
            if (!xMat.textureFilename.empty()) {
                auto &jTex = jMat["textures"];
                std::string texKey = meshKey + "_tex" + std::to_string(textureCounter++);

                // 提取纹理文件名
                std::string texPath = xMat.textureFilename;
                // 如果是绝对路径或者包含路径，提取文件名
                std::string texName = fs::path(texPath).filename().string();
                depTextures[texKey] = "Textures/" + texName;
                jTex["baseColor"] = texKey;
            }

            materials[matKey] = jMat;
        }

        // 实体
        nlohmann::json entity;
        entity["name"] = meshKey;

        auto &comp = entity["components"];
        comp["transform"]["position"] = {0, 0, 0};
        comp["transform"]["rotation"] = {0, 0, 0, 1};
        comp["transform"]["scale"] = {1, 1, 1};

        auto &meshComp = comp["mesh"];
        meshComp["geometry"] = meshKey;
        meshComp["material"] = meshKey + "_mat0";

        entities.push_back(entity);
    }

    // 写入
    std::string jsonStr = scene.dump(2);
    if (!WriteFile(outputPath, jsonStr.data(), jsonStr.size())) {
        std::cerr << "[x2scene] Failed to write: " << outputPath << "\n";
        return 1;
    }

    std::cout << "[x2scene] Written: " << outputPath << "\n";
    std::cout << "[x2scene]   Meshes: " << meshes.size() << "\n";
    std::cout << "[x2scene]   Materials: " << materials.size() << "\n";

    return 0;
}

// ==========================================================================
// 命令：hod2json — .hod → JSON 骨架
// ==========================================================================

static int CommandHOD2JSON(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool hod2json <input.hod> <output.json>\n";
        return 1;
    }

    std::string inputPath = args[0];
    std::string outputPath = args[1];

    std::cout << "[hod2json] Reading: " << inputPath << "\n";

    AssetTool::HODParser parser;
    if (!parser.ParseFile(inputPath)) {
        std::cerr << "[hod2json] Error: " << parser.GetError() << "\n";
        return 1;
    }

    const auto &hod = parser.GetResult();
    std::cout << "[hod2json] Found " << hod.BoneCount() << " bones\n";

    if (!hod.WriteJSON(outputPath)) {
        std::cerr << "[hod2json] Failed to write: " << outputPath << "\n";
        return 1;
    }

    std::cout << "[hod2json] Written: " << outputPath << "\n";
    return 0;
}

// ==========================================================================
// 命令：hod2txt — .hod → 可读文本
// ==========================================================================

static int CommandHOD2Txt(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool hod2txt <input.hod> <output.txt>\n";
        return 1;
    }

    std::string inputPath = args[0];
    std::string outputPath = args[1];

    std::cout << "[hod2txt] Reading: " << inputPath << "\n";

    AssetTool::HODParser parser;
    if (!parser.ParseFile(inputPath)) {
        std::cerr << "[hod2txt] Error: " << parser.GetError() << "\n";
        return 1;
    }

    const auto &hod = parser.GetResult();

    if (!hod.WriteText(outputPath)) {
        std::cerr << "[hod2txt] Failed to write: " << outputPath << "\n";
        return 1;
    }

    std::cout << "[hod2txt] Written: " << outputPath << "\n";
    return 0;
}

// ==========================================================================
// 命令：mpd2txt — .mpd → 可读文本
// ==========================================================================

static int CommandMPD2Txt(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool mpd2txt <input.mpd> <output.txt> [asset_dir]\n"
                  << "       asset_dir: 可选，扫描该目录的 .x 文件过滤名字表\n";
        return 1;
    }

    std::string inputPath = args[0];
    std::string outputPath = args[1];

    AssetTool::MPDParser parser;
    if (!parser.ParseFile(inputPath)) {
        std::cerr << "[mpd2txt] Error: " << parser.GetError() << "\n";
        return 1;
    }

    auto mpd = parser.GetResult();

    // 自动扫描 MPD 所在目录的 .x 文件过滤名字表（也可以用 asset_dir 覆盖）
    fs::path mpdAbs = fs::absolute(inputPath);
    std::string assetDir = (args.size() >= 3) ? args[2] : mpdAbs.parent_path().string();
    std::vector<std::string> xFiles;
    if (fs::exists(assetDir)) {
        for (auto &entry : fs::directory_iterator(assetDir)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                if (ext.size() == 2) {
                    ext[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[0])));
                    ext[1] = static_cast<char>(std::tolower(static_cast<unsigned char>(ext[1])));
                }
                if (ext == ".x")
                    xFiles.push_back(entry.path().filename().string());
            }
        }
        std::cout << "[mpd2txt] Found " << xFiles.size() << " .x files in: " << assetDir << "\n";
        if (!xFiles.empty())
            mpd.FilterByExistingFiles(xFiles);
    } else {
        std::cerr << "[mpd2txt] Warning: directory not found: " << assetDir << "\n";
    }

    if (!mpd.WriteText(outputPath)) {
        std::cerr << "[mpd2txt] Failed to write: " << outputPath << "\n";
        return 1;
    }

    std::cout << "[mpd2txt] Written: " << outputPath
              << " (" << mpd.TileCount() << " tiles, "
              << mpd.tileNames.size() << " tile names)\n";
    return 0;
}

// ==========================================================================
// 命令：spt2json — Script.spt → scene.json
// ==========================================================================

static int CommandSPT2JSON(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool spt2json <Script.spt> <scene.json>\n";
        return 1;
    }

    AssetTool::ScriptSPTParser parser;
    if (!parser.ParseFile(args[0])) {
        std::cerr << "[spt2json] Error: " << parser.GetError() << "\n";
        return 1;
    }

    const auto &scene = parser.GetResult();
    if (!scene.WriteJSON(args[1])) {
        std::cerr << "[spt2json] Failed to write: " << args[1] << "\n";
        return 1;
    }

    std::cout << "[spt2json] Written: " << args[1]
              << " (" << scene.mapTiles.size() << " tiles, "
              << scene.buildings.size() << " buildings)\n";
    return 0;
}

// ==========================================================================
// 命令：decrypt — XOR 解密文件
// ==========================================================================

static int CommandDecrypt(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool decrypt <input> <output> [key_hex]\n";
        std::cerr << "  default key: 0x0B7E7759 (PowerUp Kit)\n";
        return 1;
    }

    std::string inputPath = args[0];
    std::string outputPath = args[1];
    uint32_t key = 0x0B7E7759;

    if (args.size() >= 3) {
        key = static_cast<uint32_t>(std::stoul(args[2], nullptr, 16));
    }

    std::cout << "[decrypt] Reading: " << inputPath << "\n";
    std::cout << "[decrypt] Key: 0x" << std::hex << key << std::dec << "\n";

    std::vector<uint8_t> data;
    if (!ReadFile(inputPath, data)) {
        std::cerr << "[decrypt] Cannot read file\n";
        return 1;
    }

    AssetTool::XORCipher cipher(key);
    cipher.DecryptBuffer(data.data(), data.size());

    if (!WriteFile(outputPath, data.data(), data.size())) {
        std::cerr << "[decrypt] Failed to write: " << outputPath << "\n";
        return 1;
    }

    std::cout << "[decrypt] Written: " << outputPath << " (" << data.size() << " bytes)\n";
    return 0;
}

// ==========================================================================
// 命令：png2dds — PNG → DDS 纹理转换
// ==========================================================================

static int CommandPNG2DDS(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool png2dds <input.png> <output.dds>\n";
        return 1;
    }

    auto result = AssetTool::ConvertPNGToDDS(args[0], args[1]);
    if (!result.success) {
        std::cerr << "[png2dds] Error: " << result.error << "\n";
        return 1;
    }

    std::cout << "[png2dds] Written: " << args[1] << "\n";
    return 0;
}

// ==========================================================================
// 命令：ddsdecrypt — XOR 解密 DDS 纹理
// ==========================================================================

static int CommandDDSDecrypt(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool ddsdecrypt <input.dds> <output.dds> [key_hex]\n";
        return 1;
    }

    uint32_t key = 0x0B7E7759;
    if (args.size() >= 3) {
        key = static_cast<uint32_t>(std::stoul(args[2], nullptr, 16));
    }

    std::cout << "[ddsdecrypt] Reading: " << args[0] << "\n";

    // 使用 DecryptOrCopyDDS 自动检测已解密文件
    auto result = AssetTool::DecryptOrCopyDDS(args[0], args[1], key);
    if (!result.success) {
        std::cerr << "[ddsdecrypt] Error: " << result.error << "\n";
        return 1;
    }

    std::cout << "[ddsdecrypt] Written: " << args[1] << "\n";
    return 0;
}

// ==========================================================================
// 命令：batch — 批量处理目录
// ==========================================================================

static int CommandBatch(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool batch <input_dir> <output_dir>\n";
        return 1;
    }

    std::string inputDir = args[0];
    std::string outputDir = args[1];

    if (!fs::is_directory(inputDir)) {
        std::cerr << "[batch] Input is not a directory: " << inputDir << "\n";
        return 1;
    }

    fs::create_directories(outputDir);

    int totalFiles = 0, successCount = 0, errorCount = 0;

    for (const auto &entry : fs::recursive_directory_iterator(inputDir)) {
        if (!entry.is_regular_file()) continue;

        std::string path = entry.path().string();
        std::string ext = GetExtension(path);
        std::string stem = GetStem(path);
        std::string relPath = fs::relative(entry.path(), inputDir).string();

        std::cout << "[batch] (" << (totalFiles + 1) << ") " << relPath << "\n";

        if (ext == ".x") {
            // .x → .dxmesh + scene.json
            AssetTool::XFileParser parser;
            if (!parser.ParseFile(path)) {
                std::cerr << "[batch]   Error parsing .x: " << parser.GetError() << "\n";
                errorCount++;
                totalFiles++;
                continue;
            }

            const auto &meshes = parser.GetMeshes();
            for (size_t mi = 0; mi < meshes.size(); ++mi) {
                std::string meshName = stem;
                if (meshes.size() > 1) meshName += "_" + std::to_string(mi);

                std::string dxmeshPath = outputDir + "/" + meshName + ".dxmesh";
                if (meshes[mi].WriteDxMesh(dxmeshPath)) {
                    std::cout << "[batch]   → " << meshName << ".dxmesh\n";
                }
            }

            // 生成 scene.json
            // (简化处理：只输出第一个 mesh 的信息)
            // 完整的 scene.json 生成在独立命令中处理
            successCount++;

        } else if (ext == ".hod") {
            // .hod → JSON + txt
            AssetTool::HODParser parser;
            if (!parser.ParseFile(path)) {
                std::cerr << "[batch]   Error parsing .hod: " << parser.GetError() << "\n";
                errorCount++;
                totalFiles++;
                continue;
            }

            const auto &hod = parser.GetResult();
            hod.WriteJSON(outputDir + "/" + stem + ".hod.json");
            hod.WriteText(outputDir + "/" + stem + ".hod.txt");
            std::cout << "[batch]   → " << stem << ".hod.json, .hod.txt (" << hod.BoneCount() << " bones)\n";
            successCount++;

        } else if (ext == ".mpd") {
            // .mpd → 解析输出 txt
            AssetTool::MPDParser parser;
            if (!parser.ParseFile(path)) {
                std::cerr << "[batch]   Error parsing .mpd: " << parser.GetError() << "\n";
                errorCount++;
                totalFiles++;
                continue;
            }

            const auto &mpd = parser.GetResult();
            mpd.WriteText(outputDir + "/" + stem + ".mpd.txt");
            std::cout << "[batch]   → " << stem << ".mpd.txt (" << mpd.TileCount() << " tiles)\n";
            successCount++;

        } else if (ext == ".ani" || ext == ".sdt") {
            // 解密并输出
            std::vector<uint8_t> data;
            if (!ReadFile(path, data)) {
                std::cerr << "[batch]   Cannot read\n";
                errorCount++;
                totalFiles++;
                continue;
            }

            AssetTool::XORCipher cipher(0x0B7E7759);
            cipher.DecryptBuffer(data.data(), data.size());

            std::string outPath = outputDir + "/" + stem + ext + ".decrypted";
            WriteFile(outPath, data.data(), data.size());
            std::cout << "[batch]   → " << stem << ext << ".decrypted\n";
            successCount++;

        } else if (ext == ".dds") {
            // XOR 解密 DDS（而非直接拷贝）
            auto r = AssetTool::DecryptOrCopyDDS(path, outputDir + "/" + relPath);
            if (r.success) {
                std::cout << "[batch]   → " << relPath << " (decrypted)\n";
                successCount++;
            } else {
                std::cerr << "[batch]   DDS decrypt failed: " << r.error << "\n";
                errorCount++;
            }

        } else if (ext == ".png" || ext == ".bmp") {
            // PNG/BMP → DDS 转换
            std::string ddsRel = fs::path(relPath).stem().string() + ".dds";
            auto r = AssetTool::ConvertPNGToDDS(path, outputDir + "/" + ddsRel);
            if (r.success) {
                std::cout << "[batch]   → " << ddsRel << "\n";
                successCount++;
            } else {
                std::cerr << "[batch]   Convert failed: " << r.error << "\n";
                errorCount++;
            }
        }

        totalFiles++;
    }

    std::cout << "\n[batch] Done: " << totalFiles << " files, "
              << successCount << " success, " << errorCount << " errors\n";
    return errorCount > 0 ? 1 : 0;
}

// ==========================================================================
// 地图扫描结果
// ==========================================================================
struct MapScanResult {
    std::string inputDir;
    std::string mapName;
    std::vector<std::string> xFiles;
    std::string sptPath;
    std::string mpdPath;
};

/// 扫描地图目录，校验 SPT 和 MPD 必须同时存在
/// 成功返回 true 并填充 result；失败返回 false 并输出错误信息
static bool ScanMapDirectory(const std::string &inputDir, MapScanResult &result) {
    result = MapScanResult{};
    result.inputDir = inputDir;
    result.mapName = fs::path(inputDir).filename().string();

    if (!fs::is_directory(inputDir)) {
        std::cerr << "[scan] Input is not a directory: " << inputDir << "\n";
        return false;
    }

    for (const auto &entry : fs::recursive_directory_iterator(inputDir)) {
        if (!entry.is_regular_file()) continue;
        std::string path = entry.path().string();
        std::string filename = entry.path().filename().string();
        std::string ext = GetExtension(path);

        if (ext == ".x") {
            result.xFiles.push_back(path);
        } else if (filename == "Script.spt") {
            result.sptPath = path;
        } else if (ext == ".mpd") {
            result.mpdPath = path;
        }
    }

    std::cout << "[scan] " << result.mapName << ": " << result.xFiles.size() << " .x files";
    if (!result.sptPath.empty()) std::cout << ", SPT found";
    if (!result.mpdPath.empty()) std::cout << ", MPD found";
    std::cout << "\n";

    // 必须有 SPT，否则场景数据不完整；MPD 已弃用，不做要求
    if (result.sptPath.empty()) {
        std::cerr << "[scan] ERROR: Script.spt is required.\n";
        return false;
    }
    if (result.mpdPath.empty()) {
        std::cout << "[scan]   MPD: not found (MPD parsing deprecated, will sample from SPT)\n";
    }

    return true;
}

// ==========================================================================
// 地图场景管线：基于扫描结果生成完整 scene.json
// 输入：扫描结果（至少含 SPT 路径），输出目录
// 输出：{output}/{mapName}.scene.json + Meshes/ + Materials/ + Textures/
// ==========================================================================

static int BuildMapScene(const MapScanResult &scan, const std::string &outDir) {
    // scan 已由外部扫描校验通过，不再内部调 ScanMapDirectory
    std::string inputDir = scan.inputDir;
    std::string mapName = scan.mapName;
    std::string sptPath = scan.sptPath;
    std::string mpdPath = scan.mpdPath;
    const auto &xFiles = scan.xFiles;

    fs::create_directories(outDir);
    fs::path meshesDir = fs::path(outDir) / "Meshes";
    fs::path matsDir   = fs::path(outDir) / "Materials";
    fs::path texDir    = fs::path(outDir) / "Textures";
    fs::create_directories(meshesDir);
    fs::create_directories(matsDir);
    fs::create_directories(texDir);

    // 1. 解析 SPT（天空、光照、水面、建筑）
    AssetTool::SceneData sceneData;
    {
        AssetTool::ScriptSPTParser sptParser;
        if (!sptParser.ParseFile(sptPath)) {
            std::cerr << "[map] ERROR: SPT parse failed: " << sptParser.GetError() << "\n";
            return 1;
        }
        sceneData = sptParser.GetResult();
        std::cout << "[map]   SPT: " << sceneData.buildings.size() << " buildings, "
                  << sceneData.mapTiles.size() << " tile mappings\n";
    }

    // 3. 解析 MPD（瓦片名称、坐标）— MPD 已弃用，仅做参考，不阻塞流程
    AssetTool::MPDData mpdData;
    if (!mpdPath.empty()) {
        AssetTool::MPDParser mpdParser;
        if (mpdParser.ParseFile(mpdPath)) {
            mpdData = mpdParser.GetResult();
            std::cout << "[map]   MPD: " << mpdData.TileCount() << " tiles, "
                      << mpdData.tileNames.size() << " tile names (reference only)\n";
        } else {
            std::cout << "[map]   MPD: parse failed (" << mpdParser.GetError() << ") — will sample from SPT\n";
        }
    } else {
        std::cout << "[map]   MPD: not found — will sample from SPT\n";
    }

    // ---- 4. 转换所有 .x → .dxmesh + 提取材质 ----
    struct MeshRef { std::string stem; std::string matKey; };
    std::map<std::string, std::vector<MeshRef>> xMeshMap; // 原始 .x 路径 → meshes
    nlohmann::json allMaterials = nlohmann::json::object();
    nlohmann::json allMeshDeps = nlohmann::json::object();
    nlohmann::json texDeps = nlohmann::json::object();
    int matCount = 0, xSuccess = 0, xErrors = 0;

    // 纹理处理辅助：在同一目录下查找纹理并转换
    auto processTexture = [&](const std::string &texFile, const fs::path &xDir) -> std::string {
        if (texFile.empty()) return {};
        fs::path texPath = xDir / texFile;
        if (!fs::exists(texPath)) {
            // 尝试仅文件名（可能 xDir 搜索不到）
            texPath = fs::path(inputDir) / fs::path(texFile).filename();
            if (!fs::exists(texPath)) return {};
        }
        std::string texExt = texPath.extension().string();
        std::transform(texExt.begin(), texExt.end(), texExt.begin(), ::tolower);
        std::string stem = texPath.stem().string();
        std::string ddsName = stem + ".dds";
        fs::path ddsOut = texDir / ddsName;

        if (fs::exists(ddsOut)) {
            // 已转换过，直接返回 key
            return stem;
        }

        if (texExt == ".dds") {
            auto r = AssetTool::DecryptOrCopyDDS(texPath.string(), ddsOut.string());
            if (r.success) {
                std::cout << "[map]     texture: " << ddsName << " (decrypted)\n";
                return stem;
            }
        } else if (texExt == ".png" || texExt == ".bmp") {
            auto r = AssetTool::ConvertPNGToDDS(texPath.string(), ddsOut.string());
            if (r.success) {
                std::cout << "[map]     texture: " << ddsName << "\n";
                return stem;
            }
        }
        return {};
    };

    for (const auto &xPath : xFiles) {
        fs::path xDir = fs::path(xPath).parent_path();
        std::string xname = fs::path(xPath).filename().string();

        AssetTool::XFileParser parser;
        if (!parser.ParseFile(xPath)) {
            std::cerr << "[map]   ERROR: " << xname << " — " << parser.GetError() << "\n";
            xErrors++;
            continue;
        }

        const auto &meshes = parser.GetMeshes();
        std::string stem = fs::path(xname).stem().string();
        std::vector<MeshRef> refs;

        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            const auto &mesh = meshes[mi];
            std::string ms = stem;
            if (meshes.size() > 1) ms += "_" + std::to_string(mi);

            // 写出 .dxmesh
            mesh.WriteDxMesh((meshesDir / (ms + ".dxmesh")).string());

            // 材质
            auto matDesc = mesh.material.ToMaterialDesc();
            std::string mk = ms + "_mat0";

            nlohmann::json jm;
            jm["shader"] = matDesc.shader;
            jm["params"]["baseColor"] = {matDesc.params.baseColor[0], matDesc.params.baseColor[1],
                                         matDesc.params.baseColor[2], matDesc.params.baseColor[3]};
            jm["params"]["metallic"] = matDesc.params.metallic;
            jm["params"]["roughness"] = matDesc.params.roughness;
            jm["params"]["ao"] = matDesc.params.ao;

            // 纹理
            std::string texRef = processTexture(mesh.material.textureFilename, xDir);
            if (!texRef.empty()) {
                jm["textures"]["baseColor"] = texRef;
                texDeps[texRef] = "Textures/" + texRef + ".dds";
            }

            // 写独立 .mat 文件
            std::ofstream matFile((matsDir / (mk + ".mat")).string());
            if (matFile) matFile << jm.dump(2);

            // 收集
            allMaterials[mk] = jm;
            allMeshDeps[ms] = "Meshes/" + ms + ".dxmesh";
            refs.push_back({ms, mk});
            matCount++;
        }

        xMeshMap[xname] = refs;
        xSuccess++;
    }

    std::cout << "[map]   Converted: " << xSuccess << " .x files, "
              << matCount << " materials, " << texDeps.size() << " textures\n";

    // ---- 5. 构建 scene.json ----
    nlohmann::json scene;
    scene["$schema"] = "../../Schemas/scene.schema.json";
    scene["version"] = 1;
    scene["metadata"]["name"] = mapName;
    scene["metadata"]["description"] = "Converted from " + mapName;
    scene["baseURL"] = "Content/" + mapName;

    // 环境光照
    scene["environment"]["ambientLight"] = {
        sceneData.lightR / 255.0f * 0.4f,
        sceneData.lightG / 255.0f * 0.4f,
        sceneData.lightB / 255.0f * 0.4f,
        1.0f
    };

    // 依赖
    if (!allMeshDeps.empty()) scene["dependencies"]["meshes"] = allMeshDeps;
    if (!texDeps.empty()) scene["dependencies"]["textures"] = texDeps;

    // 材质
    scene["materials"] = allMaterials;

    // 实体
    auto &entities = scene["entities"] = nlohmann::json::array();
    int entityIdx = 0;

    auto addEntity = [&](const std::string &name,
                         float px, float py, float pz,
                         const std::string &meshKey, const std::string &matKey,
                         bool isTransparent, bool isSkybox) {
        nlohmann::json e;
        e["name"] = name;
        auto &c = e["components"];
        c["transform"]["position"] = {px, py, pz};
        c["transform"]["rotation"] = {0, 0, 0, 1};
        c["transform"]["scale"] = {1, 1, 1};
        c["mesh"]["geometry"] = meshKey;
        c["mesh"]["material"] = matKey;
        if (isSkybox) {
            c["skybox"] = nullptr;
            c["transparent"] = nullptr;
        } else if (isTransparent) {
            c["transparent"] = nullptr;
        } else {
            c["opaque"] = nullptr;
        }
        entities.push_back(e);
        entityIdx++;
    };

    // 辅助：从 .x 文件名找 xMeshMap 中的第一个 mesh ref
    auto findFirstRef = [&](const std::string &xfilename) -> MeshRef {
        auto it = xMeshMap.find(xfilename);
        if (it != xMeshMap.end() && !it->second.empty())
            return it->second[0];
        // 尝试匹配 stem（不带扩展名）
        std::string stem = fs::path(xfilename).stem().string();
        for (const auto &[xf, refs] : xMeshMap) {
            if (fs::path(xf).stem().string() == stem && !refs.empty())
                return refs[0];
        }
        // 前缀匹配：MPD tile stem 是 mesh key 前缀（如 "map00" 匹配 "map00_0"）
        for (const auto &[xf, refs] : xMeshMap) {
            std::string xstem = fs::path(xf).stem().string();
            if (xstem.size() > stem.size() && xstem.substr(0, stem.size()) == stem && xstem[stem.size()] == '_' && !refs.empty())
                return refs[0];
        }
        return {};
    };

    // A) MPD 瓦片实体（直接使用 MPD 坐标数据）
    int mpdEntityCount = 0;
    if (!mpdData.tiles.empty()) {
        for (size_t i = 0; i < mpdData.tiles.size(); ++i) {
            const auto &t = mpdData.tiles[i];
            if (t.name.empty()) continue; // 跳过无名字的条目
            std::string xname = t.name;
            if (!xname.ends_with(".x")) xname += ".x";
            // 如果 name 是纯数字（索引编号），尝试用 name table 还原文件名
            auto ref = findFirstRef(xname);
            if (ref.stem.empty() && t.tileIndex < mpdData.tileNames.size()) {
                std::string altName = mpdData.tileNames[t.tileIndex];
                ref = findFirstRef(altName);
            }
            if (ref.stem.empty()) continue;
            addEntity(t.name, t.posX, 0, t.posZ, ref.stem, ref.matKey, false, false);
            mpdEntityCount++;
        }
        if (mpdEntityCount > 0)
            std::cout << "[map]   Entities (MPD tiles): " << mpdEntityCount << "\n";
    }

    // B) SPT 瓦片采样模拟 — 仅当 MPD 无数据时作为降级方案
    if (!sceneData.mapTiles.empty() && mpdEntityCount == 0) {
        // 从建筑坐标估算地图范围
        float mapMinX = -500.0f, mapMaxX = 500.0f;
        float mapMinZ = -500.0f, mapMaxZ = 500.0f;
        if (!sceneData.buildings.empty()) {
            mapMinX = mapMaxX = sceneData.buildings[0].posX;
            mapMinZ = mapMaxZ = sceneData.buildings[0].posZ;
            for (const auto &b : sceneData.buildings) {
                if (b.posX < mapMinX) mapMinX = b.posX;
                if (b.posX > mapMaxX) mapMaxX = b.posX;
                if (b.posZ < mapMinZ) mapMinZ = b.posZ;
                if (b.posZ > mapMaxZ) mapMaxZ = b.posZ;
            }
            // 向外扩展 20% 作为瓦片区缓冲
            float ex = (mapMaxX - mapMinX) * 0.2f;
            float ez = (mapMaxZ - mapMinZ) * 0.2f;
            if (ex < 50.0f) ex = 50.0f;
            if (ez < 50.0f) ez = 50.0f;
            mapMinX -= ex; mapMaxX += ex;
            mapMinZ -= ez; mapMaxZ += ez;
        }

        float tileSize = 20.0f;
        int gridCols = std::max(1, static_cast<int>((mapMaxX - mapMinX) / tileSize));
        int gridRows = std::max(1, static_cast<int>((mapMaxZ - mapMinZ) / tileSize));
        // 限制总瓦片数避免场景过大
        const int maxTiles = 200;
        if (gridCols * gridRows > maxTiles) {
            float ratio = (float)gridCols / gridRows;
            gridCols = static_cast<int>(std::sqrt((float)maxTiles * ratio));
            gridRows = static_cast<int>((float)maxTiles / gridCols);
            if (gridCols < 1) gridCols = 1;
            if (gridRows < 1) gridRows = 1;
        }

        size_t tileTypeCount = sceneData.mapTiles.size();
        int added = 0;
        for (int r = 0; r < gridRows; ++r) {
            for (int c = 0; c < gridCols; ++c) {
                // 按位置循环选取 tile 类型
                size_t ti = (r * gridCols + c) % tileTypeCount;
                const auto &t = sceneData.mapTiles[ti];
                std::string xname = t.modelFile;
                auto ref = findFirstRef(xname);
                if (ref.stem.empty()) continue;

                // 网格基准位置
                float baseX = mapMinX + (c + 0.5f) * tileSize;
                float baseZ = mapMinZ + (r + 0.5f) * tileSize;

                // 确定性抖动（基于网格坐标），让布局不那么规整
                float jitterX = (DeterministicNoise(c, r) - 0.5f) * tileSize * 0.4f;
                float jitterZ = (DeterministicNoise(r, c + 137) - 0.5f) * tileSize * 0.4f;

                float px = baseX + jitterX;
                float pz = baseZ + jitterZ;

                addEntity(fs::path(xname).stem().string() + "_" + std::to_string(added),
                          px, 0, pz, ref.stem, ref.matKey, false, false);
                added++;
            }
        }
        std::cout << "[map]   Entities (SPT sampled tiles): " << added
                  << "  map area: (" << mapMinX << "," << mapMinZ << ")~("
                  << mapMaxX << "," << mapMaxZ << ")\n";
    }

    // B) 建筑实体（SPT SetBuilding 位置）
    if (!sceneData.buildings.empty()) {
        int bCount = 0;
        for (const auto &b : sceneData.buildings) {
            std::string mf;
            if (b.buildNo >= 0 && (size_t)b.buildNo < sceneData.buildingFiles.size())
                mf = sceneData.buildingFiles[b.buildNo];
            if (mf.empty()) continue;

            auto ref = findFirstRef(mf);
            if (ref.stem.empty()) {
                if (bCount < 5) std::cout << "[map]   WARN: building '" << mf << "' no match in meshes\n";
                continue;
            }

            addEntity(fs::path(mf).stem().string() + "_" + std::to_string(entityIdx),
                      b.posX, b.posY, b.posZ,
                      ref.stem, ref.matKey, false, false);
            bCount++;
        }
        std::cout << "[map]   Entities (buildings): " << bCount << "\n";
    }

    // C) 水面实体
    if (!sceneData.waterModel.empty()) {
        auto ref = findFirstRef(sceneData.waterModel);
        if (!ref.stem.empty()) {
            addEntity("water", 0, 0, 0, ref.stem, ref.matKey, true, false);
            std::cout << "[map]   Entity: water\n";
        }
    }

    // D) 天空盒（顶层属性，非实体）
    if (!sceneData.skyModel.empty()) {
        auto ref = findFirstRef(sceneData.skyModel);
        if (!ref.stem.empty()) {
            nlohmann::json sb;
            sb["geometry"] = ref.stem;
            sb["material"] = ref.matKey;
            scene["skybox"] = sb;
            std::cout << "[map]   Skybox: " << ref.stem << "\n";
        }
    }

    // E) Hit 碰撞体实体（从 SPT LoadHitXFile 引用）
    if (!sceneData.hitModel.empty()) {
        std::string hitStem = fs::path(sceneData.hitModel).stem().string();
        // 查找所有匹配的 hit mesh（如 Hit_0, Hit_1, map00hit, map01Hit 等）
        for (const auto &[meshKey, meshPath] : allMeshDeps.items()) {
            std::string ml = meshKey;
            std::transform(ml.begin(), ml.end(), ml.begin(), ::tolower);
            std::string hsl = hitStem;
            std::transform(hsl.begin(), hsl.end(), hsl.begin(), ::tolower);
            if (ml.find(hsl) != std::string::npos || ml.find("hit") != std::string::npos) {
                // 找到 hit mesh，查对应的材质
                std::string matKey = meshKey + "_mat0";
                if (allMaterials.contains(matKey)) {
                    addEntity(meshKey, 0, 0, 0, meshKey, matKey, true, false);
                    std::cout << "[map]   Hit entity: " << meshKey << "\n";
                }
            }
        }
    }

    // ---- 6. 写 scene.json ----
    std::string scenePath = (fs::path(outDir) / (mapName + ".scene.json")).string();
    std::ofstream sceneFile(scenePath);
    if (!sceneFile) {
        std::cerr << "[map] Failed to write scene.json\n";
        return 1;
    }
    sceneFile << scene.dump(2);

    // 输出 MPD 解析结果用于比对
    std::string mpdTxtPath = (fs::path(outDir) / (mapName + ".mpd.txt")).string();
    mpdData.WriteText(mpdTxtPath);
    std::cout << "[map]   MPD debug: " << mpdTxtPath << " (" << mpdData.TileCount() << " tiles)\n";

    std::cout << "\n[map] Done: " << xSuccess << " .x files, "
              << matCount << " materials, " << entityIdx << " entities";
    if (xErrors > 0)
        std::cout << ", " << xErrors << " ERRORS";
    std::cout << "\n";
    std::cout << "[map] Output: " << scenePath << "\n";
    return xErrors > 0 ? 1 : 0;
}

// ==========================================================================
// 命令：x2mapanalysis — 解析合并 map.x + 各 tile .x，提取正确 tile 位置
// ==========================================================================

static int CommandMapAnalysis(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool x2mapanalysis <combined.map.x> <tiles_dir> [output.txt]\n";
        return 1;
    }
    std::string combinedPath = args[0];
    std::string tilesDir = args[1];
    std::string outputPath = args.size() > 2 ? args[2] : "";

    // 1. 解析合并 map.x
    AssetTool::XFileParser combinedParser;
    if (!combinedParser.ParseFile(combinedPath)) {
        std::cerr << "[x2mapanalysis] Error: " << combinedParser.GetError() << "\n";
        return 1;
    }
    auto cMeshes = combinedParser.GetMeshes();
    if (cMeshes.empty()) { std::cerr << "[x2mapanalysis] No meshes\n"; return 1; }
    size_t mainIdx = 0;
    for (size_t i = 0; i < cMeshes.size(); ++i)
        if (cMeshes[i].VertexCount() > cMeshes[mainIdx].VertexCount()) mainIdx = i;
    auto &cm = cMeshes[mainIdx];
    cm.ComputeBounds();
    std::cout << "[x2mapanalysis] Combined: " << cm.VertexCount() << " verts, bounds: "
              << cm.boundsMin[0] << "," << cm.boundsMin[2] << " ~ "
              << cm.boundsMax[0] << "," << cm.boundsMax[2] << "\n";

    // 2. 扫描 tiles 目录中的 .x 文件
    std::vector<std::string> tileFiles;
    for (const auto &entry : fs::recursive_directory_iterator(tilesDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".x") tileFiles.push_back(entry.path().string());
    }
    std::cout << "[x2mapanalysis] Found " << tileFiles.size() << " .x files\n";

    // 3. 解析每个 tile 获取局部包围盒和顶点数
    struct TileInfo {
        std::string name; float bmin[3], bmax[3], dims[3]; size_t verts;
    };
    std::vector<TileInfo> infos;
    for (const auto &tf : tileFiles) {
        AssetTool::XFileParser p;
        if (!p.ParseFile(tf)) continue;
        auto ms = p.GetMeshes();
        if (ms.empty()) continue;
        size_t bi = 0;
        for (size_t i = 0; i < ms.size(); ++i)
            if (ms[i].VertexCount() > ms[bi].VertexCount()) bi = i;
        ms[bi].ComputeBounds();
        TileInfo ti;
        ti.name = fs::path(tf).stem().string();
        ti.verts = ms[bi].VertexCount();
        for (int d = 0; d < 3; ++d) {
            ti.bmin[d] = ms[bi].boundsMin[d]; ti.bmax[d] = ms[bi].boundsMax[d];
            ti.dims[d] = ti.bmax[d] - ti.bmin[d];
        }
        infos.push_back(ti);
    }
    std::cout << "[x2mapanalysis] Parsed " << infos.size() << " tiles\n";

    // 4. 按 grid 分组合并网格顶点
    const float gridSize = 30.0f;
    float originX = std::floor(cm.boundsMin[0] / gridSize) * gridSize;
    float originZ = std::floor(cm.boundsMin[2] / gridSize) * gridSize;
    struct CellInfo {
        int gx, gz; float cx, cz;
        float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;
        int count = 0;
    };
    std::map<std::pair<int,int>, CellInfo> cells;
    for (size_t vi = 0; vi + 2 < cm.positions.size(); vi += 3) {
        float px = cm.positions[vi], pz = cm.positions[vi + 2];
        int gx = static_cast<int>(std::floor((px - originX) / gridSize));
        int gz = static_cast<int>(std::floor((pz - originZ) / gridSize));
        auto &c = cells[std::make_pair(gx, gz)];
        c.gx = gx; c.gz = gz;
        c.cx = originX + (gx + 0.5f) * gridSize;
        c.cz = originZ + (gz + 0.5f) * gridSize;
        if (px < c.minX) c.minX = px; if (px > c.maxX) c.maxX = px;
        if (pz < c.minZ) c.minZ = pz; if (pz > c.maxZ) c.maxZ = pz;
        c.count++;
    }
    std::cout << "[x2mapanalysis] Found " << cells.size() << " occupied grid cells\n";

    // 5. 匹配每个 grid cell 到 tile 模型
    struct Result {
        std::string tileName; float worldX, worldZ;
        float cellMinX, cellMaxX, cellMinZ, cellMaxZ;
        int vertCount; float matchScore;
    };
    std::vector<Result> results;
    for (auto &[key, cell] : cells) {
        float cw = cell.maxX - cell.minX, cd = cell.maxZ - cell.minZ;
        float bestScore = 1e9f; std::string bestName;
        for (const auto &ti : infos) {
            if (ti.verts == 0) continue;
            float vDiff = std::abs(static_cast<float>(ti.verts) - cell.count) / std::max(ti.verts, (size_t)1);
            float wDiff = std::abs(ti.dims[0] - cw) / std::max(ti.dims[0], 0.001f);
            float dDiff = std::abs(ti.dims[2] - cd) / std::max(ti.dims[2], 0.001f);
            float score = vDiff * 0.5f + wDiff * 0.25f + dDiff * 0.25f;
            if (score < bestScore) { bestScore = score; bestName = ti.name; }
        }
        if (bestScore < 0.5f)
            results.push_back({bestName, cell.cx, cell.cz, cell.minX, cell.maxX, cell.minZ, cell.maxZ, cell.count, bestScore});
    }
    std::cout << "[x2mapanalysis] Matched " << results.size() << " tiles (score<0.5)\n";

    // 6. 输出
    std::ostream *out = &std::cout;
    std::ofstream outFile;
    if (!outputPath.empty()) { outFile.open(outputPath); if (outFile) out = &outFile; }
    *out << "TileName\tWorldX\tWorldZ\tCellMinX\tCellMaxX\tCellMinZ\tCellMaxZ\tVerts\tScore\n";
    for (const auto &r : results)
        *out << r.tileName << "\t" << r.worldX << "\t" << r.worldZ << "\t"
             << r.cellMinX << "\t" << r.cellMaxX << "\t" << r.cellMinZ << "\t" << r.cellMaxZ << "\t"
             << r.vertCount << "\t" << r.matchScore << "\n";
    if (!outputPath.empty()) std::cout << "[x2mapanalysis] Written: " << outputPath << "\n";
    return 0;
}

// ==========================================================================
// CLI map 命令封装：扫描 → 校验 → 构建（批量场景一步到位）
// ==========================================================================

static int CommandMap(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool map <input_map_dir> <output_dir>\n";
        return 1;
    }
    MapScanResult scan;
    if (!ScanMapDirectory(args[0], scan)) return 1;
    return BuildMapScene(scan, args[1]);
}

// ==========================================================================
// 命令：importrobot — 导入机体（合并 .x 部件 + 骨架）
//
// 使用 RobotMerger 核心逻辑（含 Body_d 修正、Ry(180°)、LR 交换）
// ==========================================================================

// ==========================================================================
// 命令：ani2output — 解析 Script.ani 动画源文件，按组输出 HOD 帧 + Tail
// ==========================================================================

static int CommandANI2Output(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool ani2output <Script.ani> <output_dir>\n";
        return 1;
    }

    std::string aniPath = args[0];
    std::string outDir = args[1];

    std::cout << "[ani2output] " << aniPath << " → " << outDir << "\n";

    AssetTool::ANIParser parser;
    if (!parser.ParseFile(aniPath)) {
        std::cerr << "[ani2output] FAILED: " << parser.GetError() << "\n";
        return 1;
    }

    const auto &groups = parser.GetGroups();
    std::cout << "[ani2output] OK — " << groups.size() << " groups\n";
    size_t totalFrames = 0;
    for (const auto &g : groups) {
        totalFrames += g.frames.size();
        std::cout << "  组 " << g.index << ": " << g.frames.size() << " 帧"
                  << (g.tail.script.empty() ? "" : " + Tail")
                  << (g.tail.scriptText.empty() ? "" : " (脚本)") << "\n";
    }
    std::cout << "[ani2output] 总帧数: " << totalFrames << "\n";

    if (!parser.WriteOutput(outDir)) {
        std::cerr << "[ani2output] FAILED: 输出目录写入失败\n";
        return 1;
    }
    std::cout << "[ani2output] DONE → " << outDir << "\n";
    return 0;
}

// ==========================================================================
// 命令：ani2anim — 解析 Script.ani 动画源文件，各组帧数据 → 动画 FBX
//   每个动画组（Tail 标记划分）→ aiAnimation，含全部骨骼 aiNodeAnim 通道
// ==========================================================================

static int CommandANI2Anim(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool ani2anim <Script.ani> <output_dir> [stem]\n";
        return 1;
    }

    std::string aniPath = args[0];
    std::string outDir = args[1];
    std::string stem = (args.size() >= 3) ? args[2] : "Robo";

    std::cout << "[ani2anim] " << aniPath << " → " << outDir << " (stem=" << stem << ")\n";

    auto result = AssetTool::RobotMerger::ExportAnimationsFBX(aniPath, outDir, stem);
    if (!result.success) {
        std::cerr << "[ani2anim] FAILED: " << result.error << "\n";
        return 1;
    }
    std::cout << "[ani2anim] OK — " << result.partCount << " 骨骼通道 → " << result.outputFiles.front() << "\n";
    return 0;
}

// ==========================================================================
// 命令：ani2frames — 每帧导出嵌套 x（调试/观察：从 x 逐个看 ANI 动作姿势）
// ==========================================================================

static int CommandANI2Frames(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool ani2frames <Script.ani> <output_dir> [stem]\n";
        return 1;
    }

    std::string aniPath = args[0];
    std::string outDir = args[1];
    std::string stem = (args.size() >= 3) ? args[2] : "Robo";

    std::cout << "[ani2frames] " << aniPath << " → " << outDir << " (stem=" << stem << ")\n";

    auto result = AssetTool::RobotMerger::ExportAnimationFramesX(aniPath, outDir, stem);
    if (!result.success) {
        std::cerr << "[ani2frames] FAILED: " << result.error << "\n";
        return 1;
    }
    std::cout << "[ani2frames] OK — " << result.partCount << " 帧 → " << outDir << "\n";
    return 0;
}

// ==========================================================================
// CommandANI2IK — B 方案：FABRIK 离线验证（脚贴地 / 手瞄准）
// 从 ANI 首帧 HOD（= 母版骨架，含部件名/A/B/矩阵）识别 arm/leg 链，
// 用母版驱动链定义（兼容 KD-06 大写 Arm1/Arm2/Arm3 变体），对每条链
// 施加演示目标（leg → 末端 Y 压地；arm → 末端前移），FABRIK 求解后
// 输出报告：求解前后末端位置、误差、迭代数、每骨骼局部矩阵。
// 参考：Docs/targets/UKW_PowerUpKit/02_RobotAndAnimation.md §8.5
// ==========================================================================
static int CommandANI2IK(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool ani2ik <Script.ani> <output_dir> [stem]\n";
        return 1;
    }

    std::string aniPath = args[0];
    std::string outDir = args[1];
    std::string stem = (args.size() >= 3) ? args[2] : "Robo";

    std::cout << "[ani2ik] " << aniPath << " → " << outDir << " (stem=" << stem << ")\n";

    // 1. 解析 ANI，取首帧 HOD 数据（母版骨架：部件名 + A/B + 矩阵）
    AssetTool::ANIParser parser;
    if (!parser.ParseFile(aniPath)) {
        std::cerr << "[ani2ik] FAILED: " << parser.GetError() << "\n";
        return 1;
    }
    const auto &groups = parser.GetGroups();
    if (groups.empty() || groups[0].frames.empty()) {
        std::cerr << "[ani2ik] FAILED: 无动画帧数据\n";
        return 1;
    }
    const auto &frameData = groups[0].frames[0].data;

    // 2. HOD 解析 → 骨架树
    AssetTool::HODParser hodParser;
    if (!hodParser.Parse(frameData.data(), frameData.size())) {
        std::cerr << "[ani2ik] FAILED: HOD 解析失败: " << hodParser.GetError() << "\n";
        return 1;
    }
    const auto &hod = hodParser.GetResult();
    std::cout << "[ani2ik] 骨架 " << hod.BoneCount() << " 骨骼\n";

    // 3. 母版驱动链识别（arm/leg，兼容大小写）
    auto chains = AssetTool::IKSolver::FindChains(hod);
    if (chains.empty()) {
        std::cerr << "[ani2ik] FAILED: 未识别到 arm/leg 链\n";
        return 1;
    }
    std::cout << "[ani2ik] 识别 " << chains.size() << " 条链\n";

    // 4. 逐链求解 + 报告
    std::string reportPath = (fs::path(outDir) / (stem + "_ik_report.txt")).string();
    std::ofstream ofs(reportPath);
    if (!ofs) {
        std::cerr << "[ani2ik] FAILED: 无法写入报告 " << reportPath << "\n";
        return 1;
    }
    ofs << "# " << stem << " IK 离线验证报告（FABRIK 多轴无约束）\n";
    ofs << "# 源: " << aniPath << "\n";
    ofs << "# 初始姿势: " << groups[0].frames[0].name << "\n\n";

    int chainIdx = 0;
    int okCount = 0;
    for (auto &chain : chains) {
        ++chainIdx;
        const auto &last = chain.joints.back();

        // 演示目标：leg → 末端 Y 压到 0（脚贴地）；arm → 末端沿 +X 前移 0.2
        std::string lower;
        for (char c : chain.name)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        float target[3] = {last.position[0], last.position[1], last.position[2]};
        if (lower.find("leg") != std::string::npos)
            target[1] = 0.0f;      // 脚贴地
        else
            target[0] += 0.2f;     // 手瞄准（前移）

        // FABRIK 求解
        auto res = AssetTool::IKSolver::SolveFABRIK(chain.joints, target);
        AssetTool::IKSolver::ApplyPositionsToLocal(chain, hod);

        ofs << "== 链 " << chainIdx << ": " << chain.name
            << " (" << chain.joints.size() << " 关节) ==";
        ofs << (res.success ? " [收敛]" : " [未收敛]") << "\n";
        ofs << "  关节: ";
        for (const auto &j : chain.joints)
            ofs << j.name << " ";
        ofs << "\n";
        ofs << "  目标: (" << target[0] << ", " << target[1] << ", " << target[2] << ")\n";
        for (size_t i = 0; i < chain.joints.size(); ++i) {
            const auto &j = chain.joints[i];
            ofs << "    位置[" << i << "] " << j.name << " = ("
                << j.position[0] << ", " << j.position[1] << ", " << j.position[2] << ")\n";
        }
        ofs << "    误差: " << res.error << "  迭代: " << res.iterations << "\n";
        for (size_t i = 0; i < chain.solvedLocalMats.size(); ++i) {
            const auto &m = chain.solvedLocalMats[i];
            ofs << "    局部矩阵[" << i << "] " << chain.joints[i].name << ":";
            for (int k = 0; k < 16; ++k)
                ofs << " " << m.m[k];
            ofs << "\n";
        }
        ofs << "\n";

        std::cout << "  [链 " << chainIdx << "] " << chain.name
                  << (res.success ? " OK" : " 未收敛")
                  << " 误差=" << res.error << "\n";
        if (res.success)
            ++okCount;
    }

    ofs.close();
    std::cout << "[ani2ik] DONE — " << okCount << "/" << chains.size()
              << " 链收敛 → " << reportPath << "\n";
    return 0;
}

static int CommandImportRobot(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        std::cerr << "Usage: AssetTool importrobot <Robo.hod> <output_dir>\n";
        return 1;
    }

    std::string hodPath = args[0];
    std::string outDir = args[1];
    bool lrSwap = true;
    bool exportX = false;
    bool exportFBX = false;

    // 解析可选参数
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--no-lrswap") lrSwap = false;
        if (args[i] == "--export-x") exportX = true;
        if (args[i] == "--export-fbx") exportFBX = true;
    }

    std::cout << "[importrobot] " << hodPath << " → " << outDir << "\n";

    AssetTool::RobotMergeOptions opts;
    opts.lrSwap = lrSwap;
    opts.exportX = exportX;
    opts.exportFBX = exportFBX;

    auto result = AssetTool::RobotMerger::Merge(hodPath, outDir, opts);
    if (!result.success) {
        std::cerr << "[importrobot] FAILED: " << result.error << "\n";
        return 1;
    }

    std::cout << "[importrobot] DONE: " << result.partCount << " parts, "
              << result.vertexCount << " verts, " << result.indexCount << " indices\n";
    for (const auto &f : result.outputFiles)
        std::cout << "  → " << f << "\n";
    return 0;
}

// ==========================================================================
// 命令：verifyfbx — 验证 FBX 文件（用 assimp 重新导入并打印结构）
// ==========================================================================

static void DumpAINode(const aiNode *node, int depth) {
    for (int i = 0; i < depth; ++i) std::cout << "  ";
    std::cout << "Node: " << (node->mName.length ? node->mName.C_Str() : "(unnamed)");
    if (node->mNumMeshes > 0) std::cout << " [meshes: " << node->mNumMeshes << "]";
    std::cout << "\n";
    for (unsigned int i = 0; i < node->mNumChildren; ++i)
        DumpAINode(node->mChildren[i], depth + 1);
}

static int CommandVerifyFBX(const std::vector<std::string> &args) {
    if (args.empty()) {
        std::cerr << "Usage: AssetTool verifyfbx <file.fbx>\n";
        return 1;
    }
    std::string path = args[0];
    std::cout << "[verifyfbx] Opening: " << path << "\n";

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_GenNormals);
    if (!scene) {
        std::cerr << "[verifyfbx] FAILED: " << importer.GetErrorString() << "\n";
        return 1;
    }

    std::cout << "[verifyfbx] OK — " << scene->mNumMeshes << " meshes, "
              << scene->mNumMaterials << " materials, "
              << scene->mNumAnimations << " animations\n";

    unsigned int totalVerts = 0, totalFaces = 0;
    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh *m = scene->mMeshes[mi];
        std::cout << "  Mesh[" << mi << "] \"" << (m->mName.length ? m->mName.C_Str() : "?") << "\": "
                  << m->mNumVertices << " verts, " << m->mNumFaces << " faces"
                  << (m->HasNormals() ? ", normals" : "")
                  << (m->HasTextureCoords(0) ? ", UV" : "")
                  << (m->mNumBones > 0 ? (", " + std::to_string(m->mNumBones) + " bones") : "")
                  << "\n";
        totalVerts += m->mNumVertices;
        totalFaces += m->mNumFaces;
        for (unsigned int bi = 0; bi < m->mNumBones; ++bi) {
            const aiBone *b = m->mBones[bi];
            std::cout << "    Bone[" << bi << "] \"" << b->mName.C_Str() << "\": "
                      << b->mNumWeights << " weights\n";
        }
    }
    std::cout << "[verifyfbx] Total: " << totalVerts << " verts, " << totalFaces << " faces\n";
    std::cout << "[verifyfbx] Node hierarchy:\n";
    if (scene->mRootNode) DumpAINode(scene->mRootNode, 0);
    return 0;
}

// ==========================================================================
// 主入口
// ==========================================================================

static void PrintUsage(const char *progName) {
    std::cout << "AssetTool — UKW PowerUp Kit Asset Converter\n\n";
    std::cout << "Usage:\n";
    std::cout << "  " << progName << " x2mesh <input.x> <output.dxmesh>\n";
    std::cout << "      Convert .x mesh to .dxmesh format\n\n";
    std::cout << "  " << progName << " x2scene <input.x> <output.scene.json>\n";
    std::cout << "      Generate scene JSON with embedded materials\n\n";
    std::cout << "  " << progName << " hod2json <input.hod> <output.json>\n";
    std::cout << "      Parse .hod skeleton tree to JSON\n\n";
    std::cout << "  " << progName << " hod2txt <input.hod> <output.txt>\n";
    std::cout << "      Parse .hod skeleton tree to readable text\n\n";
    std::cout << "  " << progName << " mpd2txt <input.mpd> <output.txt> [asset_dir]\n";
    std::cout << "      Parse .mpd map tile data to readable text\n";
    std::cout << "      asset_dir: optional, filter names against actual .x files\n\n";
    std::cout << "  " << progName << " spt2json <Script.spt> <scene.json>\n";
    std::cout << "      Parse scene script to JSON format\n\n";
    std::cout << "  " << progName << " decrypt <input> <output> [key_hex]\n";
    std::cout << "      XOR decrypt a file (default key: 0x0B7E7759)\n\n";
    std::cout << "  " << progName << " png2dds <input.png> <output.dds>\n";
    std::cout << "      Convert PNG texture to DDS format\n\n";
    std::cout << "  " << progName << " ddsdecrypt <input.dds> <output.dds> [key_hex]\n";
    std::cout << "      XOR decrypt DDS texture (default key: 0x0B7E7759)\n\n";
    std::cout << "  " << progName << " batch <input_dir> <output_dir>\n";
    std::cout << "      Batch convert all assets in a directory\n\n";
    std::cout << "  " << progName << " importrobot <Robo.hod> <output_dir> [--no-lrswap] [--export-x] [--export-fbx]\n";
    std::cout << "      Import robot: merge .x parts + skeleton → .dxmesh + .bone + hod.json + scene.json\n";
    std::cout << "        --no-lrswap  disable LR bone swap\n";
    std::cout << "        --export-x   export .x with bone hierarchy (DE verification)\n";
    std::cout << "        --export-fbx export FBX with skeleton + skinning\n\n";
    std::cout << "  " << progName << " ani2output <Script.ani> <output_dir>\n";
    std::cout << "      Parse animation source: split by Tail marker into groups, each with HOD frames + Tail\n";
    std::cout << "  " << progName << " ani2anim <Script.ani> <output_dir> [stem]\n";
    std::cout << "      Export animation FBX: each group (Tail-marked) → aiAnimation with bone channels\n";
    std::cout << "  " << progName << " ani2frames <Script.ani> <output_dir> [stem]\n";
    std::cout << "      Export each frame as nested .x (debug): observe ANI poses per frame\n";
    std::cout << "  " << progName << " ani2ik <Script.ani> <output_dir> [stem]\n";
    std::cout << "      FABRIK offline validation (B plan): identify arm/leg chains from master skeleton,\n";
    std::cout << "      solve foot-ground / hand-aim demo targets, write {stem}_ik_report.txt\n\n";
    std::cout << "  " << progName << " map <input_map_dir> <output_dir>\n";
    std::cout << "      Map pipeline: scan all .x + parse MPD/SPT → complete scene.json\n\n";
    std::cout << "  " << progName << " x2mapanalysis <combined.map.x> <tiles_dir> [output.txt]\n";
    std::cout << "      Analyze combined map.x against individual tiles to extract tile positions\n\n";
    std::cout << "  " << progName << " verifyfbx <file.fbx>\n";
    std::cout << "      Verify FBX file: re-import with assimp and print scene structure\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << progName << " x2mesh Body.x Body.dxmesh\n";
    std::cout << "  " << progName << " hod2json Robo.hod Robo_skeleton.json\n";
    std::cout << "  " << progName << " batch D:/APP/UKW/Robo/ ./output/\n";
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 0;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i)
        args.push_back(argv[i]);

    if (command == "x2mesh") {
        return CommandX2Mesh(args);
    } else if (command == "x2scene") {
        return CommandX2Scene(args);
    } else if (command == "hod2json") {
        return CommandHOD2JSON(args);
    } else if (command == "hod2txt") {
        return CommandHOD2Txt(args);
    } else if (command == "mpd2txt") {
        return CommandMPD2Txt(args);
    } else if (command == "spt2json") {
        return CommandSPT2JSON(args);
    } else if (command == "decrypt") {
        return CommandDecrypt(args);
    } else if (command == "png2dds") {
        return CommandPNG2DDS(args);
    } else if (command == "ddsdecrypt") {
        return CommandDDSDecrypt(args);
    } else if (command == "batch") {
        return CommandBatch(args);
    } else if (command == "importrobot") {
        return CommandImportRobot(args);
    } else if (command == "ani2output") {
        return CommandANI2Output(args);
    } else if (command == "ani2anim") {
        return CommandANI2Anim(args);
    } else if (command == "ani2frames") {
        return CommandANI2Frames(args);
    } else if (command == "ani2ik") {
        return CommandANI2IK(args);
    } else if (command == "map") {
        return CommandMap(args);
    } else if (command == "x2mapanalysis") {
        return CommandMapAnalysis(args);
    } else if (command == "verifyfbx") {
        return CommandVerifyFBX(args);
    } else {
        std::cerr << "Unknown command: " << command << "\n\n";
        PrintUsage(argv[0]);
        return 1;
    }
}
