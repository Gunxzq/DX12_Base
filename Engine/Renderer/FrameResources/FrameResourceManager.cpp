#include "FrameResourceManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"

#include "Common/Common.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

FrameResourceManager::~FrameResourceManager() { Shutdown(); }

void FrameResourceManager::CreatePassCB(ID3D12Device *device) {
    UINT cbSize = (sizeof(PassConstants) + 255) & ~255; // 256 字节对齐

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

    HRESULT hr =
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                        nullptr, IID_PPV_ARGS(&m_passCBResource));

    if (FAILED(hr)) {
        return;
    }

    hr = m_passCBResource->Map(0, nullptr, &m_passCBMapped);
    if (FAILED(hr)) {
        m_passCBResource.Reset();
        return;
    }
    m_passCBResource->SetName(L"PassCB");

    m_passCBAddress = m_passCBResource->GetGPUVirtualAddress();
}

void FrameResourceManager::Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;

    // 创建 PassCB
    CreatePassCB(device);

    // 初始化每帧的环形缓冲区（预分配 16MB 每类型）
    const uint32_t DEFAULT_BUFFER_SIZE = 16 * 1024 * 1024; // 16MB

    m_objectCB.Initialize(device, DEFAULT_BUFFER_SIZE, L"FrameResource_ObjectCB");
    m_skinning.Initialize(device, DEFAULT_BUFFER_SIZE, L"FrameResource_Skinning");
    m_instance.Initialize(device, DEFAULT_BUFFER_SIZE, L"FrameResource_Instance");
    m_waterCB.Initialize(device, DEFAULT_BUFFER_SIZE, L"FrameResource_WaterCB");

    m_passConstants = {};
    UpdatePassConstants();

    m_initialized = true;
}

void FrameResourceManager::Shutdown() {
    if (!m_initialized) {
        return;
    }

    m_objectCB.Shutdown();
    m_skinning.Shutdown();
    m_instance.Shutdown();
    m_waterCB.Shutdown();

    if (m_passCBResource) {
        if (m_passCBMapped) {
            m_passCBResource->Unmap(0, nullptr);
            m_passCBMapped = nullptr;
        }
        m_passCBResource.Reset();
    }

    m_device = nullptr;
    m_descriptorHeaps = nullptr;
    m_passCBAddress = 0;
    m_initialized = false;
}

void FrameResourceManager::BeginFrame(uint64_t completedFence, uint64_t nextFence) {
    if (!m_initialized)
        return;

    m_objectCB.Reclaim(completedFence);
    m_skinning.Reclaim(completedFence);
    m_instance.Reclaim(completedFence);
    m_waterCB.Reclaim(completedFence);

    m_currentFence = nextFence;
}

D3D12_GPU_VIRTUAL_ADDRESS FrameResourceManager::GetPassCBAddress() const { return m_passCBAddress; }

void FrameResourceManager::UpdatePassConstants() {
    if (!m_initialized || !m_passCBMapped) {
        return;
    }

    memcpy(m_passCBMapped, &m_passConstants, sizeof(PassConstants));
}

D3D12_GPU_VIRTUAL_ADDRESS FrameResourceManager::AllocateWithRetry(RingBuffer &buffer, const void *data, uint32_t size,
                                                                  uint64_t fence, uint32_t alignment) {
    D3D12_GPU_VIRTUAL_ADDRESS addr;
    if (data) {
        addr = buffer.AllocateUpload(data, size, fence, alignment);
    } else {
        addr = buffer.Allocate(size, fence, alignment);
    }

    if (addr == 0) {
        // 扩容：当前大小翻倍，至少满足请求大小
        uint32_t newSize = std::max(buffer.GetSize() * 2, size);
        wchar_t msg[256];
        swprintf_s(msg, L"[WARN] FrameResourceManager: RingBuffer expanding from %u to %u bytes (requested %u)\n",
                   buffer.GetSize(), newSize, size);
        OutputDebugStringW(msg);
        buffer.Initialize(m_device, newSize, buffer.GetName());

        if (data) {
            addr = buffer.AllocateUpload(data, size, fence, alignment);
        } else {
            addr = buffer.Allocate(size, fence, alignment);
        }
    }
    return addr;
}

D3D12_GPU_VIRTUAL_ADDRESS FrameResourceManager::AllocateObjectCB(const void *data, uint32_t size) {
    if (!m_initialized)
        return 0;
    return AllocateWithRetry(m_objectCB, data, size, m_currentFence);
}

D3D12_GPU_VIRTUAL_ADDRESS FrameResourceManager::AllocateSkinning(const void *data, uint32_t size) {
    if (!m_initialized)
        return 0;
    return AllocateWithRetry(m_skinning, data, size, m_currentFence, 16);
}

D3D12_GPU_VIRTUAL_ADDRESS FrameResourceManager::AllocateInstance(const void *data, uint32_t size) {
    if (!m_initialized)
        return 0;
    return AllocateWithRetry(m_instance, data, size, m_currentFence, 16);
}

D3D12_GPU_VIRTUAL_ADDRESS FrameResourceManager::AllocateWaterCB(const void *data, uint32_t size) {
    if (!m_initialized)
        return 0;
    return AllocateWithRetry(m_waterCB, data, size, m_currentFence);
}

void *FrameResourceManager::GetCPUAddress(uint32_t offset) {
    if (!m_initialized) {
        return nullptr;
    }

    return m_objectCB.GetCPUAddress(offset);
}

uint32_t FrameResourceManager::AllocateTemporarySrvSlot() {
    if (!m_initialized || !m_descriptorHeaps) {
        return UINT32_MAX;
    }

    return m_descriptorHeaps->Allocate(PartitionType::Buffer);
}

void FrameResourceManager::FreeTemporarySrvSlot(uint32_t slot, uint64_t fence) {
    if (!m_initialized || !m_descriptorHeaps) {
        return;
    }

    m_descriptorHeaps->Free(PartitionType::Buffer, slot, fence);
}

} // namespace DX12Engine::Renderer