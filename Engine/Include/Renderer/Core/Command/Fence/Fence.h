#pragma once
#include "Common/d3dUtil.h"
#include <stdexcept>
#include <windows.h>
#include <wrl/client.h>

namespace DX12Engine {
namespace Renderer {

// ========================================================================
// Fence - 极简围栏资源包装
// 职责：仅管理 ID3D12Fence 和关联的 Event 句柄的生命周期
// 设计原则：无状态、无行为，仅作为 DX12 原生资源的 RAII 容器
// ========================================================================

class Fence {
public:
    /**
     * @brief 构造函数
     * @param device D3D12 设备
     * @param initialValue 初始围栏值
     */
    explicit Fence(ID3D12Device *device, uint64_t initialValue = 0) {
        ThrowIfFailed(device->CreateFence(initialValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));

        m_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_event) {
            throw std::runtime_error("Failed to create fence event handle");
        }
    }

    ~Fence() {
        if (m_event) {
            CloseHandle(m_event);
            m_event = nullptr;
        }
        // ComPtr 会自动释放 m_fence
    }

    // 禁止拷贝和移动，确保资源唯一所有权
    Fence(const Fence &) = delete;
    Fence &operator=(const Fence &) = delete;
    Fence(Fence &&) = delete;
    Fence &operator=(Fence &&) = delete;

    // ========================================================================
    // 访问器：仅暴露底层原生指针/句柄供 Manager 使用
    // ========================================================================

    ID3D12Fence *Get() const { return m_fence.Get(); }
    HANDLE GetEventHandle() const { return m_event; }

private:
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_event = nullptr;
};

} // namespace Renderer
} // namespace DX12Engine