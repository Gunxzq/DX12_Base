#pragma once
#include "HandlePoolBase.h"
#include "Resource/Struct/ResourceHandle.h"
#include "Resource/Struct/ResourceTypes.h"
#include <vector>

namespace DX12Engine::Resource {

class GpuHandlePool : public HandlePoolBase<GpuResourceHandle, GpuResourceState, GpuResourceType> {

    friend struct TLSCache;

public:
    GpuResourceHandle AllocateSlot(GpuResourceType type, uint8_t poolId = 0,
                                   GpuResourceState initialState = GpuResourceState::Ready) override;
    void FreeSlot(GpuResourceHandle handle) override;
};

} // namespace DX12Engine::Resource