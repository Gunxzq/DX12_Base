// DataSlotPool.h — SharedDataStore 专用的槽位池
#pragma once
#include "Common/HandlePoolBase.h"
#include <cstdint>
#include <vector>

namespace DX12Engine::Core {

// ============================================================================
// 数据槽位类型定义
// ============================================================================

// 数据槽位状态机：Empty → Loading → Ready → PendingRelease → (Reclaim)
enum class DataSlotState : uint8_t { Empty, Loading, Ready, Error, PendingRelease };

// 数据槽位类型
enum class DataSlotType : uint8_t { Unknown, Mesh, Texture, Audio, Shader, UploadBuffer, ReadbackBuffer };

// 数据槽位句柄：18位索引 + 10位世代号 + 4位池ID
struct DataSlotHandle {
    uint32_t index : 18;      // 最大支持 262,144 个槽位
    uint32_t generation : 10; // 最大 1024 次复用
    uint32_t poolId : 4;      // 池ID (0-15)

    static constexpr DataSlotHandle Invalid() {
        return {0x3FFFF, 0, 0}; // Index 全1表示无效
    }

    bool IsValid() const { return index != 0x3FFFF; }

    // 用于在 Arena 中存储
    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }

    static DataSlotHandle FromUint32(uint32_t val) {
        DataSlotHandle h;
        *reinterpret_cast<uint32_t *>(&h) = val;
        return h;
    }
};

// ============================================================================
// DataSlotPool — 基于 HandlePoolBase 的槽位池（含 TLS 缓存）
// ============================================================================

class DataSlotPool : public HandlePoolBase<DataSlotHandle, DataSlotState, DataSlotType> {

    friend struct TLSCache;

public:
    DataSlotHandle AllocateSlot(DataSlotType type, uint8_t poolId = 0,
                                DataSlotState initialState = DataSlotState::Loading) override;

    void FreeSlot(DataSlotHandle handle) override;
};

} // namespace DX12Engine::Core
