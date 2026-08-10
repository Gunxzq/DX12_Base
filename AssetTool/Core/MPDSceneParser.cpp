#include "MPDSceneParser.h"
#include "XORCipher.h"

#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include <windows.h>

namespace AssetTool {

// ==========================================================================
// CP932ToUTF8 — Shift-JIS (CP932) 转 UTF-8（Windows API）
// ==========================================================================
std::string MPDSceneParser::CP932ToUTF8(const std::string &sjis) {
    if (sjis.empty())
        return sjis;
    bool pureAscii = true;
    for (char c : sjis) {
        if (static_cast<uint8_t>(c) > 127) {
            pureAscii = false;
            break;
        }
    }
    if (pureAscii)
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
// 读取辅助
// ==========================================================================
std::string MPDSceneParser::ReadFixedString(const uint8_t *data, size_t size, size_t &off, size_t len) {
    size_t end = off;
    while (end < off + len && end < size && data[end] != 0)
        ++end;
    std::string s(reinterpret_cast<const char *>(data + off), end - off);
    off += len; // 定长槽始终前进完整长度
    return CP932ToUTF8(s);
}

std::string MPDSceneParser::ReadVarString(const uint8_t *data, size_t size, size_t &off, uint32_t len) {
    std::string s;
    if (off + len <= size)
        s.assign(reinterpret_cast<const char *>(data + off), len);
    off += len;
    return CP932ToUTF8(s);
}

// ==========================================================================
// 解析（权威结构，mpd.cs load 对照）
// ==========================================================================
bool MPDSceneParser::Parse(const uint8_t *data, size_t size) {
    m_scene = MPDScene();
    m_error.clear();

    if (size < 9 || std::memcmp(data, "MPD", 3) != 0) {
        m_error = "Not a valid .mpd file (missing 'MPD' magic). File may need XOR decryption.";
        return false;
    }

    size_t off = 3;

    // [头部] WorldParts int32 + PiecesCount int16
    int32_t worldParts = 0;
    int16_t piecesCount = 0;
    std::memcpy(&worldParts, data + off, 4);
    off += 4;
    std::memcpy(&piecesCount, data + off, 2);
    off += 2;
    m_scene.worldParts = static_cast<uint32_t>(worldParts);

    if (piecesCount <= 0 || piecesCount > 4096) {
        m_error = "Invalid PiecesCount: " + std::to_string(piecesCount);
        return false;
    }

    // [Pieces 表] 每条：256B visual + 256B collision + 3B + int32 脚本长 + 脚本
    m_scene.pieces.reserve(static_cast<size_t>(piecesCount));
    for (int i = 0; i < piecesCount; ++i) {
        MPDScenePiece piece;
        piece.visualMesh = ReadFixedString(data, size, off, 256);
        piece.collisionMesh = ReadFixedString(data, size, off, 256);
        off += 3; // 3B 填充（FF FF 00）
        int32_t txtCount = 0;
        std::memcpy(&txtCount, data + off, 4);
        off += 4;
        if (txtCount > 0) {
            if (off + static_cast<size_t>(txtCount) > size) {
                m_error = "Pieces script overrun at piece " + std::to_string(i);
                return false;
            }
            piece.scriptText = ReadVarString(data, size, off, static_cast<uint32_t>(txtCount));
        }
        m_scene.pieces.push_back(std::move(piece));
    }

    // [Grid 区] int16 x + int16 y + float unk + x×y 格
    int16_t gx = 0, gy = 0;
    std::memcpy(&gx, data + off, 2);
    off += 2;
    std::memcpy(&gy, data + off, 2);
    off += 2;
    std::memcpy(&m_scene.unk, data + off, 4);
    off += 4;
    m_scene.gridX = gx;
    m_scene.gridY = gy;

    if (gx <= 0 || gy <= 0 || gx > 4096 || gy > 4096) {
        m_error = "Invalid grid size: " + std::to_string(gx) + "x" + std::to_string(gy);
        return false;
    }

    for (int x = 0; x < gx; ++x) {
        for (int y = 0; y < gy; ++y) {
            int32_t count = 0;
            std::memcpy(&count, data + off, 4);
            off += 4;
            if (count < 0 || static_cast<size_t>(count) > size) {
                m_error = "Invalid object count " + std::to_string(count) + " at cell [" + std::to_string(x) + "," +
                          std::to_string(y) + "]";
                return false;
            }
            for (int k = 0; k < count; ++k) {
                MPDSceneObject obj;
                obj.gridX = x;
                obj.gridY = y;
                std::memcpy(obj.matrix, data + off, 16 * 4);
                off += 16 * 4;
                int16_t pieceID = 0;
                std::memcpy(&pieceID, data + off, 2);
                off += 2;
                obj.pieceID = pieceID;
                int32_t rCount = 0;
                std::memcpy(&rCount, data + off, 4);
                off += 4;
                if (rCount > 0) {
                    if (off + static_cast<size_t>(rCount) > size) {
                        m_error = "Object script overrun at cell [" + std::to_string(x) + "," + std::to_string(y) +
                                  "] obj " + std::to_string(k);
                        return false;
                    }
                    obj.script = ReadVarString(data, size, off, static_cast<uint32_t>(rCount));
                }
                m_scene.objects.push_back(std::move(obj));
            }
        }
    }

    return true;
}

// ==========================================================================
// ParseFile — 从文件加载（自动检测 XOR 加密）
// ==========================================================================
bool MPDSceneParser::ParseFile(const std::string &filepath) {
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

    m_scene.filepath = filepath;

    // MPD 通常明文（头部 "MPD"），先试明文
    if (size >= 3 && std::memcmp(buffer.data(), "MPD", 3) == 0) {
        if (Parse(buffer.data(), buffer.size()))
            return true;
    }

    // 兜底：尝试 XOR 解密（某些 MOD 的 MPD 可能加密）
    XORCipher cipher(static_cast<uint32_t>(XORCipher::Version::PowerUpKit));
    cipher.DecryptBuffer(buffer.data(), buffer.size());
    if (Parse(buffer.data(), buffer.size()))
        return true;

    m_error = "Failed to parse .mpd file (plain + XOR 0x0B7E7759)";
    return false;
}

} // namespace AssetTool
