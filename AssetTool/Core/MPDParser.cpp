// ==========================================================================
// MPDParser — UKW .mpd 地图瓦片文件解析器
//
// .mpd 格式（逆向中）：
//   魔术头 "MPD"
//   名称表：根据 .x 文件名扫描
//   坐标段：待解析
// ==========================================================================

#define _CRT_SECURE_NO_WARNINGS

#include "MPDParser.h"
#include "XORCipher.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#define NOMINMAX
#include <windows.h>

namespace AssetTool {

// ==========================================================================
// SanitizeUTF8 — 过滤非 UTF-8 字符，替换为 '?'
// ==========================================================================
static std::string SanitizeUTF8(const std::string &input) {
    std::string result;
    for (size_t i = 0; i < input.size();) {
        uint8_t c = static_cast<uint8_t>(input[i]);
        if (c < 0x80) {
            // ASCII
            if (c >= 32 || c == '\t' || c == '\n')
                result += static_cast<char>(c);
            else
                result += '?';
            ++i;
        } else if (c < 0xC0) {
            // 非首字节，无效
            result += '?';
            ++i;
        } else if (c < 0xE0) {
            // 2 字节 UTF-8
            if (i + 1 < input.size() && (input[i + 1] & 0xC0) == 0x80) {
                result += input.substr(i, 2);
                i += 2;
            } else {
                result += '?';
                ++i;
            }
        } else if (c < 0xF0) {
            // 3 字节 UTF-8
            if (i + 2 < input.size() && (input[i + 1] & 0xC0) == 0x80 && (input[i + 2] & 0xC0) == 0x80) {
                result += input.substr(i, 3);
                i += 3;
            } else {
                result += '?';
                ++i;
            }
        } else if (c < 0xF8) {
            // 4 字节 UTF-8
            if (i + 3 < input.size() && (input[i + 1] & 0xC0) == 0x80 && (input[i + 2] & 0xC0) == 0x80 &&
                (input[i + 3] & 0xC0) == 0x80) {
                result += input.substr(i, 4);
                i += 4;
            } else {
                result += '?';
                ++i;
            }
        } else {
            result += '?';
            ++i;
        }
    }
    return result;
}

// ==========================================================================
// 工具函数
// ==========================================================================
static bool IsValidFloat(float f) { return std::isfinite(f) && std::fabs(f) < 1e15f; }

// ==========================================================================
// CP932ToUTF8 — Shift-JIS (CP932) 转 UTF-8（Windows API）
// ==========================================================================
static std::string CP932ToUTF8(const std::string &input) {
    if (input.empty())
        return input;
    // 先判断是否纯 ASCII（不需要转码）
    bool pureAscii = true;
    for (char c : input) {
        if (static_cast<uint8_t>(c) > 127) {
            pureAscii = false;
            break;
        }
    }
    if (pureAscii)
        return input;

    // CP932 → UTF-16 → UTF-8
    int wideLen = MultiByteToWideChar(932, 0, input.c_str(), (int)input.size(), nullptr, 0);
    if (wideLen <= 0)
        return SanitizeUTF8(input);

    std::wstring wide(wideLen, L'\0');
    MultiByteToWideChar(932, 0, input.c_str(), (int)input.size(), &wide[0], wideLen);

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0)
        return SanitizeUTF8(input);

    std::string utf8(utf8Len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wideLen, &utf8[0], utf8Len, nullptr, nullptr);
    return utf8;
}

// 判断字符是否为合法文件名组成字符（ASCII 范围）
static bool IsFileNameChar(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.' ||
           c == '-';
}

// 宽松判断：是否可能为文件名的一部分（ASCII 字母数字 + 常见日文 Shift-JIS 字节范围）
static bool IsPotentialFileNameChar(uint8_t c) {
    if (IsFileNameChar(c))
        return true;
    // Shift-JIS 首字节范围: 0x81-0x9F, 0xE0-0xEF
    // 尾字节范围: 0x40-0x7E, 0x80-0xFC
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF) || (c >= 0x40 && c <= 0x7E) || (c >= 0x80 && c <= 0xFC);
}

