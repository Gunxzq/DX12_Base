#include "RingBuffer.h"

#include "Common/Common.h"

using namespace DX12Engine::Renderer;

namespace DX12Engine::Renderer {

RingBuffer::~RingBuffer() { Shutdown(); }

bool RingBuffer::Initialize(ID3D12Device *device, uint32_t size, const std::wstring &name, D3D12_HEAP_TYPE heapType,
                            D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState) {
    if (m_initialized) {
        Shutdown();
    }

    if (size == 0 || !device) {
        return false;
    }

    m_name = name;
    m_heapType = heapType;
    m_initialState = initialState;

    CD3DX12_HEAP_PROPERTIES heapProps(heapType);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    desc.Flags = flags;

    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                                 IID_PPV_ARGS(&m_resource));

    if (FAILED(hr)) {
        wchar_t msg[256];
        swprintf_s(msg, L"[RingBuffer] CreateCommittedResource failed: hr=0x%08X\n", hr);
        OutputDebugStringW(msg);
        return false;
    }

    if (!m_name.empty())
        m_resource->SetName(m_name.c_str());

    if (m_heapType == D3D12_HEAP_TYPE_UPLOAD || m_heapType == D3D12_HEAP_TYPE_READBACK) {
        hr = m_resource->Map(0, nullptr, &m_mappedData);
        if (FAILED(hr)) {
            m_resource.Reset();
            // 输出错误信息
            wchar_t msg[512];
            swprintf_s(msg, L"[RingBuffer] Map failed: hr=0x%08X\n", hr);
            OutputDebugStringW(msg);
            return false;
        }
    } else {
        m_mappedData = nullptr; // DEFAULT 堆不可 Map
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

bool RingBuffer::IsMappable() const {
    return m_heapType == D3D12_HEAP_TYPE_UPLOAD || m_heapType == D3D12_HEAP_TYPE_READBACK;
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

    // 1. 计算物理偏移（取模）
    uint32_t physicalHead = m_head % m_size;
    uint32_t alignedPhysicalHead = ((physicalHead + alignment - 1) / alignment) * alignment;
    uint32_t alignedSize = size + (alignedPhysicalHead - physicalHead);

    // 2. 检查是否需要回绕
    if (alignedPhysicalHead + alignedSize > m_size) {
        // 空间不足？
        if (alignedSize > m_tail) {
            wchar_t msg[256];
            swprintf_s(msg, L"[RingBuffer] Allocate failed: name=%s, size = %d, fence = %d, alignment = %d\n",
                       m_name.c_str(), size, fence, alignment);
            OutputDebugStringW(msg);
            return 0;
        }

        // 记录尾部的浪费空间
        uint32_t wastedSpace = m_size - alignedPhysicalHead;
        if (wastedSpace > 0) {
            m_pending.push({wastedSpace, fence});
            m_allocatedSize += wastedSpace;
        }

        // 在开头分配（物理偏移 0）
        D3D12_GPU_VIRTUAL_ADDRESS result = m_gpuAddress;
        m_pending.push({size, fence});
        m_head += wastedSpace + size; // 逻辑偏移增加
        m_allocatedSize += size;

        if (IsMappable() && m_mappedData) {
            memset(static_cast<uint8_t *>(m_mappedData), 0, size);
        }
        return result;
    } else {
        // 正常尾部分配
        if (alignedPhysicalHead + alignedSize > m_size) {
            return 0;
        }

        D3D12_GPU_VIRTUAL_ADDRESS result = m_gpuAddress + alignedPhysicalHead;
        m_pending.push({alignedSize, fence});
        m_head += alignedSize; // 逻辑偏移增加
        m_allocatedSize += alignedSize;

        return result;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS RingBuffer::AllocateUpload(const void *data, uint32_t size, uint64_t fence,
                                                     uint32_t alignment) {

    if (!IsMappable()) {
        return 0;
    }

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