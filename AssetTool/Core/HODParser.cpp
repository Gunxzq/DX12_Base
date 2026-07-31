#include "HODParser.h"
#include "XORCipher.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include <DirectXMath.h>

namespace fs = std::filesystem;

namespace AssetTool {

// ==========================================================================
// 辅助：过滤非 UTF-8 字符（nlohmann_json 序列化时遇到非法编码会崩溃）
// ==========================================================================

static std::string SanitizeUTF8(const std::string &input) {
    std::string result;
    result.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c < 0x80) {
            // ASCII: 保留
            if (c >= 32 || c == '\t' || c == '\n')
                result += c;
            else
                result += '?';
            i++;
        } else if (c < 0xC0) {
            // 意外的后续字节 → 替换为 ?
            result += '?';
            i++;
        } else if (c < 0xE0) {
            // 2 字节 UTF-8
            if (i + 1 >= input.size()) { result += '?'; i++; continue; }
            unsigned char c2 = static_cast<unsigned char>(input[i + 1]);
            if ((c2 & 0xC0) == 0x80) {
                result += c; result += c2; i += 2;
            } else {
                result += '?'; i++;
            }
        } else if (c < 0xF0) {
            // 3 字节 UTF-8
            if (i + 2 >= input.size()) { result += '?'; i++; continue; }
            unsigned char c2 = static_cast<unsigned char>(input[i + 1]);
            unsigned char c3 = static_cast<unsigned char>(input[i + 2]);
            if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
                result += c; result += c2; result += c3; i += 3;
            } else {
                result += '?'; i++;
            }
        } else if (c < 0xF8) {
            // 4 字节 UTF-8
            if (i + 3 >= input.size()) { result += '?'; i++; continue; }
            unsigned char c2 = static_cast<unsigned char>(input[i + 1]);
            unsigned char c3 = static_cast<unsigned char>(input[i + 2]);
            unsigned char c4 = static_cast<unsigned char>(input[i + 3]);
            if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80 && (c4 & 0xC0) == 0x80) {
                result += c; result += c2; result += c3; result += c4; i += 4;
            } else {
                result += '?'; i++;
            }
        } else {
            result += '?'; i++;
        }
    }
    return result;
}

// ==========================================================================
// HODParser
// ==========================================================================
//
// HOD 二进制结构（所有 entry 统一大小）：
//   Header: 0x00-0x0E (15 字节)
//   ──
//   每个 entry:
//     [64B] 文件名 + null + padding
//     [64B] 4×4 矩阵 (16 floats)
//     [4B]  A = 节点索引
//     [4B]  B = 父节点索引（0 = 根节点）
//     total: 136 字节
// ==========================================================================

bool HODParser::Parse(const uint8_t *data, size_t size) {
    m_result = HODData();
    m_error.clear();

    const uint8_t *pos = data;
    const uint8_t *end = data + size;

    // 验证魔术头
    if (size < 4 || std::memcmp(pos, "HOD", 3) != 0) {
        m_error = "Not a valid .hod file (missing 'HOD' magic). "
                  "File may need XOR decryption with a different key, "
                  "or the file is already decrypted and corrupted.";
        return false;
    }
    pos += 3;

    // 名字固定从 offset 0x0F 开始（已验证名字和矩阵正确）
    pos = data + 0x0F;

    // 所有 entry 统一大小：256B name + 64B matrix + 4B A + 4B B
    constexpr size_t ENTRY_NAME    = 256;
    constexpr size_t ENTRY_MATRIX  = 64; // 16 floats
    constexpr size_t ENTRY_A       = 4;
    constexpr size_t ENTRY_B       = 4;
    constexpr size_t ENTRY_SIZE    = ENTRY_NAME + ENTRY_MATRIX + ENTRY_A + ENTRY_B; // = 328

    while (pos + ENTRY_SIZE <= end) {
        HODBone bone;

        // 文件名: 256 字节
        {
            const char *nameStart = reinterpret_cast<const char *>(pos);
            size_t nameLen = strnlen(nameStart, ENTRY_NAME);
            bone.name.assign(nameStart, nameLen);
            pos += ENTRY_NAME;
        }

        // 矩阵: 16 floats
        {
            float matf[16];
            for (int i = 0; i < 16; ++i) {
                if (pos + 4 > end) break;
                std::memcpy(&matf[i], pos, 4);
                pos += 4;
            }
            for (int i = 0; i < 16; ++i)
                bone.transform[i] = static_cast<double>(matf[i]);
        }

        // A = 节点索引（二进制 1-based）
        if (pos + ENTRY_A > end) break;
        uint32_t rawA = 0;
        std::memcpy(&rawA, pos, ENTRY_A);
        pos += ENTRY_A;
        bone.rawA = rawA;

        // B 值
        {
            uint32_t rawB = 0;
            if (pos + ENTRY_B > end) break;
            std::memcpy(&rawB, pos, ENTRY_B);
            pos += ENTRY_B;
            bone.rawB = rawB;
        }

        m_result.bones.push_back(bone);
    }

    // 构建父子层级关系
    m_result.BuildHierarchy();
    m_result.DecomposeAll();

    if (m_result.bones.empty()) {
        m_error = "No bones parsed from .hod file";
        return false;
    }

    return true;

fail:
    m_error = "Unexpected end of data in first .hod entry";
    return false;
}

