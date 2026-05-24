#pragma once
#include "HandlePoolBase.h"
#include "Resource/Struct/ResourceHandle.h"
#include "Resource/Struct/ResourceTypes.h"
#include <vector>

namespace DX12Engine::Resource {

class CpuHandlePool : public HandlePoolBase<CpuResourceHandle, CpuResourceState, CpuResourceType> {

    friend struct TLSCache;

public:
    CpuResourceHandle AllocateSlot(CpuResourceType type, uint8_t poolId = 0,
                                   CpuResourceState initialState = CpuResourceState::Loading) override;

    void FreeSlot(CpuResourceHandle handle) override;
};

} // namespace DX12Engine::Resource