// ==========================================================================
// ScriptSPTParser — UKW Script.spt 场景配置文件解析器
//
// Script.spt = Shift-JIS 文本，定义：
//   天空/雾色/光照/水面/瓦片/建筑/BGM
// ==========================================================================

#include "ScriptSPTParser.h"
#include "XORCipher.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#define NOMINMAX
#include <windows.h>

namespace AssetTool {

// ==========================================================================
// Shift-JIS (CP932) → UTF-8
// ==========================================================================
static std::string SJISToUTF8(const std::string &sjis) {
    if (sjis.empty())
        return sjis;
    int wideLen = MultiByteToWideChar(932, 0, sjis.c_str(), (int)sjis.size(), nullptr, 0);
    if (wideLen <= 0)
        return sjis;
    std::wstring wide(wideLen, L'\0');
    MultiByteToWideChar(932, 0, sjis.c_str(), (int)sjis.size(), &wide[0], wideLen);
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0)
        return sjis;
    std::string utf8(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen, &utf8[0], utf8Len, nullptr, nullptr);
    return utf8;
}

// ==========================================================================
// 去除首尾空白
// ==========================================================================
static std::string Trim(const std::string &s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r'))
        start++;
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r'))
        end--;
    return s.substr(start, end - start);
}

// ==========================================================================
// 移除引号
// ==========================================================================
static std::string Unquote(const std::string &s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

// ==========================================================================
// 分割参数（逗号分隔，支持引号）
// ==========================================================================
static std::vector<std::string> SplitArgs(const std::string &line) {
    std::vector<std::string> args;
    std::string cur;
    bool inQuote = false;
    for (char c : line) {
        if (c == '"') {
            inQuote = !inQuote;
            cur += c;
            continue;
        }
        if (c == ',' && !inQuote) {
            args.push_back(Unquote(Trim(cur)));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        args.push_back(Unquote(Trim(cur)));
    return args;
}

// ==========================================================================
// 解析一个 float 参数
// ==========================================================================
static float ParseFloat(const std::string &s) {
    try {
        return std::stof(s);
    } catch (...) {
        return 0.0f;
    }
}

// ==========================================================================
// 解析一个 int 参数
// ==========================================================================
static int ParseInt(const std::string &s) {
    try {
        return std::stoi(s);
    } catch (...) {
        return 0;
    }
}

// ==========================================================================
// ScriptSPTParser::ParseLine — 解析单条指令
// ==========================================================================
void ScriptSPTParser::ParseLine(const std::string &line) {
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '\'')
        return; // 注释

    // 提取函数名和参数
    auto parenOpen = trimmed.find('(');
    auto parenClose = trimmed.rfind(')');
    if (parenOpen == std::string::npos || parenClose == std::string::npos)
        return;

    std::string funcName = Trim(trimmed.substr(0, parenOpen));
    std::string argsStr = trimmed.substr(parenOpen + 1, parenClose - parenOpen - 1);
    auto args = SplitArgs(argsStr);

    if (funcName == "LoadSkyXFile" && args.size() >= 4) {
        m_result.skyModel = args[0];
        m_result.fogR = ParseInt(args[1]);
        m_result.fogG = ParseInt(args[2]);
        m_result.fogB = ParseInt(args[3]);
    } else if (funcName == "SetLightColor" && args.size() >= 3) {
        m_result.lightR = ParseInt(args[0]);
        m_result.lightG = ParseInt(args[1]);
        m_result.lightB = ParseInt(args[2]);
    } else if (funcName == "LoadWaterXFile" && args.size() >= 1) {
        m_result.waterModel = args[0];
    } else if (funcName == "LoadHitXFile" && args.size() >= 1) {
        m_result.hitModel = args[0];
    } else if (funcName == "LoadMaterialXFile" && args.size() >= 1) {
        m_result.materialFile = args[0];
    } else if (funcName == "BgmFileName" && args.size() >= 2) {
        m_result.bgmFile = args[1];
    } else if (funcName == "LoadBuildingXFile" && args.size() >= 2) {
        m_result.buildingFiles.push_back(args[1]);
    } else if (funcName == "LoadMapXFile" && args.size() >= 3) {
        MapTileEntry entry;
        entry.index = ParseInt(args[0]);
        entry.modelFile = args[1];
        entry.hitFile = args[2];
        m_result.mapTiles.push_back(entry);
    } else if (funcName == "MapSetting" && args.size() >= 3) {
        MapSettingEntry entry;
        entry.index = ParseInt(args[0]);
        entry.xfileNo = ParseInt(args[1]);
        entry.waterHeight = ParseFloat(args[2]);
        m_result.mapSettings.push_back(entry);
    } else if (funcName == "MapSettingEx" && args.size() >= 11) {
        MapSettingEntry entry;
        entry.index = ParseInt(args[0]);
        entry.xfileNo = ParseInt(args[1]);
        entry.paramCount = 8;
        for (int i = 0; i < 8 && i + 2 < (int)args.size(); ++i)
            entry.params[i] = ParseInt(args[i + 2]);
        entry.waterHeight = ParseFloat(args[10]);
        m_result.mapSettings.push_back(entry);
    } else if (funcName == "SetBuilding" && args.size() >= 6) {
        BuildingInstance bld;
        bld.mapNo = ParseInt(args[0]);
        bld.index = ParseInt(args[1]);
        bld.buildNo = ParseInt(args[2]);
        bld.hp = ParseInt(args[3]);
        bld.posX = ParseFloat(args[4]);
        bld.posY = ParseFloat(args[5]);
        bld.posZ = (args.size() > 6) ? ParseFloat(args[6]) : 0;
        m_result.buildings.push_back(bld);
    }
}

// ==========================================================================
// ScriptSPTParser::ParseFile — 解析脚本文件
// ==========================================================================
bool ScriptSPTParser::ParseFile(const std::string &filepath) {
    m_result = SceneData();
    m_error.clear();
    m_result.sourceFile = filepath;

    // 读取文件（原始二进制）
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        m_error = "Cannot open file: " + filepath;
        return false;
    }
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char *>(buffer.data()), size);
    file.close();

    // 检测是否为 XOR 加密（SPT 首字节应为 0x27 即单引号）
    std::string content;
    if (size > 0 && buffer[0] == 0x27) {
        // 明文，直接使用
        content.assign(reinterpret_cast<const char *>(buffer.data()), size);
    } else {
        // 尝试 XOR 解密
        XORCipher cipher(0x0B7E7759);
        cipher.DecryptBuffer(buffer.data(), size);
        if (size > 0 && buffer[0] == 0x27) {
            content.assign(reinterpret_cast<const char *>(buffer.data()), size);
        } else {
            // 尝试其他 key
            uint32_t altKeys[] = {
                0x95127634, 0x19870430, 0xAC510B91, 0x13322366, 0xEF452301, 0x33333323, 0x33322166,
            };
            bool decrypted = false;
            for (uint32_t altKey : altKeys) {
                if (altKey == 0x0B7E7759)
                    continue;
                std::vector<uint8_t> altBuf(buffer);
                XORCipher altCipher(altKey);
                altCipher.DecryptBuffer(altBuf.data(), altBuf.size());
                if (altBuf[0] == 0x27) {
                    content.assign(reinterpret_cast<const char *>(altBuf.data()), altBuf.size());
                    decrypted = true;
                    break;
                }
            }
            if (!decrypted) {
                m_error = "Failed to decrypt Script.spt (tried multiple keys)";
                return false;
            }
        }
    }

    // Shift-JIS → UTF-8
    std::string utf8 = SJISToUTF8(content);

    // 逐行解析
    std::istringstream stream(utf8);
    std::string line;
    while (std::getline(stream, line)) {
        ParseLine(line);
    }

    return true;
}