// ==========================================================================
// MPDParser::ExtractTileNames — 按 FFFF0001 标记 + 自动检测间距
//
// 规则：
//   第一个名字在头部之后（偏移 9）
//   后续名字：找 FFFF0001 + 4B → [num] 01 00 00 00 [filename.x\0]
//   自动检测第一个 mesh 到其 hit 的间距作为本文件固定步长
// ==========================================================================
void MPDParser::ExtractTileNames(const uint8_t *data, size_t size) {
    std::set<std::string> seen;
    const uint8_t ffff0001[] = {0xFF, 0xFF, 0x00, 0x01};

    // 找 .x\0 并提取文件名
    auto extractName = [&](size_t searchStart, size_t searchEnd) -> std::string {
        for (size_t i = searchStart; i + 3 <= searchEnd; ++i) {
            if (data[i] != 0x2E || data[i + 1] != 0x78 || data[i + 2] != 0x00)
                continue;
            int nameStart = static_cast<int>(i);
            while (nameStart > static_cast<int>(searchStart) && IsFileNameChar(data[nameStart - 1]))
                --nameStart;
            uint8_t first = data[nameStart];
            if (!(first >= '0' && first <= '9') && !(first >= 'A' && first <= 'Z') && !(first >= 'a' && first <= 'z'))
                return "";
            std::string raw(reinterpret_cast<const char *>(data + nameStart), static_cast<size_t>(i - nameStart + 2));
            std::string name = CP932ToUTF8(raw);
            std::string stem = name;
            auto dot = stem.rfind('.');
            if (dot != std::string::npos) stem = stem.substr(0, dot);
            if (stem.size() < 2 || seen.count(name))
                return "";
            seen.insert(name);
            return name;
        }
        return "";
    };

    // 按 FFFF0001 顺序构建名字表
    // 每个 FFFF0001 对应一个索引，空标记也占位
    // 第一个名字在头部偏移 9，无 FFFF0001
    std::vector<std::string> names;

    // 第一个名字：头部（偏移 9）后直接找 .x\0
    std::string firstName = extractName(9, 256);
    if (!firstName.empty()) names.push_back(firstName);

    // 后续名字：每个 FFFF0001 对应一个条目
    for (size_t i = 0; i + 16 < size; ++i) {
        if (std::memcmp(data + i, ffff0001, 4) != 0)
            continue;
        size_t searchStart = i + 4 + 4;
        size_t searchEnd = std::min(searchStart + 256, size);
        std::string name = extractName(searchStart, searchEnd);
        names.push_back(name); // 空字符串也占位
    }

    m_result.tileNames = names;
}

