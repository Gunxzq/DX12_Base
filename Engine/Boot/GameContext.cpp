#include "GameContext.h"
#include "ConfigManager.h"
#include "Logger/Logger.h"
#include "Platform/Input/InputSystem.h"
#include "Platform/Windows/Window.h"
#include "Renderer/RHI/Command/Allocator/CommandAllocatorPool.h"
#include "Renderer/RHI/Command/CommandList/CommandListPool.h"
#include "Renderer/RHI/Command/CommandManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"

using namespace DX12Engine::Boot;
using namespace DX12Engine::Input;
using namespace DX12Engine::Renderer;

namespace DX12Engine::Boot {

bool GameContext::IsValid() const {
    if (!Window) {
        m_invalidReason = "Window is not set";
        return false;
    }
    if (!Config) {
        m_invalidReason = "Config is not set";
        return false;
    }
    if (!Logging) {
        m_invalidReason = "Logging is not set";
        return false;
    }
    if (!MainTimer) {
        m_invalidReason = "MainTimer is not set";
        return false;
    }
    if (!DeviceContext) {
        m_invalidReason = "DeviceContext is not set";
        return false;
    }
    m_invalidReason = nullptr;
    return true;
}

const char *GameContext::GetInvalidReason() const { return m_invalidReason ? m_invalidReason : "All fields are valid"; }

/**
 * @brief 获取当前后台缓冲区索引
 * @return UINT
 * @date 2026-05-22
 */
UINT GameContext::GetBackBufferIndex() const {
    if (DeviceContext) {
        return DeviceContext->GetSwapChainManager().GetCurrentIndex();
    }
    return 0;
}

/**
 * @brief 获取当前后台缓冲区资源指针
 * @return ID3D12Resource*
 * @date 2026-05-22
 */
ID3D12Resource *GameContext::GetBackBuffer() const {
    if (DeviceContext) {
        return DeviceContext->GetSwapChainManager().GetCurrentBackBuffer();
    }

    return nullptr;
}

/**
 * @brief 获取指定类型命令队列的Fence 值
 * @param type
 * @return uint64_t
 * @date 2026-05-22
 */
uint64_t GameContext::GetFenceValue(D3D12_COMMAND_LIST_TYPE type) {
    if (DeviceContext) {
        return DeviceContext->GetCommandManager().GetCompletedFenceValue(type);
    }
    return 0;
}

// ========================================================================
// 命令系统便捷方法实现
// ========================================================================

void GameContext::FlushAllQueues() {
    if (DeviceContext) {
        DeviceContext->GetCommandManager().FlushAllQueues();
    }
}

template <D3D12_COMMAND_LIST_TYPE Type>
typename Renderer::CommandAllocatorPool<Type>::Handle GameContext::GetAllocatorHandle(uint64_t currentCompleted) {
    if (DeviceContext) {
        return DeviceContext->GetCommandManager().AcquireAllocator<Type>(currentCompleted);
    }
    return {};
}

template <D3D12_COMMAND_LIST_TYPE Type>
ID3D12CommandAllocator *GameContext::GetAllocator(const typename Renderer::CommandAllocatorPool<Type>::Handle &handle) {
    if (DeviceContext) {
        return DeviceContext->GetCommandManager().GetAllocator<Type>(handle);
    }
    return {};
}

template <D3D12_COMMAND_LIST_TYPE Type>
typename Renderer::CommandListPool<Type>::Handle
GameContext::AcquireCommandListHandle(ID3D12CommandAllocator *allocator) {
    if (DeviceContext) {
        return DeviceContext->GetCommandManager().AcquireCommandListHandle<Type>(allocator);
    }
    return {};
}

template <D3D12_COMMAND_LIST_TYPE Type>
Renderer::CommandList GameContext::GetCommandList(const typename Renderer::CommandListPool<Type>::Handle &handle) {
    if (DeviceContext) {
        return DeviceContext->GetCommandManager().GetCommandList<Type>(handle);
    }
    return {};
}

template <D3D12_COMMAND_LIST_TYPE Type>
void GameContext::ReleaseCommandList(const typename Renderer::CommandListPool<Type>::Handle &handle) {
    if (DeviceContext) {
        DeviceContext->GetCommandManager().ReleaseCommandList<Type>(handle);
    }
}

template <D3D12_COMMAND_LIST_TYPE Type>
void GameContext::ReleaseAllocator(const typename Renderer::CommandAllocatorPool<Type>::Handle &handle,
                                   uint64_t fenceValue) {
    if (DeviceContext) {
        DeviceContext->GetCommandManager().ReleaseAllocator<Type>(handle, fenceValue);
    }
}

uint64_t GameContext::GetNextSequence() {
    if (DeviceContext) {
        return DeviceContext->GetCommandManager().GetNextSequence();
    }
    return 0;
}

// 显式实例化常用类型
template typename Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle
    GameContext::GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(uint64_t);
template typename Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>::Handle
    GameContext::GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_COMPUTE>(uint64_t);
template typename Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COPY>::Handle
    GameContext::GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_COPY>(uint64_t);

template typename ID3D12CommandAllocator *GameContext::GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(
    const typename Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle &);
template typename ID3D12CommandAllocator *GameContext::GetAllocator<D3D12_COMMAND_LIST_TYPE_COMPUTE>(
    const typename Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>::Handle &);
template typename ID3D12CommandAllocator *GameContext::GetAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(
    const typename Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COPY>::Handle &);

template typename Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle
GameContext::AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(ID3D12CommandAllocator *);
template typename Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>::Handle
GameContext::AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COMPUTE>(ID3D12CommandAllocator *);
template typename Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_COPY>::Handle
GameContext::AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_COPY>(ID3D12CommandAllocator *);

template Renderer::CommandList GameContext::GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(
    const typename Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle &);
template Renderer::CommandList GameContext::GetCommandList<D3D12_COMMAND_LIST_TYPE_COMPUTE>(
    const typename Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>::Handle &);
template Renderer::CommandList GameContext::GetCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(
    const typename Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_COPY>::Handle &);

template void GameContext::ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(
    const typename Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle &);
template void GameContext::ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_COMPUTE>(
    const typename Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>::Handle &);
template void GameContext::ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_COPY>(
    const typename Renderer::CommandListPool<D3D12_COMMAND_LIST_TYPE_COPY>::Handle &);

template void GameContext::ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(
    const typename Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_DIRECT>::Handle &, uint64_t);
template void GameContext::ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_COMPUTE>(
    const typename Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COMPUTE>::Handle &, uint64_t);
template void GameContext::ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_COPY>(
    const typename Renderer::CommandAllocatorPool<D3D12_COMMAND_LIST_TYPE_COPY>::Handle &, uint64_t);

} // namespace DX12Engine::Boot