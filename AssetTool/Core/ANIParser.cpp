#include "ANIParser.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // 防止 windows.h 定义 min/max 宏污染 std::min/std::max
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace AssetTool {

namespace {

// 帧块常量（实测逆向，2026-07-31）
constexpr size_t HOD_DATA_SIZE = 9847; // 每帧 HOD 数据体长度（从 "HOD" 魔术起）
constexpr size_t TAIL_HEADER = 16;     // Tail 16B 头（段数/时间/速度/段1大小）

#ifdef _WIN32
/// 宽字符路径 → UTF-8（仅用于错误消息显示）
static std::string WideToUTF8Local(const std::wstring &wstr) {
    if (wstr.empty())
        return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &out[0], len, nullptr, nullptr);
    return out;
}
#endif

/// 是否名字字符（文件名主体：字母/数字/下划线/点）
inline bool IsNameChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '.'; }

/// 从 .hod 位置向前提取帧名（不含 .hod 扩展名）
std::string ExtractFrameBaseName(const uint8_t *data, size_t hodPos) {
    size_t s = hodPos;
    while (s > 0 && IsNameChar(data[s - 1]))
        s--;
    return std::string(reinterpret_cast<const char *>(data + s), hodPos - s);
}

/// 在 [start, limit) 区间内找 Tail 段起点（明文特征）
/// Tail 头 = 版本(1..10) + 时间 + 速度(可为 0) + 脚本大小，脚本以明文 SPT 开头
/// 返回 Tail 头位置；未找到返回 (size_t)-1
static size_t FindTailStart(const uint8_t *data, size_t start, size_t limit) {
    if (start + 16 > limit)
        return static_cast<size_t>(-1);
    for (size_t pos = start; pos + 16 <= limit; ++pos) {
        uint32_t ver = 0;
        std::memcpy(&ver, data + pos, 4);
        if (ver < 1 || ver > 10)
            continue;
        float sp = 0.0f;
        std::memcpy(&sp, data + pos + 8, 4);
        if (sp < 0.0f || sp > 10.0f)
            continue;
        uint32_t sz = 0;
        std::memcpy(&sz, data + pos + 12, 4);
        if (sz == 0 || sz > limit - pos - 16)
            continue;
        // 脚本开头应为 Shift-JIS 注释 ' 或 ASCII 命令
        size_t scriptPos = pos + 16;
        unsigned char c0 = data[scriptPos];
        if (c0 == 0x27 || (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) {
            // 脚本前 200B 内应有明文 IF(@ 特征
            for (size_t j = scriptPos; j + 4 <= pos + 16 + sz && j < scriptPos + 200; ++j) {
                if (std::memcmp(data + j, "IF(@", 4) == 0)
                    return pos;
            }
        }
    }
    return static_cast<size_t>(-1);
}

/// 组编号 → 输出目录名（01, 02, ...）
std::string GroupDirName(uint32_t index) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02u", index);
    return buf;
}

/// 帧序号 → 文件名前缀（001, 002, ...）
std::string FramePrefix(uint32_t n) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%03u", n);
    return buf;
}

/// 将 Shift-JIS 字节流转为可读文本（ASCII 可见，非 ASCII 显示为 ?）
std::string MakeReadableText(const uint8_t *data, size_t size) {
    std::string out;
    out.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = data[i];
        if (c >= 0x20 && c < 0x7F)
            out += static_cast<char>(c);
        else if (c == '\r' || c == '\n' || c == '\t')
            out += static_cast<char>(c);
        else
            out += '?';
    }
    return out;
}

} // namespace

// ==========================================================================
// 解析：标记扫描整个源文件
// ==========================================================================

bool ANIParser::ParseFile(const std::string &filepath) {
    m_groups.clear();
    m_error.clear();
    m_diagnostics.clear();

    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        m_error = "Cannot open file: " + filepath;
        return false;
    }
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char *>(buffer.data()), size);

    return ParseBuffer(buffer.data(), buffer.size());
}

bool ANIParser::ParseFileW(const std::wstring &filepath) {
    m_groups.clear();
    m_error.clear();
    m_diagnostics.clear();

    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        m_error = "Cannot open file (wide path): " + WideToUTF8Local(filepath);
        return false;
    }
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char *>(buffer.data()), size);

    return ParseBuffer(buffer.data(), buffer.size());
}