// ==========================================================================
// MPDParser::ParseCoordinateSection — 网格推算 tile 坐标
//
// 原理：MPD 的二进制 tile 位置格式过于复杂，绕过二进制解析，
// 直接根据 tile 在资产表中的索引顺序推算网格位置。
// 网格尺寸从 tile 总数推算（已知地图为近似正方形网格）。
//
// City 类地图网格参数：
//   - mapChip 瓦片约 20 单位宽
//   - 排列为 rows × cols 网格（从 tile 总数开平方估算）
// ==========================================================================
void MPDParser::ParseCoordinateSection(const uint8_t *data, size_t size) {
    std::vector<MPDTile> parsed;
    std::set<size_t> seenPos;

    const uint8_t marker3F[] = {0x00, 0x00, 0x80, 0x3F};
    const uint8_t markerBF[] = {0x00, 0x00, 0x80, 0xBF};

    // 检查标记：XX 00 80 3F（首字节可变，后 3 字节固定）
    auto isMarker = [&](size_t pos) -> bool {
        return pos + 4 <= size && data[pos + 1] == 0x00 && data[pos + 2] == 0x80 && data[pos + 3] == 0x3F;
    };
    // 检查 BF 标记：XX 00 80 BF（首字节可变）
    auto isBFMarker = [&](size_t pos) -> bool {
        return pos + 4 <= size && data[pos + 1] == 0x00 && data[pos + 2] == 0x80 && data[pos + 3] == 0xBF;
    };

    // 统计 Type A 锚点的 XX 值分布
    std::map<uint8_t, int> xxCounts;
    for (size_t i = 0; i + 8 < size; ++i) {
        if (data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x00 &&
            data[i + 4] == 0x00 && data[i + 5] == 0x00 &&
            data[i + 6] == 0x80 && data[i + 7] == 0x3F) {
            uint8_t xx = data[i];
            if (xx != 0) xxCounts[xx]++;
        }
    }
    uint8_t dominantXX = 0;
    int maxCount = 0;
    for (const auto &[xx, count] : xxCounts) {
        if (count > maxCount) { maxCount = count; dominantXX = xx; }
    }

    // 解析辅助 lambda：从锚点 off 处解析一个 Type A 条目，返回 m4 位置（0=失败）
    auto parseTileAtAnchor = [&](size_t off, uint8_t xx) -> size_t {
        size_t m1 = off + 4;
        size_t m2 = m1 + 20;
        size_t m3 = m2 + 20;
        size_t m4 = m3 + 20;
        if (m4 + 10 > size) return 0;
        if (!isMarker(m1)) return 0;
        if (!isMarker(m2)) return 0;
        if (!isMarker(m3)) return 0;
        if (!isMarker(m4)) return 0;

        size_t gap3Off = m3 + 4;
        float x = *reinterpret_cast<const float *>(data + gap3Off + 4);
        float y = *reinterpret_cast<const float *>(data + gap3Off + 8);
        float z = *reinterpret_cast<const float *>(data + gap3Off + 12);
        (void)y;

        uint32_t index = static_cast<uint32_t>(data[m4 + 4]) |
                        (static_cast<uint32_t>(data[m4 + 5]) << 8);

        // 渲染文本（m4 后固定 6 字节偏移处开始，遇连续 00 停止）
        std::string renderText;
        size_t tp = m4 + 4 + 6;
        int zeroRun = 0;
        while (tp < size && zeroRun < 2) {
            if (data[tp] == 0x00) { ++zeroRun; ++tp; continue; }
            zeroRun = 0;
            if (data[tp] >= 0x20 && data[tp] <= 0x7E) {
                renderText += static_cast<char>(data[tp]);
            } else if (data[tp] == 0x0D || data[tp] == 0x0A || data[tp] == 0x09) {
                renderText += static_cast<char>(data[tp]);
            } else if (data[tp] >= 0x80) {
                renderText += static_cast<char>(data[tp]);
            } else { break; }
            ++tp;
            if (renderText.size() > 240) break;
        }

        if (x > -10000 && x < 10000 && z > -10000 && z < 10000 &&
            std::isfinite(x) && std::isfinite(z)) {
            MPDTile tile;
            tile.tileIndex = index;
            tile.posX = x;
            tile.posZ = z;
            tile.renderText = renderText;
            parsed.push_back(tile);
            // 输出条目二进制（以索引为中心）用于验证
            std::cout << "[TILE] idx=" << index << " raw=";
            size_t hs = m4 + 4, he = std::min(m4 + 16, size);
            for (size_t hi = hs; hi < he; ++hi)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)data[hi];
            std::cout << std::dec << "\n";
            return m4;
        }
        return 0;
    };

    // ================================================================
    // 步骤1：解析 Type A 条目 (XX 00 00 00 00 00 80 3F + 4×3F)
    // ================================================================
    if (dominantXX != 0) {
        for (size_t off = 0; off + 80 < size; ++off) {
            if (data[off] != dominantXX) continue;
            if (std::memcmp(data + off + 1, "\x00\x00\x00\x00\x00", 5) != 0) continue;
            if (data[off + 6] != 0x80 || data[off + 7] != 0x3F) continue;
            if (seenPos.count(off)) continue;
            seenPos.insert(off);

            size_t m4 = parseTileAtAnchor(off, dominantXX);
            if (m4 != 0) {
                // 连续条目：紧跟 m4+8 处可能有 XX=0 的条目
                size_t nextOff = m4 + 8;
                while (nextOff + 80 < size) {
                    if (data[nextOff] != 0x00) break;
                    if (std::memcmp(data + nextOff + 1, "\x00\x00\x00\x00\x00", 5) != 0) break;
                    if (data[nextOff + 6] != 0x80 || data[nextOff + 7] != 0x3F) break;
                    if (seenPos.count(nextOff)) break;
                    seenPos.insert(nextOff);
                    size_t nm4 = parseTileAtAnchor(nextOff, 0);
                    if (nm4 == 0) break;
                    nextOff = nm4 + 8;
                }
            }
        }
    }

    // ================================================================
    // 步骤2：解析 Type B 条目 (00 00 80 BF + 3×3F, 8-8-24 gaps)
    // ================================================================
    std::set<size_t> seenBF;
    for (size_t off = 0; off + 80 < size; ++off) {
        if (!isBFMarker(off)) continue;
        if (seenBF.count(off)) continue;
        seenBF.insert(off);

        // 验证后续 3 个 3F 标记，间隔 8-8-24
        size_t m2 = off + 4 + 8;    // off + 4 + 8
        size_t m3 = m2 + 4 + 8;     // m2 + 4 + 8
        size_t m4 = m3 + 4 + 24;    // m3 + 4 + 24

        if (m4 + 10 > size) continue;
        if (!isMarker(m2)) continue;
        if (!isMarker(m3)) continue;
        if (!isMarker(m4)) continue;

        // 坐标在 gap3 (m3+4 ~ m4)，偏移 +12 (X) 和 +20 (Z)
        size_t gap3Off = m3 + 4;
        float x = *reinterpret_cast<const float *>(data + gap3Off + 12);
        float z = *reinterpret_cast<const float *>(data + gap3Off + 20);

        // 索引在 m4 后
        uint32_t index = static_cast<uint32_t>(data[m4 + 4]) |
                        (static_cast<uint32_t>(data[m4 + 5]) << 8);

        // 渲染文本（m4 后固定 6 字节偏移处开始，遇连续 00 停止）
        std::string renderText;
        size_t tp = m4 + 4 + 6;
        int zeroRun = 0;
        while (tp < size && zeroRun < 2) {
            if (data[tp] == 0x00) { ++zeroRun; ++tp; continue; }
            zeroRun = 0;
            if (data[tp] >= 0x20 && data[tp] <= 0x7E) {
                renderText += static_cast<char>(data[tp]);
            } else if (data[tp] == 0x0D || data[tp] == 0x0A || data[tp] == 0x09) {
                renderText += static_cast<char>(data[tp]);
            } else if (data[tp] >= 0x80) {
                renderText += static_cast<char>(data[tp]);
            } else {
                break;
            }
            ++tp;
            if (renderText.size() > 240) break;
        }

        if (x > -10000 && x < 10000 && z > -10000 && z < 10000 &&
            std::isfinite(x) && std::isfinite(z)) {
            MPDTile tile;
            tile.tileIndex = index;
            tile.posX = x;
            tile.posZ = z;
            tile.renderText = renderText;
            parsed.push_back(tile);
            // 输出 BF 条目二进制（以索引为中心）用于验证
            std::cout << "[TILE] idx=" << index << " (0x" << std::hex << index << std::dec
                      << ") @" << x << "," << z << " raw=";
            size_t hexStart = m4 + 4;
            size_t hexEnd = std::min(m4 + 16, size);
            for (size_t hi = hexStart; hi < hexEnd; ++hi)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)data[hi];
            std::cout << std::dec << "\n";
        }
    }

    // ================================================================
    // 步骤3：去重（按 index + 坐标去重）
    // ================================================================
    std::set<std::pair<uint32_t, uint64_t>> dedup;
    std::vector<MPDTile> unique;
    for (const auto &tile : parsed) {
        uint64_t coordKey = (static_cast<uint64_t>(
            *reinterpret_cast<const uint32_t *>(&tile.posX)) << 32) |
            *reinterpret_cast<const uint32_t *>(&tile.posZ);
        auto key = std::make_pair(tile.tileIndex, coordKey);
        if (dedup.find(key) == dedup.end()) {
            dedup.insert(key);
            unique.push_back(tile);
        }
    }

    if (!unique.empty())
        m_result.tiles = unique;
}

