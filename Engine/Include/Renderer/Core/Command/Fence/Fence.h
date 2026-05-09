#pragma once

#include "Common/Common.h"
#include "Common/d3dUtil.h"
#include <atomic>
#include <cstdint>
#include <wrl/client.h>

namespace DX12Engine {
namespace Renderer {

// ========================================================================
// Fence - 围栏对象
// 封装 DX12 围栏（Fence）对象，用于 GPU 与 CPU 同步
// 利用全局原子计数器作为围栏值，描述程序开始到结束的生命周期
// ========================================================================

class Fence {
public:
    Fence() = default;
    ~Fence() {
        if (m_event) {
            CloseHandle(m_event);
        }
    }

    // 禁止拷贝和移动
    Fence(const Fence &) = delete;
    Fence &operator=(const Fence &) = delete;
    Fence(Fence &&) = delete;
    Fence &operator=(Fence &&) = delete;

    /**
     * @brief 创建围栏对象
     * @param device D3D12 设备
     * @param initialValue 初始围栏值，默认为 0
     */
    void Create(ID3D12Device *device, uint64_t initialValue = 0) {
        ThrowIfFailed(device->CreateFence(initialValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        m_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_event) {
            throw std::runtime_error("Failed to create fence event");
        }
        m_lastSignaled = initialValue;
    }

    /**
     * @brief GPU 端 Signal：递增围栏值（线程安全）
     * @param queue 命令队列
     * @return uint64_t 新的围栏值
     */
    uint64_t Signal(ID3D12CommandQueue *queue) {
        uint64_t value = m_lastSignaled.fetch_add(1) + 1;
        queue->Signal(m_fence.Get(), value);
        return value;
    }

    /**
     * @brief 查询当前完成的围栏值（只读，线程安全）
     * @return uint64_t 已完成的围栏值
     */
    uint64_t GetCompletedValue() const { return m_fence->GetCompletedValue(); }

    /**
     * @brief CPU 端等待：阻塞直到指定围栏值完成
     * @param value 要等待的围栏值
     */
    void Wait(uint64_t value) {
        if (m_fence->GetCompletedValue() >= value) {
            return;
        }
        ThrowIfFailed(m_fence->SetEventOnCompletion(value, m_event));
        WaitForSingleObject(m_event, INFINITE);
    }

    /**
     * @brief 获取底层围栏对象
     * @return ID3D12Fence* 围栏指针
     */
    ID3D12Fence *Get() const { return m_fence.Get(); }

    /**
     * @brief 获取最近一次 Signal 的值
     * @return uint64_t 最近一次 Signal 的围栏值
     */
    uint64_t GetLastSignaledValue() const { return m_lastSignaled.load(); }

private:
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence; // DX12 围栏对象
    HANDLE m_event = nullptr;                    // 事件句柄，用于 CPU 等待
    std::atomic<uint64_t> m_lastSignaled{0};     // 最近一次 Signal 的值
};

} // namespace Renderer
} // namespace DX12Engine