// ==========================================================================
// ParseFile — 从文件加载（自动检测 XOR 加密）
// ==========================================================================

bool HODParser::ParseFile(const std::string &filepath, uint32_t decryptKey) {
    // 读取文件
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

    // 保存原始数据备用（后面多 key 尝试需要）
    std::vector<uint8_t> original = buffer;

    // 先尝试直接解析（文件可能是明文）
    if (size >= 4 && std::memcmp(buffer.data(), "HOD", 3) == 0) {
        // 明文头部正确，不尝试 XOR 解密——若解析失败就是文件结构问题
        return Parse(buffer.data(), buffer.size());
    }

    // 非明文头，尝试 XOR 解密后解析
    {
        XORCipher cipher(decryptKey);
        cipher.DecryptBuffer(buffer.data(), buffer.size());
        if (Parse(buffer.data(), buffer.size())) return true;
    }

    // 仍失败，尝试其他已知 MOD key
    uint32_t altKeys[] = {
        0x95127634, // SeedMod
        0x19870430, // MSV_MOD
        0xAC510B91, // SeedMod 2.0.5
        0x13322366, // United Mod
        0xEF452301, // Raid Mod
        0x33333323, // The Epic of War
        0x33322166, // UN Evo
    };
    for (uint32_t altKey : altKeys) {
        if (altKey == decryptKey) continue;
        // 用原始数据（未改动的副本）尝试其他 key
        std::vector<uint8_t> altBuf = original;
        XORCipher altCipher(altKey);
        altCipher.DecryptBuffer(altBuf.data(), altBuf.size());
        if (Parse(altBuf.data(), altBuf.size())) {
            return true;
        }
    }

    // 所有方式都失败，保留最后一个错误信息
    return false;
}

// ==========================================================================
// HODData 方法
// ==========================================================================

void HODData::BuildHierarchy() {
    if (bones.empty()) return;

    // A/B 含义（来自社区文档）：
    //   A = 部件等级（数字越小等级越高），B = 子部件数量
    // 文件按深度优先顺序存储，每个 entry 末尾的 A/B 属于下一个 entry
    // bone[0] (root) 使用隐式值 A=0 (根节点等级), B 忽略
    //
    // 构建算法：用栈维护当前层级路径
    //   - 对于 bone[i] (i>=1)：其等级 = bone[i-1].rawA
    //   - 在栈上回溯寻找等级 == 当前等级-1 的父节点

    // 清空原有层级
    for (auto &bone : bones) {
        bone.parentIndex = -1;
        bone.children.clear();
    }

    bones[0].parentIndex = -1; // root 无父

    // 栈：(bone_index, level)
    std::vector<std::pair<uint32_t, uint32_t>> levelStack;
    levelStack.push_back({0, 0}); // root 等级 0

    for (size_t i = 1; i < bones.size(); ++i) {
        uint32_t currentLevel = bones[i - 1].rawA; // 前移：前一 entry 的 rawA = 此 entry 的等级

        // 回溯栈顶，找到等级 == currentLevel - 1 的父节点
        while (!levelStack.empty() && levelStack.back().second >= currentLevel) {
            levelStack.pop_back();
        }

        if (!levelStack.empty() && levelStack.back().second == currentLevel - 1) {
            uint32_t parentIdx = levelStack.back().first;
            bones[i].parentIndex = static_cast<int>(parentIdx);
            bones[parentIdx].children.push_back(static_cast<uint32_t>(i));
        } else {
            // 未找到匹配父节点，回退到根
            bones[0].children.push_back(static_cast<uint32_t>(i));
        }

        levelStack.push_back({static_cast<uint32_t>(i), currentLevel});
    }
}

// ==========================================================================
// 骨骼矩阵 → TRS 分解
// ==========================================================================

void HODBone::DecomposeTRS() {
    using namespace DirectX;

    XMMATRIX mat = GetMatrix();

    // XMMatrixDecompose 需要列主序矩阵，
    // GetMatrix() 返回行主序，需要转置
    mat = XMMatrixTranspose(mat);

    XMVECTOR scaleVec, rotVec, transVec;
    if (XMMatrixDecompose(&scaleVec, &rotVec, &transVec, mat)) {
        // 位置
        position[0] = XMVectorGetX(transVec);
        position[1] = XMVectorGetY(transVec);
        position[2] = XMVectorGetZ(transVec);

        // 旋转（四元数）
        rotation[0] = XMVectorGetX(rotVec);
        rotation[1] = XMVectorGetY(rotVec);
        rotation[2] = XMVectorGetZ(rotVec);
        rotation[3] = XMVectorGetW(rotVec);

        // 缩放
        scale[0] = XMVectorGetX(scaleVec);
        scale[1] = XMVectorGetY(scaleVec);
        scale[2] = XMVectorGetZ(scaleVec);
    } else {
        // 分解失败，保持默认值
        position[0] = static_cast<float>(transform[12]);
        position[1] = static_cast<float>(transform[13]);
        position[2] = static_cast<float>(transform[14]);
    }
}