// ==========================================================================
// MPDParser::Parse — 解析已解密的 MPD 数据
// ==========================================================================
bool MPDParser::Parse(const uint8_t *data, size_t size) {
    m_result = MPDData();
    m_error.clear();

    if (size < 8 || std::memcmp(data, "MPD", 3) != 0) {
        m_error = "Not a valid .mpd file (missing 'MPD' magic). "
                  "File may need XOR decryption.";
        return false;
    }

    // 头部偏移7-8是固定值 C8 00 = 200（所有地图一致），不是 tile 实例数

    ExtractTileNames(data, size);

    // 即使名字表为空，也尝试解析坐标段
    ParseCoordinateSection(data, size);

    // 将 tileNames 中的名称关联到 tile 上，跳过空占位
    // 二进制索引基于原始名称表（无空占位），需要跳过空槽位
    for (auto &tile : m_result.tiles) {
        int nonEmptyIdx = 0;
        std::string foundName;
        for (size_t ni = 0; ni < m_result.tileNames.size(); ++ni) {
            if (!m_result.tileNames[ni].empty()) {
                if (nonEmptyIdx == (int)tile.tileIndex) {
                    foundName = m_result.tileNames[ni];
                    break;
                }
                nonEmptyIdx++;
            }
        }
        if (!foundName.empty()) {
            tile.name = foundName;
        } else {
            tile.name = std::to_string(tile.tileIndex) + ".x";
        }
    }

    // 提取文本配置指令（LightColor, FogColor, BGM, PopInfo 等）
    ExtractConfigText(data, size);

    // 提取 00 00 80 3F 后的文本标记
    ExtractRenderSettings(data, size);

    // 提取 PopInfo 出生点数据
    ExtractPopInfo(data, size);

    if (m_result.tileNames.empty() && m_result.tiles.empty()) {
        m_error = "No tile data found in .mpd file";
        return false;
    }

    return true;
}

