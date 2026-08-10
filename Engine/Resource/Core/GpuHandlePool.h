// GpuHandlePool.h — GPU 句柄池
#pragma once
#include "Common/HandlePoolBase.h"
#include <cstdint>
#include <vector>

namespace DX12Engine::Resource {

// ============================================================================
// GPU 资源类型定义
// ============================================================================

// GPU 端资源状态
enum class GpuResourceState : uint8_t { Empty, Ready, PendingRelease };

// GPU 端资源句柄：22位索引 + 10位世代号
struct GpuResourceHandle {
    // 默认成员初始化器 = 全 1（0xFFFFFFFF）无效值：默认构造的句柄天然无效，
    // 杜绝"位域零初始化 {index=0,gen=0} 被 IsValid 误判为有效"（InstanceCullingBuffer m_cullParamsUp 分配失效根因）
    uint32_t index : 22 = 0x3FFFFF;   // 最大支持 4,194,304 个 GPU 资源（默认全 1）
    uint32_t generation : 10 = 0x3FF; // 最大 1024 次复用（默认全 1）

    static constexpr GpuResourceHandle Invalid() {
        return {0x3FFFFF, 0x3FF}; // 整个 32 位 = 0xFFFFFFFF（对齐 SamplerHandle slot=UINT32_MAX 约定）
    }

    bool IsValid() const { return static_cast<uint32_t>(*this) != 0xFFFFFFFF; }

    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }

    static GpuResourceHandle FromUint32(uint32_t val) {
        GpuResourceHandle h;
        *reinterpret_cast<uint32_t *>(&h) = val;
        return h;
    }
};

// ============================================================================
// GpuHandlePool — 基于 HandlePoolBase 的 GPU 句柄池（含 TLS 缓存）
// ============================================================================

class GpuHandlePool : public HandlePoolBase<GpuResourceHandle, GpuResourceState> {

    friend struct TLSCache;

public:
    GpuResourceHandle AllocateSlot(uint8_t poolId = 0,
                                   GpuResourceState initialState = GpuResourceState::Ready) override;
    void FreeSlot(GpuResourceHandle handle) override;
};

} // namespace DX12Engine::Resource
