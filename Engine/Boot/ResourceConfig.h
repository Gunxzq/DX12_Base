#pragma once

#include "Common/Common.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace DX12Engine {
namespace Boot {

// ========================================================================
// 内存池策略枚举
// ========================================================================

enum class MemoryStrategy : int { Linear = 0, FixedSizeBlock = 1, RingBuffer = 2 };

// 辅助宏：用于 nlohmann json 序列化 enum class 为字符串
NLOHMANN_JSON_SERIALIZE_ENUM(MemoryStrategy, {{MemoryStrategy::Linear, "Linear"},
                                              {MemoryStrategy::FixedSizeBlock, "FixedSizeBlock"},
                                              {MemoryStrategy::RingBuffer, "RingBuffer"}})

// ========================================================================
// 单个内存池配置结构体
// ========================================================================

struct MemoryPoolConfig {
    uint32_t HandleTag = 0; // 对应 JSON 中的 HandleTag，用于路由表索引
    std::string Name;       // 调试名称
    MemoryStrategy Strategy = MemoryStrategy::Linear;

    // 通用字段
    uint32_t MaxHandles = 0; // 该池最大支持的句柄数量

    // Linear 策略特有
    size_t SizeMB = 0;     // 线性池总大小 (MB)
    size_t Alignment = 16; // 对齐字节数

    // FixedSizeBlock 策略特有
    size_t BlockSize = 0; // 固定块大小
    uint32_t Count = 0;   // 块数量

    // RingBuffer 策略特有
    // SizeMB 也可用于 RingBuffer 表示环形缓冲区总大小

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(MemoryPoolConfig, HandleTag, Name, Strategy, MaxHandles, SizeMB, Alignment,
                                   BlockSize, Count)
};

// ========================================================================
// 句柄池配置结构体
// ========================================================================

struct HandlePoolConfig {
    uint32_t MaxTotalHandles = 262144;
    uint32_t InitialFreeListReserve = 262144;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(HandlePoolConfig, MaxTotalHandles, InitialFreeListReserve)
};

// ========================================================================
// 顶层资源系统配置结构体
// ========================================================================

struct ResourceSystemConfig {
    HandlePoolConfig HandlePoolConfig;
    std::vector<MemoryPoolConfig> MemoryPools;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ResourceSystemConfig, HandlePoolConfig, MemoryPools)
};

} // namespace Boot
} // namespace DX12Engine