// ==========================================================================
// MPDParser::ParseFile — 从文件加载（自动检测 XOR 加密）
// ==========================================================================
bool MPDParser::ParseFile(const std::string &filepath, uint32_t decryptKey) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        m_error = "Cannot open file: " + filepath;
        return false;
    }
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char *>(buffer.data()), size);

    m_result.filepath = filepath;

    if (size >= 4 && std::memcmp(buffer.data(), "MPD", 3) == 0) {
        return Parse(buffer.data(), buffer.size());
    }

    {
        XORCipher cipher(decryptKey);
        cipher.DecryptBuffer(buffer.data(), buffer.size());
        if (Parse(buffer.data(), buffer.size()))
            return true;
    }

    uint32_t altKeys[] = {
        0x95127634, 0x19870430, 0xAC510B91, 0x13322366, 0xEF452301, 0x33333323, 0x33322166,
    };
    for (uint32_t altKey : altKeys) {
        if (altKey == decryptKey)
            continue;
        std::vector<uint8_t> altBuf(buffer);
        XORCipher altCipher(altKey);
        altCipher.DecryptBuffer(altBuf.data(), altBuf.size());
        if (Parse(altBuf.data(), altBuf.size()))
            return true;
    }

    m_error = "Failed to parse .mpd file (tried plain + " + std::to_string(1 + sizeof(altKeys) / sizeof(altKeys[0])) +
              " keys)";
    return false;
}

// ==========================================================================
// 提取文本配置指令（SPT 风格的明文）
// ==========================================================================

void MPDParser::ExtractConfigText(const uint8_t *data, size_t size) {
    // 使用 FFFF00XX + 3B 模式定位配置段明文
    // 同时回退扫描已知文本模式（LightColor, FogColor 等）
    std::set<std::string> seenText;

    // 方法1：FFFF00XX + 3B 模式定位
    // 文本从 FFFF00XX 后跳过 3 字节开始，到下一个 FFFF00XX 或 00 00 80 3F 结束
    const uint8_t ffff00[] = {0xFF, 0xFF, 0x00};
    for (size_t i = 0; i + 12 < size; ++i) {
        if (std::memcmp(data + i, ffff00, 3) != 0)
            continue;
        if (data[i + 3] == 0x01) // 跳过 FFFF0001（名字表）
            continue;
        size_t textPos = i + 4 + 3; // FFFF00 XX (4B) + 3B gap
        // 跳过前导 00
        while (textPos < size && data[textPos] == 0x00) ++textPos;
        if (textPos + 5 >= size) continue;

        // 收集文本直到结束标记 64 00 64 00 00 00 F0 41 或 FFFF00 或 00 00 80 3F
        // 用 00 作为字段分隔符，每个 00 分隔的段独立成条目
        std::vector<uint8_t> currentField;
        auto flushField = [&]() {
            if (currentField.size() < 3) { currentField.clear(); return; }
            currentField.push_back(0);
            std::string raw(reinterpret_cast<const char *>(currentField.data()));
            std::string decoded = CP932ToUTF8(raw);
            if (!decoded.empty() && seenText.count(decoded) == 0) {
                seenText.insert(decoded);
                m_result.textSections.push_back(decoded);
            }
            currentField.clear();
        };

        const uint8_t endMark[] = {0x64, 0x00, 0x64, 0x00, 0x00, 0x00, 0xF0, 0x41};
        bool foundEnd = false;
        for (size_t j = textPos; j < size && j < textPos + 1000; ++j) {
            if (j + 8 <= size && std::memcmp(data + j, endMark, 8) == 0)
                break;
            if (j + 6 < size && data[j] == 0xFF && data[j + 1] == 0xFF && data[j + 2] == 0x00) {
                foundEnd = true;
                break;
            }
            if (j + 4 < size && data[j] == 0x00 && data[j + 1] == 0x00 && data[j + 2] == 0x80 && data[j + 3] == 0x3F)
                break;
            if (data[j] == 0x00) {
                flushField();
                continue;
            }
            if (data[j] == 0x0D || data[j] == 0x0A || data[j] == 0x20)
                continue;
            currentField.push_back(data[j]);
        }
        flushField();

        if (foundEnd) {
            i = i + 4 + 3;
        }
    }

    // 方法2：在全文中搜索已知配置指令（兼容旧格式）
    std::string text(reinterpret_cast<const char *>(data), size);

    auto parseIntTriple = [](const std::string &s, int &a, int &b, int &c) {
        sscanf(s.c_str(), "%d,%d,%d", &a, &b, &c);
    };

    auto extractParen = [&](size_t start) -> std::string {
        size_t open = text.find('(', start);
        if (open == std::string::npos) return {};
        size_t close = text.find(')', open);
        if (close == std::string::npos) return {};
        return text.substr(open + 1, close - open - 1);
    };

    size_t pos = 0;

    // LightColor(r,g,b)
    while ((pos = text.find("LightColor", pos)) != std::string::npos) {
        auto args = extractParen(pos);
        if (!args.empty())
            parseIntTriple(args, m_result.lightR, m_result.lightG, m_result.lightB);
        pos += 10;
    }

    // FogColor(r,g,b)
    pos = 0;
    while ((pos = text.find("FogColor", pos)) != std::string::npos) {
        auto args = extractParen(pos);
        if (!args.empty())
            parseIntTriple(args, m_result.fogR, m_result.fogG, m_result.fogB);
        pos += 8;
    }

    // LightDir(x,y,z)
    pos = 0;
    while ((pos = text.find("LightDir", pos)) != std::string::npos) {
        auto args = extractParen(pos);
        if (!args.empty())
            sscanf(args.c_str(), "%f,%f,%f", &m_result.lightDirX, &m_result.lightDirY, &m_result.lightDirZ);
        pos += 8;
    }

    // BgmFileName(idx, name)
    pos = 0;
    while ((pos = text.find("BgmFileName", pos)) != std::string::npos) {
        auto args = extractParen(pos);
        if (!args.empty()) {
            auto comma = args.find(',');
            if (comma != std::string::npos) {
                m_result.bgmFile = args.substr(comma + 1);
                while (!m_result.bgmFile.empty() && m_result.bgmFile.front() == ' ') m_result.bgmFile.erase(0, 1);
                while (!m_result.bgmFile.empty() && m_result.bgmFile.back() == ' ') m_result.bgmFile.pop_back();
            }
        }
        pos += 11;
    }

    // PopInfo(idx, ...) — 去重，只保留唯一值
    pos = 0;
    std::set<std::string> seenPop;
    while ((pos = text.find("PopInfo(", pos)) != std::string::npos) {
        auto close = text.find(')', pos);
        if (close == std::string::npos || close - pos > 80) { pos += 7; continue; }
        std::string args = text.substr(pos + 8, close - pos - 8);
        int firstVal = 0;
        if (sscanf(args.c_str(), "%d", &firstVal) == 1 && firstVal >= 0 && firstVal <= 999) {
            std::string line = "PopInfo(" + args + ")";
            if (seenPop.find(line) == seenPop.end()) {
                seenPop.insert(line);
                m_result.popInfo.push_back(line);
            }
        }
        pos = close + 1;
    }
}

