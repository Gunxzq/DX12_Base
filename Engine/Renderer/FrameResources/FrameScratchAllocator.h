#pragma once

#include "Common/d3dUtil.h"
#include <wrl/client.h>

namespace DX12Engine::Renderer {

// 临时上传分配结果
struct ScratchAllocation {
    void *cpuPtr = nullptr;                // CPU 映射写入地址
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = 0; // GPU 可见地址
};

/**
 * @brief 帧临时上传分配器（Scratch Allocator）
 *
 * 管理 2 块 upload heap ring-buffer，交替使用，fence 同步。
 * 适用于小量、临时、ad-hoc 的 GPU 上传（预览 CB、debug 绘制、ImGui 等）。
 *
 * 生命周期：
 *   Frame N:   BeginFrame(completedFence) → 切换到 buffer[A] ← 等待 fence[A] 完成
 *             Allocate() / memcpy → 写入 buffer[A]
 *             FrameDriver 提交命令列表 → 信号 fence[N]
 *   Frame N+1: BeginFrame(completedFence) → 切换到 buffer[B] ← fence[A] 已过时，无需等待
 *             Allocate() / memcpy → 写入 buffer[B]
 *             FrameDriver 提交命令列表 → 信号 fence[N+1]
 *   Frame N+2: BeginFrame(completedFence) → 切换到 buffer[A] ← fence[N] 已过时，buffer[A] 可用
 */
class FrameScratchAllocator {
public:
    FrameScratchAllocator() = default;
    ~FrameScratchAllocator();

    FrameScratchAllocator(const FrameScratchAllocator &) = delete;
    FrameScratchAllocator &operator=(const FrameScratchAllocator &) = delete;

    FrameScratchAllocator(FrameScratchAllocator &&other) noexcept
        : m_buffers{std::move(other.m_buffers[0]), std::move(other.m_buffers[1])}, m_currentIndex(other.m_currentIndex),
          m_size(other.m_size), m_head(other.m_head), m_name(std::move(other.m_name)),
          m_initialized(other.m_initialized) {
        other.m_currentIndex = 0;
        other.m_size = 0;
        other.m_head = 0;
        other.m_initialized = false;
    }

    FrameScratchAllocator &operator=(FrameScratchAllocator &&other) noexcept {
        if (this != &other) {
            Shutdown();
            m_buffers[0] = std::move(other.m_buffers[0]);
            m_buffers[1] = std::move(other.m_buffers[1]);
            m_currentIndex = other.m_currentIndex;
            m_size = other.m_size;
            m_head = other.m_head;
            m_name = std::move(other.m_name);
            m_initialized = other.m_initialized;
            other.m_currentIndex = 0;
            other.m_size = 0;
            other.m_head = 0;
            other.m_initialized = false;
        }
        return *this;
    }

    bool Initialize(ID3D12Device *device, uint32_t size, const std::wstring &name = L"FrameScratchAllocator");
    void Shutdown();

    void BeginFrame(uint64_t completedFence);
    void SubmitFrame(uint64_t fence);

    ScratchAllocation Allocate(uint32_t size, uint32_t alignment = 256);

    bool IsInitialized() const { return m_initialized; }
    uint32_t GetSize() const { return m_size; }          // 单块大小
    uint32_t GetTotalSize() const { return m_size * 2; } // 总大小
    uint32_t GetUsedSize() const { return m_head; }      // 当前块已用
    uint32_t GetRemainingSpace() const;                  // 当前块剩余

private:
    struct Buffer {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        void *mappedData = nullptr;               // CPU 映射指针
        D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0; // GPU 虚拟地址
        uint64_t pendingFence = 0;                // 该 buffer 正在被 GPU 使用的 fence 值
    };

    Buffer m_buffers[2];
    uint32_t m_currentIndex = 0; // 当前使用的 buffer 索引
    uint32_t m_size = 0;         // 单块大小
    uint32_t m_head = 0;         // 当前块写指针
    std::wstring m_name;
    bool m_initialized = false;
};

} // namespace DX12Engine::Renderer