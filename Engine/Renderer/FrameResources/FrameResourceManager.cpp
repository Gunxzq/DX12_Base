#include "FrameResourceManager.h"
#include "FrameResourceConfig.h"
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

RingBuffer *FrameResourceManager::FindBuffer(const std::string &name) {
    for (auto &entry : m_ringBuffers) {
        if (entry.name == name)
            return &entry.buffer;
    }
    return nullptr;
}

void FrameResourceManager::Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps,
                                      const FrameResourceConfig &config) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;

    // 创建 PassCB
    CreatePassCB(device);

    // 按配置创建环形缓冲区
    for (const auto &rbCfg : config.ringBuffers) {
        auto &entry = m_ringBuffers.emplace_back();
        entry.name = rbCfg.name;
        entry.alignment = rbCfg.alignment;

        std::wstring wname(rbCfg.name.begin(), rbCfg.name.end());
        entry.buffer.Initialize(device, rbCfg.initialSize, wname);
    }

    m_passConstants = {};
    UpdatePassConstants();

    m_initialized = true;
}

void FrameResourceManager::Shutdown() {
    if (!m_initialized) {
        return;
    }

    for (auto &entry : m_ringBuffers) {
        entry.buffer.Shutdown();
    }
    m_ringBuffers.clear();

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

    for (auto &entry : m_ringBuffers) {
        entry.buffer.Reclaim(completedFence);
    }

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

D3D12_GPU_VIRTUAL_ADDRESS FrameResourceManager::Allocate(const std::string &name, const void *data, uint32_t size) {
    if (!m_initialized)
        return 0;

    RingBuffer *buf = FindBuffer(name);
    if (!buf)
        return 0;

    // 查找对应 alignment
    uint32_t alignment = 256;
    for (const auto &entry : m_ringBuffers) {
        if (entry.name == name) {
            alignment = entry.alignment;
            break;
        }
    }

    return AllocateWithRetry(*buf, data, size, m_currentFence, alignment);
}

void *FrameResourceManager::GetCPUAddress(uint32_t offset) {
    if (!m_initialized) {
        return nullptr;
    }

    // 默认返回第一个 RingBuffer 的 CPU 地址（通常为 ObjectCB）
    if (!m_ringBuffers.empty()) {
        return m_ringBuffers[0].buffer.GetCPUAddress(offset);
    }
    return nullptr;
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