// ==========================================================================
// MPDData::ToText — 输出可读文本
// ==========================================================================
std::string MPDData::ToText() const {
    std::ostringstream oss;

    oss << "MPD File Information\n";
    oss << "====================\n\n";
    oss << "File Signature: MPD\n";
    oss << "Number of Tile Names (scanned): " << tileNames.size() << "\n";
    oss << "Number of Tile (parsed): " << tiles.size() << "\n\n";

    if (!tileNames.empty()) {
        oss << "--- Tile Name Table ---\n";
        for (size_t i = 0; i < tileNames.size(); ++i) {
            oss << "  [" << i << "] " << SanitizeUTF8(tileNames[i]) << "\n";
        }
        oss << "\n";
    }

    if (!tiles.empty()) {
        oss << "--- Tile Coordinates ---\n";
        for (size_t i = 0; i < tiles.size(); ++i) {
            const auto &tile = tiles[i];
            oss << "Index:" << static_cast<int>(tile.tileIndex) << "  Name:" << tile.name << "  X:" << std::fixed
                << std::setprecision(6) << tile.posX << "  Y:" << std::fixed << std::setprecision(6) << tile.posZ;
            if (!tile.renderText.empty())
                oss << "  Render: " << SanitizeUTF8(tile.renderText);
            if (!tile.rawData.empty()) {
                oss << "  pre=";
                for (size_t bi = 0; bi < tile.rawData.size() && bi < 16; ++bi)
                    oss << std::hex << std::setfill('0') << std::setw(2) << (int)tile.rawData[bi];
                oss << std::dec;
            }
            oss << "\n";
        }
    }

    // ---- 原始明文标记段 ----
    if (!textSections.empty()) {
        oss << "\n--- Raw Text Markers (" << textSections.size() << " sections) ---\n";
        for (size_t i = 0; i < textSections.size(); ++i)
            oss << "  [" << i << "] " << SanitizeUTF8(textSections[i]) << "\n";
    }

    // ---- 文本配置段 ----
    oss << "\n--- Config Text ---\n";
    oss << "LightColor(" << lightR << "," << lightG << "," << lightB << ");\n";
    oss << "FogColor(" << fogR << "," << fogG << "," << fogB << ");\n";
    oss << "LightDir(" << lightDirX << "," << lightDirY << "," << lightDirZ << ");\n";
    if (!bgmFile.empty())
        oss << "BgmFileName(0," << bgmFile << ");\n";
    for (const auto &pi : popInfo)
        oss << pi << ";\n";

    // ---- 00 00 80 3F 文本标记（渲染参数） ----
    if (!renderSettings.empty()) {
        oss << "\n--- Render Settings (" << renderSettings.size() << " unique, "
            << renderWithAt << " with @, " << renderWithoutAt << " without @) ---\n";
        for (size_t i = 0; i < renderSettings.size(); ++i)
            oss << "  [" << i << "] " << SanitizeUTF8(renderSettings[i]) << "\n";
    }

    // ---- PopInfo 出生点 ----
    if (!popInfoEntries.empty()) {
        oss << "\n--- PopInfo (" << popInfoEntries.size() << " entries) ---\n";
        for (const auto &pi : popInfoEntries) {
            oss << "  [" << pi.popIndex << "] Pos=(" << std::fixed << std::setprecision(2)
                << pi.posX << ", " << pi.posY << ", " << pi.posZ << ")"
                << "  Params=" << pi.param1 << "," << pi.param2
                << "  Text: " << SanitizeUTF8(pi.rawText) << "\n";
        }
    }

    return oss.str();
}