// ==========================================================================
// SceneData::ToJSON — 输出 JSON
// ==========================================================================
std::string SceneData::ToJSON() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"sourceFile\": \"" << sourceFile << "\",\n";

    // 天空
    oss << "  \"sky\": {\n";
    oss << "    \"model\": \"" << skyModel << "\",\n";
    oss << "    \"fogColor\": [" << fogR << "," << fogG << "," << fogB << "]\n";
    oss << "  },\n";

    // 光照
    oss << "  \"light\": {\n";
    oss << "    \"color\": [" << lightR << "," << lightG << "," << lightB << "]\n";
    oss << "  },\n";

    // 水面
    if (!waterModel.empty())
        oss << "  \"water\": \"" << waterModel << "\",\n";

    // BGM
    if (!bgmFile.empty())
        oss << "  \"bgm\": \"" << bgmFile << "\",\n";

    // 瓦片映射
    if (!mapTiles.empty()) {
        oss << "  \"mapTiles\": [\n";
        for (size_t i = 0; i < mapTiles.size(); ++i) {
            const auto &t = mapTiles[i];
            oss << "    {\"index\":" << t.index << ",\"model\":\"" << t.modelFile << "\",\"hit\":\"" << t.hitFile
                << "\"}";
            if (i + 1 < mapTiles.size())
                oss << ",";
            oss << "\n";
        }
        oss << "  ],\n";
    }

    // 建筑
    if (!buildings.empty()) {
        oss << "  \"buildings\": [\n";
        for (size_t i = 0; i < buildings.size(); ++i) {
            const auto &b = buildings[i];
            oss << "    {\"map\":" << b.mapNo << ",\"index\":" << b.index << ",\"buildNo\":" << b.buildNo
                << ",\"hp\":" << b.hp << ",\"pos\":[" << b.posX << "," << b.posY << "," << b.posZ << "]}";
            if (i + 1 < buildings.size())
                oss << ",";
            oss << "\n";
        }
        oss << "  ]\n";
    }

    oss << "}\n";
    return oss.str();
}