bool ANIParser::ParseBuffer(const uint8_t *data, size_t size) {
    if (size < 16) {
        m_error = "File too small";
        return false;
    }

    // 1) 扫描所有 ".hod" 文件名位置（块边界，含母版）
    std::vector<size_t> hodPositions;
    for (size_t i = 0; i + 4 <= size; ++i) {
        if (data[i] == '.' && data[i + 1] == 'h' && data[i + 2] == 'o' && data[i + 3] == 'd')
            hodPositions.push_back(i);
    }
    if (hodPositions.empty()) {
        m_error = "No .hod block boundaries found";
        return false;
    }

    // 1b) 变体检测：无 .hod 块标记的机体（如 KD-04_4 头部 AN2a.hod，帧间无 .hod 文件名）
    //     HD2 块数远多于 .hod 位置数 → 帧边界 = HD2 魔术本身，名字区 = [uint16 长度][Shift-JIS 名字]
    size_t hd2ScanCount = 0;
    for (size_t i = 0; i + 3 <= size; ++i) {
        if (std::memcmp(data + i, "HD2", 3) == 0)
            hd2ScanCount++;
    }
    bool variantNoHod = (hd2ScanCount >= 8 && hodPositions.size() * 4 < hd2ScanCount);

    // 2) 解析母版块（第一个 HD2 类型=0 / 第一个 HOD 块 = 母版，含完整骨架）
    //    不依赖 .hod 位置（母版块远离头部文件名区，如 PUK 母版 @0x103，头部 .hod @0x07 相距 252B）
    //    不依赖文件名，兼容 AN2Robo/AN2a/AN2robo/ANIsenkan 等变体
    size_t bi = 0;
    {
        bool masterIsHd2 = false;
        size_t masterMagic = static_cast<size_t>(-1);
        // 扫描第一个 HD2（类型=0 = 母版）
        for (size_t i = 0; i + 19 <= size; ++i) {
            if (std::memcmp(data + i, "HD2", 3) == 0) {
                uint32_t type = 0;
                std::memcpy(&type, data + i + 3, 4);
                if (type == 0) {
                    masterMagic = i;
                    masterIsHd2 = true;
                    break;
                }
            }
        }
        // 无 HD2 母版 → 扫描第一个 HOD 块（1.008 原版）
        if (masterMagic == static_cast<size_t>(-1)) {
            for (size_t i = 0; i + 15 <= size; ++i) {
                if (std::memcmp(data + i, "HOD", 3) == 0) {
                    masterMagic = i;
                    masterIsHd2 = false;
                    break;
                }
            }
        }
        if (masterMagic != static_cast<size_t>(-1))
            ParseMaster(data, size, masterMagic, masterIsHd2);

        // 标准模式（有 .hod 帧标记）：首个 .hod 即母版文件名（ANIRobo/AN2Robo/AN2a...）→ 帧定位跳过
        if (!variantNoHod && hodPositions.size() > 1) {
            std::string base = ExtractFrameBaseName(data, hodPositions[0]);
            // 含 "ANIa"（KD-08 变体 1.008 原版，2026-08-01 修复：此前被误当帧边界致首帧 0 字节）
            if (base == "ANIRobo" || base == "ANIrobo" || base == "AN2Robo" || base == "AN2robo" || base == "AN2a" ||
                base == "ANIa" || base == "ANIsenkan" || base == "AN2Hangar" || base == "AN2hangar" ||
                base == "ANIHangar") {
                bi = 1;
            }
        }
    }

    // 3) 每帧定位块数据起点（.hod 文件名 → 最近的 HOD/HD2 魔术）
    //    部件数/类型仅作校验与骨架信息（标记），不参与帧数据大小计算
    struct FrameLoc {
        size_t hodPos;      // .hod 文件名位置 / HD2 魔术位置（帧名标记）
        size_t hodStart;    // HOD/HD2 魔术位置（块数据起点）
        size_t nameLen;     // 名字区长度（帧名长度，用于下一帧名字起点标记）
        bool isHd2;         // HD2（PUK）vs HOD（1.008）
        uint32_t partCount; // 部件数（标记：骨架/校验）
    };
    std::vector<FrameLoc> frames;
    if (variantNoHod) {
        // 变体模式：无 .hod 块标记，帧边界 = HD2 魔术本身
        // 名字区 = [uint16 长度][Shift-JIS 名字]，紧邻 HD2 前
        frames.reserve(hd2ScanCount);
        for (size_t i = 0; i + 19 <= size; ++i) {
            if (std::memcmp(data + i, "HD2", 3) != 0)
                continue;
            uint32_t hd2Type = 0;
            std::memcpy(&hd2Type, data + i + 3, 4);
            if (hd2Type == 0)
                continue; // 母版（类型=0）跳过
            uint32_t partCount = 0;
            std::memcpy(&partCount, data + i + 7, 4);
            if (partCount == 0 || partCount > 255)
                continue;
            // 名字区总长：找 [uint16 len][len B 名字] 紧邻 HD2 前（len 字段在 HD2-2-len 处）
            size_t nameArea = 0;
            for (size_t o = 4; o <= 100 && o <= i; o += 2) {
                uint16_t len = 0;
                std::memcpy(&len, data + i - o, 2);
                if (len >= 2 && len <= 64 && o == 2 + len) {
                    // 校验名字区为 Shift-JIS 高位字节居多
                    size_t hi = 0;
                    for (size_t b = 0; b < len; ++b) {
                        if (data[i - len + b] >= 0x80)
                            hi++;
                    }
                    if (hi * 2 >= len) {
                        nameArea = o;
                        break;
                    }
                }
            }
            frames.push_back({i, i, nameArea, true, partCount});
        }
    } else {
        frames.reserve(hodPositions.size() - bi);
        for (size_t k = bi; k < hodPositions.size(); ++k) {
            size_t p = hodPositions[k];
            size_t found = static_cast<size_t>(-1);
            bool isHd2 = false;
            // 先试 .hod + 4（HD2），再试 .hod + 20（HOD）
            for (size_t delta : {size_t(4), size_t(20)}) {
                size_t cand = p + delta;
                if (cand + 4 <= size &&
                    (std::memcmp(data + cand, "HOD", 3) == 0 || std::memcmp(data + cand, "HD2", 3) == 0)) {
                    found = cand;
                    isHd2 = std::memcmp(data + cand, "HD2", 3) == 0;
                    break;
                }
            }
            if (found == static_cast<size_t>(-1)) {
                for (size_t j = p; j + 4 <= size && j < p + 300; ++j) {
                    if (std::memcmp(data + j, "HOD", 3) == 0 || std::memcmp(data + j, "HD2", 3) == 0) {
                        found = j;
                        isHd2 = std::memcmp(data + j, "HD2", 3) == 0;
                        break;
                    }
                }
            }
            if (found == static_cast<size_t>(-1))
                continue;
            // 部件数：HOD@+3 / HD2@+7（仅合理性校验）
            uint32_t partCount = 0;
            std::memcpy(&partCount, data + found + (isHd2 ? 7 : 3), 4);
            if (partCount == 0 || partCount > 255)
                continue;
            // HD2 母版（类型=0）跳过；帧块（类型=1）保留
            if (isHd2) {
                uint32_t hd2Type = 0;
                std::memcpy(&hd2Type, data + found + 3, 4);
                if (hd2Type == 0)
                    continue;
            }
            size_t nameLen = ExtractFrameBaseName(data, p).length();
            frames.push_back({p, found, nameLen, isHd2, partCount});
        }
    }
    if (frames.empty()) {
        m_error = "No animation frames located (no valid HOD/HD2 block after .hod names)";
        return false;
    }

    // 4) 分组 + Tail（标记法，无固定步长）：
    //    帧数据结束 = 下一帧名字起点（= 下一帧 .hod 位置 − 名字长度）
    //    组尾帧的 Tail 段用明文特征（IF(@ + 16B 头）在 [块数据, 名字起点) 区间内定位
    //    空 Tail 组（无明文特征，Tail 全零）用"部件数 × 每部件大小"推算帧数据结束兜底
    //    最后一组帧数据结束 = 文件尾 IKDATA（End.dat 魔数）前
    std::vector<ANIGroup> groups;
    ANIGroup cur;
    for (size_t k = 0; k < frames.size(); ++k) {
        // 帧数据结束候选 = 下一帧名字起点（标记）
        size_t dataEnd = size;
        if (k + 1 < frames.size())
            dataEnd = frames[k + 1].hodPos - frames[k + 1].nameLen;
        else {
            // 最后一帧：延伸到 IKDATA（End.dat 魔数）前
            for (size_t i = frames[k].hodStart; i + 6 <= size; ++i) {
                if (std::memcmp(data + i, "IKDATA", 6) == 0) {
                    dataEnd = i;
                    break;
                }
            }
        }

        // 在 [块数据+头部, 名字起点) 内找 Tail 明文特征（组尾帧才有）
        size_t headSize = frames[k].isHd2 ? 19 : 15; // HD2 头 19B / HOD 头 15B
        size_t tailStart = static_cast<size_t>(-1);
        if (frames[k].hodStart + headSize < dataEnd)
            tailStart = FindTailStart(data, frames[k].hodStart + headSize, dataEnd);

        // 空 Tail 兜底：无明文 Tail 时，用"部件数 × 每部件大小"推算帧数据结束
        // （部件数来自 HD2/HOD 头标记，每部件大小为确定结构，非写死固定步长）
        size_t calcEnd =
            frames[k].hodStart + (frames[k].isHd2 ? (static_cast<size_t>(frames[k].partCount) * 179 - 8 + 19)
                                                  : (7 + static_cast<size_t>(frames[k].partCount) * 328));

        // 帧数据结束：明文 Tail → Tail 起点；空 Tail（dataEnd > calcEnd）→ calcEnd；组内帧 → dataEnd
        size_t frameEnd = dataEnd;
        bool isGroupEnd = false;
        if (tailStart != static_cast<size_t>(-1)) {
            frameEnd = tailStart;
            isGroupEnd = true;
        } else if (dataEnd > calcEnd + 4) {
            frameEnd = calcEnd;
            isGroupEnd = true;
        }

        ANIFrame frame;
        frame.name = ExtractFrameBaseName(data, frames[k].hodPos) + ".hod";
        size_t n =
            (frameEnd > frames[k].hodStart) ? std::min(frameEnd - frames[k].hodStart, size - frames[k].hodStart) : 0;
        frame.data.assign(data + frames[k].hodStart, data + frames[k].hodStart + n);
        cur.frames.push_back(std::move(frame));

        // 组尾帧：解析 Tail 并收组
        if (isGroupEnd) {
            ANITail tail;
            bool isLast = (k + 1 >= frames.size());
            size_t tailLimit = dataEnd;
            if (tailStart == static_cast<size_t>(-1)) {
                // 空 Tail：Tail 段 = [calcEnd, dataEnd)（全零填充，仅保留头）
                tailStart = calcEnd;
            }
            if (ParseTail(data, size, tailStart, tailLimit, isLast, tail))
                cur.tail = std::move(tail);
            groups.push_back(std::move(cur));
            cur = ANIGroup();
        }
    }
    if (!cur.frames.empty()) {
        ANITail tail;
        size_t dataEnd = size;
        for (size_t i = frames.back().hodStart; i + 6 <= size; ++i) {
            if (std::memcmp(data + i, "IKDATA", 6) == 0) {
                dataEnd = i;
                break;
            }
        }
        if (ParseTail(data, size, dataEnd, size, true, tail))
            cur.tail = std::move(tail);
        groups.push_back(std::move(cur));
    }

    if (groups.empty()) {
        m_error = "No animation groups parsed";
        return false;
    }
    for (size_t i = 0; i < groups.size(); ++i)
        groups[i].index = static_cast<uint32_t>(i + 1);

    // 诊断信息（日志用）：部件数/帧块间距/组数/帧数
    {
        char buf[512];
        uint32_t partCount = 0;
        if (!frames.empty())
            std::memcpy(&partCount, data + frames[0].hodStart + (frames[0].isHd2 ? 7 : 3), 4);
        size_t minGap = 0, maxGap = 0;
        for (size_t k = 0; k + 1 < frames.size(); ++k) {
            size_t g = frames[k + 1].hodPos - frames[k].hodPos;
            if (minGap == 0 || g < minGap)
                minGap = g;
            if (g > maxGap)
                maxGap = g;
        }
        size_t totalFrames = 0;
        for (const auto &g : groups)
            totalFrames += g.frames.size();
        snprintf(buf, sizeof(buf), "部件数=%u .hod块=%zu 帧块间距min/max=%zu/%zu 组数=%zu 总帧数=%zu", partCount,
                 frames.size(), minGap, maxGap, groups.size(), totalFrames);
        m_diagnostics = buf;
    }

    m_groups = std::move(groups);
    return true;
}