bool MPDData::WriteText(const std::string &outputPath) const {
    std::ofstream ofs(outputPath);
    if (!ofs)
        return false;
    ofs << ToText();
    return true;
}

// ==========================================================================
// MPDData::FilterByExistingFiles — 用真实资产剔除不存在的名字，重算索引
// ==========================================================================
void MPDData::FilterByExistingFiles(const std::vector<std::string> &existingFiles) {
    if (tileNames.empty())
        return;

    // 构建真实文件名集合（全小写，去掉路径只留文件名）
    // 同时保留原始顺序，用于后续补充缺失条目
    struct FileEntry { std::string name; std::string lower; };
    std::vector<FileEntry> realFiles;
    std::set<std::string> realFileSet;
    for (const auto &path : existingFiles) {
        std::string name = path;
        auto sep = name.find_last_of("/\\");
        if (sep != std::string::npos)
            name = name.substr(sep + 1);
        std::string lower;
        lower.reserve(name.size());
        for (char c : name)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (realFileSet.insert(lower).second)
            realFiles.push_back({name, lower});
    }

    if (realFiles.empty())
        return;

    // 过滤：只保留在 realFiles 中存在的名字，空占位保留
    std::vector<std::string> filtered;
    std::set<std::string> existingNames;
    for (const auto &n : tileNames) {
        if (n.empty()) {
            filtered.push_back(n); // 空占位保留
            continue;
        }
        std::string lower;
        lower.reserve(n.size());
        for (char c : n)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (realFileSet.count(lower)) {
            filtered.push_back(n);
            existingNames.insert(lower);
        }
    }

    // 补充：将 realFiles 中不在 name table 的文件按扫描顺序追加到尾部
    for (const auto &fe : realFiles) {
        if (!existingNames.count(fe.lower))
            filtered.push_back(fe.name);
    }

    size_t before = tileNames.size();
    tileNames = filtered;
    size_t after = tileNames.size();

    std::cout << "[FilterByExistingFiles] " << before << " → " << after
              << " tile names (removed " << (before - after) << " non-existing)\n";
}

// ==========================================================================
// MPDParser::ExtractRenderSettings — 提取 00 00 80 3F 后的文本标记
//
// 00 00 80 3F + 6 字节后取非 00 内容作为渲染配置明文
// 检测 @ 标记但不省略，用于统计
// ==========================================================================
void MPDParser::ExtractRenderSettings(const uint8_t *data, size_t size) {
    std::vector<std::string> settings;
    int countWithAt = 0, countWithoutAt = 0;
    const int gap = 6;

    for (size_t i = 0; i + 10 + gap < size; ++i) {
        if (data[i] != 0x00 || data[i + 1] != 0x00 || data[i + 2] != 0x80 || data[i + 3] != 0x3F)
            continue;

        size_t textPos = i + 4 + gap;
        if (textPos + 4 >= size) continue;

        // 跳过前导 00
        while (textPos < size && data[textPos] == 0x00) ++textPos;
        if (textPos + 4 >= size) continue;

        // 收集文本直到遇到连续 00 或 00 00 80 3F
        std::vector<uint8_t> textBytes;
        int zeroRun = 0;
        bool hasAt = false;
        for (size_t j = textPos; j < size && j < textPos + 500; ++j) {
            uint8_t b = data[j];
            if (b == 0x00) {
                ++zeroRun;
                if (zeroRun >= 3) break;
                continue;
            }
            zeroRun = 0;
            if (b == 0x0D || b == 0x0A) continue;
            if (b == 0x40) hasAt = true; // @
            textBytes.push_back(b);
        }
        if (textBytes.size() < 4) continue;

        if (hasAt) countWithAt++; else countWithoutAt++;

        textBytes.push_back(0);
        std::string raw(reinterpret_cast<const char *>(textBytes.data()));
        std::string decoded = CP932ToUTF8(raw);

        // 去重
        bool dup = false;
        for (const auto &s : settings) {
            if (s == decoded) { dup = true; break; }
        }
        if (!dup && decoded.size() > 2)
            settings.push_back(decoded);
    }

    m_result.renderSettings = settings;
    m_result.renderWithAt = countWithAt;
    m_result.renderWithoutAt = countWithoutAt;
}