void HODData::DecomposeAll() {
    for (auto &bone : bones) {
        bone.DecomposeTRS();
    }
}

std::string HODData::ToJSON() const {
    nlohmann::json j;

    j["version"] = 1;
    j["name"] = SanitizeUTF8(fs::path(filepath).stem().string());
    j["boneCount"] = BoneCount();

    auto &jBones = j["bones"] = nlohmann::json::array();
    for (size_t i = 0; i < bones.size(); ++i) {
        const auto &bone = bones[i];
        nlohmann::json jBone;

        jBone["index"] = i;
        jBone["name"] = SanitizeUTF8(bone.name);
        jBone["parent"] = bone.parentIndex;

        // TRS
        auto &jPos = jBone["position"] = nlohmann::json::array();
        jPos.push_back(bone.position[0]);
        jPos.push_back(bone.position[1]);
        jPos.push_back(bone.position[2]);

        auto &jRot = jBone["rotation"] = nlohmann::json::array();
        jRot.push_back(bone.rotation[0]);
        jRot.push_back(bone.rotation[1]);
        jRot.push_back(bone.rotation[2]);
        jRot.push_back(bone.rotation[3]);

        auto &jScl = jBone["scale"] = nlohmann::json::array();
        jScl.push_back(bone.scale[0]);
        jScl.push_back(bone.scale[1]);
        jScl.push_back(bone.scale[2]);

        // 原始 4×4 矩阵（参考）
        auto &jMat = jBone["matrix"] = nlohmann::json::array();
        for (int r = 0; r < 4; ++r) {
            auto row = nlohmann::json::array();
            for (int c = 0; c < 4; ++c) {
                row.push_back(bone.transform[r * 4 + c]);
            }
            jMat.push_back(row);
        }

        // 子节点索引
        if (!bone.children.empty()) {
            auto &jChildren = jBone["children"] = nlohmann::json::array();
            for (uint32_t child : bone.children)
                jChildren.push_back(static_cast<int>(child));
        }

        jBones.push_back(jBone);
    }

    return j.dump(2);
}

std::string HODData::ToText() const {
    std::ostringstream oss;

    oss << "HOD File Information\n";
    oss << "====================\n\n";
    oss << "File: " << SanitizeUTF8(filepath) << "\n";
    oss << "Number of Bones: " << BoneCount() << "\n\n";

    for (size_t i = 0; i < bones.size(); ++i) {
        const auto &bone = bones[i];

        oss << "--- Bone[" << i << "] ---\n";

        // A/B forward-shift display
        uint32_t dA = (i == 0) ? 0 : bones[i - 1].rawA;
        uint32_t dB = (i == 0) ? 1 : bones[i - 1].rawB;
        oss << "A:" << dA << "  B:" << dB << "\n";

        oss << "Name: " << SanitizeUTF8(bone.name) << "\n";
        oss << "Parent: " << bone.parentIndex << "\n";

        // TRS
        oss << "Position: ("
            << bone.position[0] << ", " << bone.position[1] << ", " << bone.position[2] << ")\n";
        oss << "Rotation: ("
            << bone.rotation[0] << ", " << bone.rotation[1] << ", "
            << bone.rotation[2] << ", " << bone.rotation[3] << ")\n";
        oss << "Scale: ("
            << bone.scale[0] << ", " << bone.scale[1] << ", " << bone.scale[2] << ")\n";

        // Matrix
        oss << "Matrix:\n";
        for (int r = 0; r < 4; ++r) {
            oss << "  ";
            for (int c = 0; c < 4; ++c) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%10.6f", bone.transform[r * 4 + c]);
                oss << buf;
                if (c < 3) oss << " ";
            }
            oss << "\n";
        }

        // Children
        if (!bone.children.empty()) {
            oss << "Children: ";
            for (size_t ci = 0; ci < bone.children.size(); ++ci) {
                uint32_t childIdx = bone.children[ci];
                if (childIdx < bones.size())
                    oss << "[" << childIdx << "]" << SanitizeUTF8(bones[childIdx].name);
                else
                    oss << "[" << childIdx << "]?";
                if (ci + 1 < bone.children.size()) oss << ", ";
            }
            oss << "\n";
        }

        oss << "\n";
    }

    return oss.str();
}

bool HODData::WriteJSON(const std::string &outputPath) const {
    std::ofstream ofs(outputPath);
    if (!ofs) return false;
    ofs << ToJSON();
    return true;
}

bool HODData::WriteText(const std::string &outputPath) const {
    std::ofstream ofs(outputPath);
    if (!ofs) return false;
    ofs << ToText();
    return true;
}

} // namespace AssetTool
