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
    uint32_t index : 22;      // 最大支持 4,194,304 个 GPU 资源
    uint32_t generation : 10; // 最大 1024 次复用

    static constexpr GpuResourceHandle Invalid() {
        return {0x3FFFFF, 0}; // Index 全1表示无效
    }

    bool IsValid() const { return index != 0x3FFFFF; }

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
