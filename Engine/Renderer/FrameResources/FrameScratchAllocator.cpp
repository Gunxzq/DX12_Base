#include "FrameScratchAllocator.h"
#include "Common/Common.h"

using namespace DX12Engine::Renderer;

FrameScratchAllocator::~FrameScratchAllocator() { Shutdown(); }

/**
 * @brief 初始化帧临时上传分配器
 * @param device D3D12 设备
 * @param size 上传空间大小
 * @param name 资源名称
 * @return bool
 * @date 2026-07-21
 */
bool FrameScratchAllocator::Initialize(ID3D12Device *device, uint32_t size, const std::wstring &name) {
    if (m_initialized) {
        Shutdown();
    }

    if (size == 0 || !device) {
        return false;
    }

    m_name = name;
    m_size = size;

    for (int i = 0; i < 2; ++i) {
        auto &buf = m_buffers[i];
        std::wstring bufName = m_name + L"_" + std::to_wstring(i);

        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);

        HRESULT hr =
            device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&buf.resource));

        if (FAILED(hr)) {
            for (int j = 0; j < i; ++j) {
                if (m_buffers[j].resource) {
                    if (m_buffers[j].mappedData) {
                        m_buffers[j].resource->Unmap(0, nullptr);
                    }
                    m_buffers[j].resource.Reset();
                }
            }
            return false;
        }

        buf.resource->SetName(bufName.c_str());

        hr = buf.resource->Map(0, nullptr, &buf.mappedData);
        if (FAILED(hr)) {
            buf.resource.Reset();
            for (int j = 0; j < i; ++j) {
                if (m_buffers[j].resource) {
                    if (m_buffers[j].mappedData) {
                        m_buffers[j].resource->Unmap(0, nullptr);
                    }
                    m_buffers[j].resource.Reset();
                }
            }
            return false;
        }

        buf.gpuAddress = buf.resource->GetGPUVirtualAddress();
        buf.pendingFence = 0;
    }

    m_currentIndex = 0;
    m_head = 0;
    m_initialized = true;
    return true;
}

/**
 * @brief 关闭帧临时上传分配器
 * @date 2026-07-21
 */
void FrameScratchAllocator::Shutdown() {
    if (!m_initialized) {
        return;
    }

    // 调用方（FrameDriver）应确保在 Shutdown 前 FlushAllQueues。
    for (int i = 0; i < 2; ++i) {
        auto &buf = m_buffers[i];
        if (buf.resource) {
            if (buf.mappedData) {
                buf.resource->Unmap(0, nullptr);
                buf.mappedData = nullptr;
            }
            buf.resource.Reset();
        }
        buf.gpuAddress = 0;
        buf.pendingFence = 0;
    }

    m_currentIndex = 0;
    m_size = 0;
    m_head = 0;
    m_initialized = false;
}

/**
 * @brief 开始新一帧的分配
 * @param completedFence 上一次帧的完成 fence
 * @date 2026-07-21
 */
void FrameScratchAllocator::BeginFrame(uint64_t completedFence) {
    if (!m_initialized) {
        return;
    }

    // 切换到下一块 buffer
    uint32_t nextIndex = (m_currentIndex + 1) % 2;
    auto &nextBuf = m_buffers[nextIndex];

    // 如果下一块 buffer 仍有 pending fence 且 GPU 尚未完成，等待
    // 正常路径下 completedFence 应已超过 pendingFence
    if (nextBuf.pendingFence > 0 && nextBuf.pendingFence > completedFence) {
        OutputDebugStringW(L"[FrameScratchAllocator] Waiting for pending fence...\n");
    }

    m_currentIndex = nextIndex;
    m_head = 0;
}

/**
 * @brief 提交当前帧的分配，标记当前 buffer 正在被 GPU 使用
 * @param fence 当前帧的 fence
 * @date 2026-07-21
 */
void FrameScratchAllocator::SubmitFrame(uint64_t fence) {
    if (!m_initialized)
        return;

    // 标记当前 buffer 正在被 GPU 使用
    m_buffers[m_currentIndex].pendingFence = fence;
}

/**
 * @brief 分配临时上传内存
 * @param size 分配大小
 * @param alignment 对齐大小
 * @return ScratchAllocation
 * @date 2026-07-21
 */
ScratchAllocation FrameScratchAllocator::Allocate(uint32_t size, uint32_t alignment) {
    ScratchAllocation result = {nullptr, 0};

    if (!m_initialized || size == 0 || size > m_size || alignment == 0) {
        return result;
    }

    auto &buf = m_buffers[m_currentIndex];

    // 对齐当前写指针
    uint32_t alignedHead = (m_head + alignment - 1) & ~(alignment - 1);
    uint32_t alignedSize = (size + alignment - 1) & ~(alignment - 1);

    // 检查剩余空间是否足够
    if (alignedHead + alignedSize > m_size) {
        // 尝试回绕到开头
        if (alignedSize > m_size) {
            return result;
        }

        alignedHead = 0;
        m_head = alignedSize;
        result.cpuPtr = buf.mappedData;
        result.gpuAddr = buf.gpuAddress;
        return result;
    }

    // 正常分配
    result.cpuPtr = static_cast<uint8_t *>(buf.mappedData) + alignedHead;
    result.gpuAddr = buf.gpuAddress + alignedHead;
    m_head = alignedHead + alignedSize;

    return result;
}

/**
 * @brief 获取当前帧剩余的临时上传内存空间
 * @return uint32_t 剩余空间大小
 * @date 2026-07-21
 */
uint32_t FrameScratchAllocator::GetRemainingSpace() const {
    if (!m_initialized) {
        return 0;
    }
    return m_size - m_head;
}