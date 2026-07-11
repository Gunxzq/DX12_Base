#pragma once
// ========================================================================
// HODParser — UKW .hod 机体组装文件解析器
//
// .hod 格式（XOR 解密后）：
//   Entry 统一 328 字节：256B name + 64B matrix(float) + 4B A + 4B B
//   A/B 前移机制：每个 entry 末尾的 A/B 属于下一个 entry
//   第一个 entry 隐式 A=0, B=1
//   矩阵为 float（非 double），索引 1-based
//
// 输出：JSON 格式的现代骨架树（骨骼名/父子关系/TRS 变换）
// ========================================================================

#include <cstdint>
#include <string>
#include <vector>
#include <DirectXMath.h>

namespace AssetTool {

/// HOD 中的一个部件（骨骼）节点 — 现代结构
struct HODBone {
    std::string name;                    // 部件名，如 "Body_d.x"
    
    // 原始数据
    double transform[16];                // 4×4 矩阵（行主序，double 精度）
    uint32_t rawA = 0;                   // A 原始值（二进制 1-based）
    uint32_t rawB = 0;                   // B 原始值（二进制 1-based）

    // 派生数据
    int parentIndex = -1;                // 父骨骼索引（-1 = 根）
    std::vector<uint32_t> children;      // 子骨骼索引列表

    // TRS 分解结果
    float position[3]  = {0, 0, 0};
    float rotation[4]  = {0, 0, 0, 1};  // 四元数 [x, y, z, w]
    float scale[3]     = {1, 1, 1};

    /// 从 4×4 矩阵分解出 TRS
    void DecomposeTRS();

    DirectX::XMMATRIX GetMatrix() const {
        return DirectX::XMMatrixSet(
            static_cast<float>(transform[0]), static_cast<float>(transform[1]),
            static_cast<float>(transform[2]), static_cast<float>(transform[3]),
            static_cast<float>(transform[4]), static_cast<float>(transform[5]),
            static_cast<float>(transform[6]), static_cast<float>(transform[7]),
            static_cast<float>(transform[8]), static_cast<float>(transform[9]),
            static_cast<float>(transform[10]), static_cast<float>(transform[11]),
            static_cast<float>(transform[12]), static_cast<float>(transform[13]),
            static_cast<float>(transform[14]), static_cast<float>(transform[15])
        );
    }
};

/// HOD 解析结果：完整骨架树
struct HODData {
    std::string filepath;                // 原始 .hod 文件路径
    uint32_t flags = 0;                  // 原始标志位
    std::vector<HODBone> bones;          // 所有部件（DFS 顺序，根在 index 0）

    uint32_t BoneCount() const { return static_cast<uint32_t>(bones.size()); }

    /// 构建父子关系（基于 A/B 编码）
    void BuildHierarchy();

    /// 分解所有骨骼矩阵为 TRS
    void DecomposeAll();

    /// 输出为现代 JSON 骨架格式
    std::string ToJSON() const;

    /// 输出为可读文本格式
    std::string ToText() const;

    /// 写入 JSON 文件
    bool WriteJSON(const std::string &outputPath) const;

    /// 写入文本文件
    bool WriteText(const std::string &outputPath) const;
};

class HODParser {
public:
    HODParser() = default;

    /// 解析已解密的 .hod 数据
    bool Parse(const uint8_t *data, size_t size);

    /// 从文件加载并解析（自动解密）
    bool ParseFile(const std::string &filepath, uint32_t decryptKey = 0x0B7E7759);

    /// 获取解析结果
    const HODData &GetResult() const { return m_result; }

    /// 获取解析错误信息
    const std::string &GetError() const { return m_error; }

private:
    HODData m_result;
    std::string m_error;
};

} // namespace AssetTool