// ==========================================================================
// Tail 解析：16B 头 + 明文 SPT 脚本
//   中间组：段结构推进（n>0）或直接跳零（n==0）→ 到 Note 起点
//   最后一组：延伸到文件尾 IKDATA（End.dat）前
// ==========================================================================

bool ANIParser::ParseMaster(const uint8_t *data, size_t size, size_t magicPos, bool isHd2) {
    m_master = ANIMaster();
    m_master.isHd2 = isHd2;

    // 部件数：HOD@+3 / HD2@+7
    uint32_t partCount = 0;
    std::memcpy(&partCount, data + magicPos + (isHd2 ? 7 : 3), 4);
    if (partCount == 0 || partCount > 255)
        return false;

    // 部件名起点：HOD 母版 0x0F / HD2 母版 0x13；每部件步长：HOD 328B / HD2 399B
    const size_t nameOffset = isHd2 ? 0x13 : 0x0F;
    const size_t entrySize = isHd2 ? 399 : 328;
    size_t pos = magicPos + nameOffset;

    m_master.bones.reserve(partCount);
    for (uint32_t i = 0; i < partCount; ++i) {
        if (pos + entrySize > size)
            break;

        ANIMasterBone bone;
        // 部件名（256B 名字区，\0 截断）
        const char *nameStart = reinterpret_cast<const char *>(data + pos);
        size_t nameLen = strnlen(nameStart, 256);
        bone.name.assign(nameStart, nameLen);

        // A/B 层级：每部件末尾 8B（A @ +entrySize-8, B @ +entrySize-4）
        std::memcpy(&bone.rawA, data + pos + entrySize - 8, 4);
        std::memcpy(&bone.rawB, data + pos + entrySize - 4, 4);

        m_master.bones.push_back(std::move(bone));
        pos += entrySize;
    }

    return !m_master.bones.empty();
}

