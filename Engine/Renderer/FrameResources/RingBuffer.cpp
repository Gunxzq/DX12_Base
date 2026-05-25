#include "RingBuffer.h"
#include <cassert>
#include <cstring>

using namespace DX12Engine::Renderer;

namespace DX12Engine::Renderer {

RingBuffer::~RingBuffer() { Shutdown(); }

bool RingBuffer::Initialize(ID3D12Device *device, uint32_t size, D3D12_HEAP_TYPE heapType) {
    if (m_initialized) {
        Shutdown();
    }

    if (size == 0 || !device) {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES heapProps(heapType);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);

    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_resource));

    if (FAILED(hr)) {
        return false;
    }

    hr = m_resource->Map(0, nullptr, &m_mappedData);
    if (FAILED(hr)) {
        m_resource.Reset();
        return false;
    }

    m_gpuAddress = m_resource->GetGPUVirtualAddress();
    m_size = size;
    m_head = 0;
    m_tail = 0;
    m_allocatedSize = 0;

    while (!m_pending.empty()) {
        m_pending.pop();
    }

    m_initialized = true;
    return true;
}

void RingBuffer::Shutdown() {
    if (!m_initialized) {
        return;
    }

    if (m_resource) {
        if (m_mappedData) {
            m_resource->Unmap(0, nullptr);
            m_mappedData = nullptr;
        }
        m_resource.Reset();
    }

    m_gpuAddress = 0;
    m_size = 0;
    m_head = 0;
    m_tail = 0;
    m_allocatedSize = 0;

    while (!m_pending.empty()) {
        m_pending.pop();
    }

    m_initialized = false;
}

void RingBuffer::Reclaim(uint64_t completedFence) {
    while (!m_pending.empty() && m_pending.front().fence <= completedFence) {
        const auto &alloc = m_pending.front();
        m_tail = (m_tail + alloc.size) % m_size;
        m_allocatedSize -= alloc.size;
        m_pending.pop();
    }
}

D3D12_GPU_VIRTUAL_ADDRESS RingBuffer::Allocate(uint32_t size, uint64_t fence, uint32_t alignment) {
    if (!m_initialized || size == 0) {
        return 0;
    }

    // 对齐当前写指针
    uint32_t alignedHead = (m_head + alignment - 1) & ~(alignment - 1);
    uint32_t padding = alignedHead - m_head;

    // 计算可用连续空间
    uint32_t freeSpace;
    if (m_head >= m_tail) {
        freeSpace = m_size - m_head + m_tail;
    } else {
        freeSpace = m_tail - m_head;
    }

    // 空间不足，尝试回绕
    if (freeSpace < padding + size) {
        if (m_tail == 0) {
            return 0;
        }
        // 回绕到开头
        alignedHead = 0;
        padding = (m_size - m_head) % m_size;
        freeSpace = m_tail;
        if (freeSpace < size) {
            return 0;
        }
    }

    D3D12_GPU_VIRTUAL_ADDRESS result = m_gpuAddress + alignedHead;
    m_head = alignedHead + size;
    m_allocatedSize += padding + size;
    m_pending.push({padding + size, fence});

    return result;
}

D3D12_GPU_VIRTUAL_ADDRESS RingBuffer::AllocateUpload(const void *data, uint32_t size, uint64_t fence,
                                                     uint32_t alignment) {
    D3D12_GPU_VIRTUAL_ADDRESS address = Allocate(size, fence, alignment);
    if (address == 0) {
        return 0;
    }

    uint32_t offset = static_cast<uint32_t>(address - m_gpuAddress);
    memcpy(static_cast<uint8_t *>(m_mappedData) + offset, data, size);

    return address;
}

void *RingBuffer::GetCPUAddress(uint32_t offset) const {
    if (!m_initialized || offset >= m_size) {
        return nullptr;
    }
    return static_cast<uint8_t *>(m_mappedData) + offset;
}

uint32_t RingBuffer::GetFreeSpace() const {
    if (m_head >= m_tail) {
        return m_size - m_head + m_tail;
    } else {
        return m_tail - m_head;
    }
}

void RingBuffer::Reset() {
    if (!m_initialized) {
        return;
    }

    m_head = 0;
    m_tail = 0;
    m_allocatedSize = 0;
    while (!m_pending.empty()) {
        m_pending.pop();
    }
}

} // namespace DX12Engine::Renderer