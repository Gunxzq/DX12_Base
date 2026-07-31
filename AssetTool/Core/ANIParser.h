#pragma once
// ========================================================================
// ANIParser — UKW Script.ani 动画文件解析器
//
// 源文件结构（2026-07-31 实测逆向）：
//   [文件名头 "ANIRobo.hod\0" + padding]
//   [母版块 HOD]（冗余：每帧 HOD 自带完整骨骼，可跳过）
//   [组: N×HOD 帧块][Tail 明文状态机] × 65 组
//
// 块边界 = ".hod" 文件名后缀（348 个，与社区拆解产物逐一对齐）
// 每个 HOD 块 = 文件名区（名字\0 + CD 填充，20B 对齐）+ HOD 数据（恒 9847B）
// Tail = 16B 头（版本/时间值/速度值/脚本大小）+ 明文 SPT 脚本（Shift-JIS）
//
// 解析策略：标记扫描（非固定步长）
//   扫 "HOD" → 连续 HOD 块 = 一组动画帧序列
//   遇明文 Tail（IF(/ENDIF; 特征）→ 该组结束
//   Tail 之后再次出现 "HOD" → 下一组开始
//
// 输出：按组输出文件夹，每组含顺序前缀的 HOD 动画帧 + Tail 信息
// ========================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace AssetTool {

/// 一个动画帧（HOD 数据）
struct ANIFrame {
    std::string name;              // 帧名（源文件内嵌名，如 "robo_stand.hod"）
    std::vector<uint8_t> data;     // HOD 原始数据（9847B，从 "HOD" 魔术起）
};

/// Tail 中的一个段落（段头时间/速度 + 脚本）
struct ANITailSegment {
    uint32_t time = 0;              // 时间值（段头字段）
    float speed = 0.0f;             // 速度值（段头字段）
    std::vector<uint8_t> script;    // 脚本原始字节（Shift-JIS）
    std::string scriptText;         // 可读文本
};

/// Tail 状态机（16B 头 + 明文 SPT 脚本）
struct ANITail {
    uint32_t version    = 1;       // 版本（实测恒 1）
    uint32_t time       = 0;       // 时间值（段落时长）
    float    speed      = 0.0f;    // 速度值（实测 0.1f）
    uint32_t scriptSize = 0;       // 脚本数据大小
    std::vector<uint8_t> script;   // 明文 SPT 脚本原始字节（Shift-JIS，段1..n 拼接）
    std::string scriptText;        // Shift-JIS → UTF-8 解码文本
    std::vector<ANITailSegment> segments;  // 段落列表（段1 用 16B 头，段2..n 各带 12B 段头）
};

/// 一个动画组（一组连续 HOD 帧 + Tail 状态机）
struct ANIGroup {
    uint32_t index = 0;            // 组序号（1-based，对应拆解产物文件夹编号）
    std::vector<ANIFrame> frames;  // 动画帧（HOD 数据，源文件顺序）
    ANITail tail;                  // 状态机脚本
};

/// ANI 母版骨架部件（部件名 + A/B 层级，矩阵由同目录 Robo.hod 提供）
struct ANIMasterBone {
    std::string name;              // 部件名（如 "Body_d.x"，PUK 母版 256B 名字区）
    uint32_t rawA = 0;             // A 原始值（二进制 1-based）
    uint32_t rawB = 0;             // B 原始值（二进制 1-based）
};

/// ANI 母版骨架（HD2 类型=0 / HOD 首块）
struct ANIMaster {
    std::vector<ANIMasterBone> bones;  // 部件列表（源文件顺序，root 在 index 0）
    bool isHd2 = false;                // PUK HD2 母版（399B/部件）vs 1.008 HOD 母版（328B/部件）

    uint32_t BoneCount() const { return static_cast<uint32_t>(bones.size()); }
};

class ANIParser {
public:
    ANIParser() = default;

    /// 解析源文件 Script.ani
    bool ParseFile(const std::string &filepath);

    /// 解析源文件（宽字符路径，避免中文目录 UTF-8 窄路径打开失败）
    bool ParseFileW(const std::wstring &filepath);

    /// 获取解析结果
    const std::vector<ANIGroup> &GetGroups() const { return m_groups; }

    /// 获取母版骨架（部件名 + A/B 层级；矩阵由同目录 Robo.hod 提供）
    const ANIMaster &GetMaster() const { return m_master; }

    /// 获取解析错误信息
    const std::string &GetError() const { return m_error; }

    /// 获取解析诊断信息（部件数/帧块间距/组数等，用于日志）
    const std::string &GetDiagnostics() const { return m_diagnostics; }

    /// 按组输出到目录：{outDir}/01/001_xxx.hod ... + Tail.dat + Tail.txt
    bool WriteOutput(const std::string &outputDir) const;

private:
    /// 核心解析：标记扫描整个缓冲区
    bool ParseBuffer(const uint8_t *data, size_t size);

    /// 从源文件定位 Tail 段（组尾帧 HOD 数据 +9847B 处），解析 16B 头 + 脚本
    /// tailLimit: 本组结束边界（下一帧 .hod 位置或文件尾）
    /// isLastGroup: 最后一组 Tail 延伸到文件尾 IKDATA（End.dat）前
    bool ParseTail(const uint8_t *data, size_t size, size_t tailOffset, size_t tailLimit,
                   bool isLastGroup, ANITail &out) const;

    /// 解析母版块（首个 .hod 后的 HOD/HD2 块）：部件名 + A/B 层级
    /// 1.008 HOD 母版：部件名区 0x0F 起，每部件 328B（256名+64矩阵+8AB）
    /// PUK HD2 母版：部件名区 0x13 起，每部件 399B（256名+135数据+8AB）
    bool ParseMaster(const uint8_t *data, size_t size, size_t hodPos, bool isHd2);

    std::vector<ANIGroup> m_groups;
    ANIMaster m_master;           // 母版骨架（部件名 + A/B）
    std::string m_error;
    std::string m_diagnostics; // 解析诊断信息（日志用）
};

} // namespace AssetTool
