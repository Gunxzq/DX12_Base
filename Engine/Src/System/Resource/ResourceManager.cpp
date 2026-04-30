// ResourceManager.cpp
#include "System/Resource/ResourceManager.h"
#include <algorithm>
#include <cassert>
#include <iostream>

namespace DX12Engine {
namespace System {
namespace Resource {

// 【修复】全局帧计数器，模拟引擎主循环的帧增长
// 在实际工程中，这里应该引用 Engine::GetFrameCount()
static uint64_t s_globalFrameCount = 0;

ResourceManager &ResourceManager::GetInstance() {
    static ResourceManager instance;
    return instance;
}

void ResourceManager::Initialize() {
    m_handlePool.Initialize();
    m_dataPool.Initialize();
    m_pendingReleases.reserve(1024);
    s_globalFrameCount = 0;

    std::cout << "[ResourceManager] Initialized." << std::endl;
}

void ResourceManager::Shutdown() {
    std::cout << "[ResourceManager] Shutting down..." << std::endl;

    // 1. 强制立即释放所有待回收资源
    for (auto &pr : m_pendingReleases) {
        void *ptr = m_handlePool.GetDataPtr(pr.handle);
        if (ptr) {
            m_dataPool.Free(ptr);
        }
        m_handlePool.FreeSlot(pr.handle);
    }
    m_pendingReleases.clear();

    // 2. 关闭底层池
    m_dataPool.Shutdown();
    m_handlePool.Shutdown();

    std::cout << "[ResourceManager] Shutdown complete." << std::endl;
}

// --- 被动调用接口 ---

ResourceHandle ResourceManager::AllocateSlot(ResourceType type) {
    // 胶水逻辑：将上层的 Type 映射到底层 HandlePool 的初始化逻辑
    return m_handlePool.AllocateSlot(type);
}

void ResourceManager::RegisterData(ResourceHandle handle, void *dataPtr, size_t size) {
    // 胶水逻辑：验证状态流转合法性 (Loading -> Ready)
    if (!m_handlePool.Validate(handle)) {
        assert(false && "RegisterData: Invalid Handle");
        return;
    }

    ResourceState currentState = m_handlePool.GetState(handle);
    if (currentState != ResourceState::Loading) {
        // 允许重复注册吗？通常不允许。如果状态已经是 Ready，可能是逻辑错误
        if (currentState == ResourceState::Ready) {
            std::cerr << "[Warning] RegisterData called on already Ready handle." << std::endl;
            return;
        }
        assert(false && "RegisterData: Handle is not in Loading state");
        return;
    }

// 【修复点 1】Debug 模式下检查指针是否在 DataPool 的管辖范围内
// 防止外部传入栈指针或 malloc 指针导致 Shutdown 时崩溃
#ifdef _DEBUG
    if (dataPtr) {
        assert(m_dataPool.Contains(dataPtr) && "RegisterData: Pointer is not managed by DataPool!");
    }
#endif

    m_handlePool.SetDataPtr(handle, dataPtr);
    m_handlePool.SetState(handle, ResourceState::Ready);

    // 注意：正如架构文档所述，这里不发送消息。
    // 调用方 (LoadingBucket) 负责在 IO 完成后发出 ResourceReadyEvent
}

void *ResourceManager::GetData(ResourceHandle handle) const {
    // 胶水逻辑：防火墙检查
    if (!m_handlePool.Validate(handle)) {
        return nullptr;
    }

    if (m_handlePool.GetState(handle) != ResourceState::Ready) {
        return nullptr;
    }

    return m_handlePool.GetDataPtr(handle);
}

void ResourceManager::Release(ResourceHandle handle) {
    if (!m_handlePool.Validate(handle)) {
        return;
    }

    ResourceState state = m_handlePool.GetState(handle);

    // 幂等性保护：如果已经在排队释放或已空，忽略
    if (state == ResourceState::PendingRelease || state == ResourceState::Empty) {
        return;
    }

    // 策略：延迟回收
    PendingRelease pr;
    pr.handle = handle;
    // 【修复点 2】使用真实的全局帧计数，确保延迟回收逻辑生效
    pr.releaseFrame = GetCurrentFrame() + 3; // 延迟 3 帧

    m_pendingReleases.push_back(pr);

    // 状态流转：Ready -> PendingRelease
    // 此时 GetData 将返回 nullptr，防止渲染线程访问即将释放的内存
    m_handlePool.SetState(handle, ResourceState::PendingRelease);
}

void ResourceManager::Update(float deltaTime) {
    // 【修复点 3】每帧更新全局帧计数
    // 在实际引擎中，这通常由 Engine::Update() 统一驱动，这里为了独立测试模拟递增
    s_globalFrameCount++;

    uint64_t currentFrame = GetCurrentFrame();

    // 遍历延迟回收队列
    auto it = m_pendingReleases.begin();
    while (it != m_pendingReleases.end()) {
        if (currentFrame >= it->releaseFrame) {
            // 真正释放内存
            void *ptr = m_handlePool.GetDataPtr(it->handle);
            if (ptr) {
                m_dataPool.Free(ptr);
            }

            // 释放 Handle 槽位 (重置 Generation，允许复用)
            m_handlePool.FreeSlot(it->handle);

            // 从队列中移除
            it = m_pendingReleases.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t ResourceManager::GetActiveCount() const { return m_handlePool.GetActiveCount(); }

size_t ResourceManager::GetMemoryUsage() const { return m_dataPool.GetTotalAllocatedSize(); }

uint64_t ResourceManager::GetCurrentFrame() const {
    // 【修复点 4】返回真实的全局帧计数，而非硬编码的 0
    return s_globalFrameCount;
}

} // namespace Resource
} // namespace System
} // namespace DX12Engine