#include "RingBuffer.h"

#include "Common/Common.h"

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
    if (!m_initialized || size == 0 || size > m_size) {
        return 0;
    }

    // 对齐当前写指针
    uint32_t alignedHead = (m_head + alignment - 1) & ~(alignment - 1);
    uint32_t alignedSize = size + (alignedHead - m_head);

    // 检查是否需要回绕
    bool needsWrap = (alignedHead + alignedSize > m_size);

    if (needsWrap) {
        // 需要回绕到开头
        if (alignedSize > m_tail) {
            return 0; // 空间不足
        }

        // 记录尾部的浪费空间
        uint32_t wastedSpace = m_size - m_head;
        if (wastedSpace > 0) {
            m_pending.push({wastedSpace, fence});
            m_allocatedSize += wastedSpace;
        }

        // 在开头分配
        D3D12_GPU_VIRTUAL_ADDRESS result = m_gpuAddress;
        m_pending.push({size, fence});
        m_head = size;
        m_allocatedSize += size;

        return result;
    } else {
        // 正常在尾部分配
        if (alignedHead + alignedSize > m_size) {
            return 0;
        }

        D3D12_GPU_VIRTUAL_ADDRESS result = m_gpuAddress + alignedHead;
        m_pending.push({alignedSize, fence});
        m_head = alignedHead + size;
        m_allocatedSize += alignedSize;

        return result;
    }
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

D3D12_GPU_VIRTUAL_ADDRESS RingBuffer::GetGPUAddress(uint32_t offset) const {
    if (!m_initialized || offset >= m_size) {
        return 0;
    }
    return m_gpuAddress + offset;
}

void RingBuffer::Free(uint32_t offset, uint64_t fence) {
    if (!m_initialized || offset >= m_size) {
        return;
    }

    // 注意：由于环形缓冲区的特性，不能随意释放任意偏移
    // 必须按分配顺序释放（FIFO）
    // 这里简化处理：如果释放的不是尾部，需要特殊处理

    // 实际实现中，可以遍历 pending 队列找到对应的分配
    // 或者要求调用者保证按顺序释放
}

} // namespace DX12Engine::Renderer