bool ANIParser::ParseTail(const uint8_t *data, size_t size, size_t tailOffset, size_t tailLimit, bool isLastGroup,
                          ANITail &out) const {
    if (tailOffset + TAIL_HEADER > size)
        return false;

    // 16B 头：段数/版本(uint32) 时间值(uint32) 速度(float) 段1脚本大小(uint32)
    uint32_t n = 0, timeVal = 0, seg1Size = 0;
    float speed = 0.0f;
    std::memcpy(&n, data + tailOffset + 0, 4);
    std::memcpy(&timeVal, data + tailOffset + 4, 4);
    std::memcpy(&speed, data + tailOffset + 8, 4);
    std::memcpy(&seg1Size, data + tailOffset + 12, 4);

    out.version = n;
    out.time = timeVal;
    out.speed = speed;
    out.scriptSize = seg1Size;
    out.segments.clear();

    size_t tailEnd = tailOffset;
    if (isLastGroup) {
        // 最后一组：Tail 延伸到文件尾 IKDATA（End.dat 魔数）前
        size_t ik = size;
        for (size_t i = tailOffset; i + 6 <= size; ++i) {
            if (std::memcmp(data + i, "IKDATA", 6) == 0) {
                ik = i;
                break;
            }
        }
        tailEnd = ik;
    } else if (n > 0 && seg1Size > 0 && seg1Size < 4 * 1024 * 1024 &&
               tailOffset + TAIL_HEADER + seg1Size <= tailLimit) {
        // 段结构：段1 = 16B 头的时间/速度/大小；段2..n 各带 12B 段头（时间/速度/大小）
        size_t pos = tailOffset + TAIL_HEADER;
        for (uint32_t i = 1; i <= n; ++i) {
            uint32_t tSeg = 0, seg = 0;
            float spSeg = 0.0f;
            if (i == 1) {
                tSeg = timeVal;
                spSeg = speed;
                seg = seg1Size;
            } else {
                if (pos + 12 > tailLimit)
                    break;
                std::memcpy(&tSeg, data + pos + 0, 4);
                std::memcpy(&spSeg, data + pos + 4, 4);
                std::memcpy(&seg, data + pos + 8, 4);
                pos += 12;
            }
            if (seg == 0 || seg > 4 * 1024 * 1024 || pos + seg > tailLimit)
                break;
            ANITailSegment segOut;
            segOut.time = tSeg;
            segOut.speed = spSeg;
            segOut.script.assign(data + pos, data + pos + seg);
            segOut.scriptText = MakeReadableText(segOut.script.data(), segOut.script.size());
            out.segments.push_back(std::move(segOut));
            pos += seg;
        }
        // 跳零到 Note 起点（Tail 段尾零填充后为下一组动画名）
        while (pos < tailLimit && data[pos] == 0)
            pos++;
        tailEnd = pos;
    } else {
        // n==0：纯零填充 Tail（空脚本），跳零到 Note 起点
        size_t pos = tailOffset;
        while (pos < tailLimit && data[pos] == 0)
            pos++;
        tailEnd = pos;
    }

    if (tailEnd <= tailOffset || tailEnd > size)
        return false;
    size_t len = tailEnd - tailOffset;
    if (len > TAIL_HEADER) {
        out.script.assign(data + tailOffset + TAIL_HEADER, data + tailEnd);
        out.scriptText = MakeReadableText(out.script.data(), out.script.size());
    }
    return true;
}