// ==========================================================================
// MPDParser::ExtractPopInfo — 提取 PopInfo 出生点数据
//
// 标记2: 00 00 80 3F（确定）
// 标记1: 00 00 80 XF（X 任意，在标记2之前）
// 标记3: 00 00 80 3F（在 PopInfo 文本之后）
// 先找标记2，再向两边扩展找标记1和标记3
// ==========================================================================
void MPDParser::ExtractPopInfo(const uint8_t *data, size_t size) {
    std::vector<MPDPopInfo> entries;
    const uint8_t marker3F[] = {0x00, 0x00, 0x80, 0x3F};

    // 找所有 00 00 80 3F 位置
    std::vector<size_t> m3fPos;
    for (size_t i = 0; i + 4 < size; ++i) {
        if (std::memcmp(data + i, marker3F, 4) == 0)
            m3fPos.push_back(i);
    }

    // 解析 PopInfo 段
    for (size_t mi = 0; mi + 1 < m3fPos.size(); ++mi) {
        size_t m2 = m3fPos[mi];       // 标记2（当前）
        size_t m3 = m3fPos[mi + 1];   // 标记3（下一个）

        // 标记2 后跳过前导00，找 PopInfo 文本
        size_t textPos = m2 + 4;
        while (textPos < size && data[textPos] == 0x00) ++textPos;
        // 读参数（2 × uint16，8字节）
        if (textPos + 8 > m3) continue;
        int param1 = static_cast<int>(data[textPos]) | (static_cast<int>(data[textPos + 1]) << 8);
        int param2 = static_cast<int>(data[textPos + 2]) | (static_cast<int>(data[textPos + 3]) << 8);
        textPos += 8;

        // 找 PopInfo 文本
        size_t popStart = textPos;
        while (popStart + 8 < m3) {
            if (data[popStart] == 'P' && data[popStart + 1] == 'o' && data[popStart + 2] == 'p')
                break;
            ++popStart;
        }
        if (popStart + 8 >= m3) continue;

        int popIdx = 0;
        sscanf(reinterpret_cast<const char *>(data + popStart + 8), "%d", &popIdx);

        // 提取文本：只取到 PopInfo(...); 的 ; 为止
        size_t textEnd = popStart;
        while (textEnd + 1 < m3) {
            // 找到 ; 结束
            if (data[textEnd] == 0x3B) {
                // 检查后面是 0D 0A 或 00 → 确认是结束
                if (textEnd + 1 < m3 && (data[textEnd + 1] == 0x0D || data[textEnd + 1] == 0x00))
                    break;
            }
            // 检查后缀 00 2E BD 3B B3
            if (textEnd + 5 <= m3 && data[textEnd] == 0x00 && data[textEnd + 1] == 0x2E &&
                data[textEnd + 2] == 0xBD && data[textEnd + 3] == 0x3B && data[textEnd + 4] == 0xB3)
                break;
            ++textEnd;
        }

        std::string rawText(reinterpret_cast<const char *>(data + popStart), textEnd - popStart);

        // 找标记1：从标记2往前找 00 00 80 XF（X 任意，末字节以 0F 结尾）
        size_t m1 = 0;
        for (int back = 4; back < 60 && back <= (int)m2; ++back) {
            size_t pos = m2 - back;
            if (data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x80 &&
                (data[pos + 3] & 0x0F) == 0x0F) {
                m1 = pos;
                break;
            }
        }

        // 如果没找到标记1，跳过
        if (m1 == 0) continue;

        // 在标记1→标记2之间找坐标 float
        float posX = 0, posY = 0, posZ = 0;
        for (size_t off = m1 + 4; off + 12 <= m2; off += 4) {
            float x = *reinterpret_cast<const float *>(data + off);
            float y = *reinterpret_cast<const float *>(data + off + 4);
            float z = *reinterpret_cast<const float *>(data + off + 8);
            if (x > 100 && x < 10000 && z > 100 && z < 10000) {
                posX = x; posY = y; posZ = z;
                break;
            }
        }

        MPDPopInfo entry;
        entry.popIndex = popIdx;
        entry.posX = posX; entry.posY = posY; entry.posZ = posZ;
        entry.param1 = param1; entry.param2 = param2;
        entry.rawText = rawText;
        entries.push_back(entry);

        // 跳到标记3
        mi++; // 跳过标记3
    }

    m_result.popInfoEntries = entries;
}

} // namespace AssetTool