// ==========================================================================
// SceneData::ToText — 输出可读文本
// ==========================================================================
std::string SceneData::ToText() const {
    std::ostringstream oss;

    oss << "Scene: " << sourceFile << "\n";
    oss << "================\n\n";

    oss << "Sky: " << skyModel << "\n";
    oss << "Fog Color: (" << fogR << "," << fogG << "," << fogB << ")\n";
    oss << "Light Color: (" << lightR << "," << lightG << "," << lightB << ")\n";
    oss << "Water: " << waterModel << "\n";
    oss << "Hit: " << hitModel << "\n";
    oss << "Material: " << materialFile << "\n";
    oss << "BGM: " << bgmFile << "\n\n";

    oss << "Buildings (" << buildingFiles.size() << " types):\n";
    for (size_t i = 0; i < buildingFiles.size(); ++i)
        oss << "  [" << i << "] " << buildingFiles[i] << "\n";
    oss << "\n";

    oss << "Map Tiles (" << mapTiles.size() << "):\n";
    for (const auto &t : mapTiles)
        oss << "  [" << t.index << "] " << t.modelFile << " (hit: " << t.hitFile << ")\n";
    oss << "\n";

    if (!buildings.empty()) {
        oss << "Building Placements (" << buildings.size() << "):\n";
        for (const auto &b : buildings)
            oss << "  Map[" << b.mapNo << "] "
                << "Build#" << b.buildNo << " "
                << "HP:" << b.hp << " "
                << "@(" << b.posX << ", " << b.posY << ", " << b.posZ << ")\n";
    }

    return oss.str();
}

// ==========================================================================
// SceneData::WriteJSON — 写入 JSON 文件
// ==========================================================================
bool SceneData::WriteJSON(const std::string &outputPath) const {
    std::ofstream ofs(outputPath);
    if (!ofs)
        return false;
    ofs << ToJSON();
    return true;
}

} // namespace AssetTool