// ==========================================================================
// 输出：按组写文件夹
//    {outDir}/01/001_robo_stand.hod ... + Tail.dat + Tail.txt
// ==========================================================================

bool ANIParser::WriteOutput(const std::string &outputDir) const {
    std::error_code ec;
    fs::create_directories(outputDir, ec);

    for (const ANIGroup &g : m_groups) {
        fs::path groupPath = fs::path(outputDir) / GroupDirName(g.index);
        fs::create_directories(groupPath, ec);

        // 顺序前缀 HOD 动画帧
        for (size_t i = 0; i < g.frames.size(); ++i) {
            const ANIFrame &frame = g.frames[i];
            std::string outName = FramePrefix(static_cast<uint32_t>(i + 1)) + "_" + frame.name;
            fs::path outPath = groupPath / outName;
            std::ofstream out(outPath, std::ios::binary);
            if (out)
                out.write(reinterpret_cast<const char *>(frame.data.data()),
                          static_cast<std::streamsize>(frame.data.size()));
        }

        // Tail.dat：16B 头 + 明文脚本（含尾零填充，与拆解产物一致）
        if (!g.tail.script.empty() || g.tail.scriptSize > 0) {
            fs::path tailPath = groupPath / "Tail.dat";
            std::ofstream out(tailPath, std::ios::binary);
            if (out) {
                uint8_t header[TAIL_HEADER];
                std::memcpy(header + 0, &g.tail.version, 4);
                std::memcpy(header + 4, &g.tail.time, 4);
                std::memcpy(header + 8, &g.tail.speed, 4);
                std::memcpy(header + 12, &g.tail.scriptSize, 4);
                out.write(reinterpret_cast<const char *>(header), TAIL_HEADER);
                out.write(reinterpret_cast<const char *>(g.tail.script.data()),
                          static_cast<std::streamsize>(g.tail.script.size()));
            }
        }

        // Tail.txt：社区版格式（---NNN--- / 时间,速度 / Begin/End / ===NNN===）
        // 脚本保留 Shift-JIS 原始字节（与社区拆解产物一致）
        {
            fs::path txtPath = groupPath / "Tail.txt";
            std::ofstream out(txtPath, std::ios::binary);
            if (!out)
                continue;

            for (size_t si = 0; si < g.tail.segments.size(); ++si) {
                const ANITailSegment &seg = g.tail.segments[si];

                // ---NNN---
                char segMark[32];
                snprintf(segMark, sizeof(segMark), "---%03zu---\r\n", si + 1);
                out << segMark;

                // 时间,速度（速度去前导 0 / 尾随 0：0.1 → .1, 0 → 0）
                char speedBuf[32];
                snprintf(speedBuf, sizeof(speedBuf), "%.5f", seg.speed);
                std::string speedStr = speedBuf;
                while (!speedStr.empty() && speedStr.back() == '0')
                    speedStr.pop_back();
                if (!speedStr.empty() && speedStr.back() == '.')
                    speedStr.pop_back();
                if (speedStr.rfind("0.", 0) == 0)
                    speedStr = speedStr.substr(1); // 0.xxx → .xxx
                out << seg.time << "," << speedStr << "\r\n";

                // #####Begin#####
                out << "#####Begin#####\r\n";

                // 脚本（Shift-JIS 原样字节）
                out.write(reinterpret_cast<const char *>(seg.script.data()),
                          static_cast<std::streamsize>(seg.script.size()));
                if (seg.script.empty() || seg.script.back() != '\n')
                    out << "\r\n";

                // #####End#####
                out << "#####End#####\r\n";
            }

            // ===NNN===（组索引 0-based，与社区版一致：组01→===000===, 组02→===001===）
            char tailMark[32];
            snprintf(tailMark, sizeof(tailMark), "===%03u===\r\n", g.index - 1);
            out << tailMark;
        }
    }
    return true;
}

} // namespace AssetTool
