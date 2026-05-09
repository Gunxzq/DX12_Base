#pragma once

#include "Common/Common.h"
#include "Common/d3dUtil.h"
#include "Renderer/Core/Fence.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace DX12Engine {
namespace Renderer {

// ========================================================================
// FenceManager - 围栏对象管理器
// 初始化围栏对象，管理一个原子计数器作为 GPU 生命周期的围栏值
// ========================================================================

class FenceManager {
public:
    FenceManager() = default;
    ~FenceManager() = default;

    // 禁止拷贝和移动
    FenceManager(const FenceManager &) = delete;
    FenceManager &operator=(const FenceManager &) = delete;
    FenceManager(FenceManager &&) = delete;
    FenceManager &operator=(FenceManager &&) = delete;

    /**
     * @brief 获取或创建指定类型的围栏对象（线程安全）
     * @param device D3D12 设备
     * @param type 命令列表类型
     * @return Fence* 围栏指针
     */
    Fence *GetOrCreateFence(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type) {
        {
            std::shared_lock lock(m_mutex);
            auto it = m_fences.find(type);
            if (it != m_fences.end()) {
                return it->second.get();
            }
        }

        std::unique_lock lock(m_mutex);
        // 双重检查
        auto it = m_fences.find(type);
        if (it != m_fences.end()) {
            return it->second.get();
        }

        // 获取当前真实完成值作为基准
        uint64_t baseValue = m_globalCompleted.load();

        auto fence = std::make_unique<Fence>();
        fence->Create(device, baseValue);

        Fence *ptr = fence.get();
        m_fences[type] = std::move(fence);
        return ptr;
    }

    /**
     * @brief Signal：GPU 端递增围栏值（线程安全）
     * @param fence 围栏指针
     * @param queue 命令队列
     * @return uint64_t 新的围栏值
     * @note 注意：不立即更新 m_globalCompleted，因为 GPU 还没完成
     */
    uint64_t Signal(Fence *fence, ID3D12CommandQueue *queue) { return fence->Signal(queue); }

    /**
     * @brief 刷新全局完成值：从 GPU 读取所有队列的完成值（线程安全）
     */
    void RefreshGlobalCompleted() {
        uint64_t maxCompleted = m_globalCompleted.load();

        std::shared_lock lock(m_mutex);
        for (auto &[type, fence] : m_fences) {
            uint64_t completed = fence->GetCompletedValue();
            if (completed > maxCompleted) {
                maxCompleted = completed;
            }
        }

        m_globalCompleted = maxCompleted;
    }

    /**
     * @brief 检查指定围栏值是否已完成（线程安全）
     * @param value 围栏值
     * @return bool 是否已完成
     */
    bool IsCompleted(uint64_t value) {
        RefreshGlobalCompleted();
        return m_globalCompleted.load() >= value;
    }

    /**
     * @brief 等待指定围栏值完成（通常主线程调用）
     * @param value 要等待的围栏值
     */
    void WaitForValue(uint64_t value) {
        RefreshGlobalCompleted();
        if (m_globalCompleted >= value) {
            return;
        }

        std::shared_lock lock(m_mutex);
        auto it = m_fences.find(D3D12_COMMAND_LIST_TYPE_DIRECT);
        if (it != m_fences.end()) {
            it->second->Wait(value);
            m_globalCompleted = value;
        }
    }

    /**
     * @brief 获取全局已完成的最大围栏值
     * @return uint64_t 全局完成值
     */
    uint64_t GetGlobalCompletedValue() const { return m_globalCompleted.load(); }

    /**
     * @brief 销毁所有围栏对象
     */
    void Shutdown() {
        std::unique_lock lock(m_mutex);
        m_fences.clear();
        m_globalCompleted = 0;
    }

private:
    mutable std::shared_mutex m_mutex;                                            // 读写锁
    std::unordered_map<D3D12_COMMAND_LIST_TYPE, std::unique_ptr<Fence>> m_fences; // 按类型存储的围栏
    std::atomic<uint64_t> m_globalCompleted{0};                                   // 全局完成的围栏值
};

} // namespace Renderer
} // namespace DX12Engine
