#pragma once

#include "Common/Common.h"
#include "ECS/Core/Entity.h"
#include "GameTimer.h"
#include "Logger/Logger.h"
#include "Renderer/Core/CullingSystem.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Core/RenderQueue.h"
#include "Resource/Struct/TextureHandle.h"
#include "Resource/Texture/TextureManager.h"
#include <unordered_map>

// 前向声明 Renderer 命名空间中的类型
namespace DX12Engine::Renderer {
class D3D12DeviceContext;
class CameraManager;
template <D3D12_COMMAND_LIST_TYPE Type> class CommandAllocatorPool;
template <D3D12_COMMAND_LIST_TYPE Type> class CommandListPool;
class CommandList;
} // namespace DX12Engine::Renderer

namespace DX12Engine {
namespace Renderer {
class FrameResourceManager;
class RenderItemBuilder;
} // namespace Renderer

namespace Resource {
class DescriptorHeapCollection;
class GeometryResourceManager;
class MaterialManager;
} // namespace Resource
namespace Input {
class InputManager;
}

namespace Event {
class MessageDispatcher;
}

namespace ECS {
class Registry;
}

namespace Scheduler {
class FrameDriver;
}

namespace Logger {
class Logger;
}

namespace Platform {
class Window;
}

namespace Boot {

// ========================================================================
// 前向声明
// ========================================================================

class ConfigManager;
class GameTimer;

// ========================================================================
// GameContext - 依赖注入容器
// ========================================================================

class GameContext {
public:
    GameContext() = default;
    ~GameContext() = default;

    // 禁止拷贝和移动
    GameContext(const GameContext &) = delete;
    GameContext &operator=(const GameContext &) = delete;
    GameContext(GameContext &&) = delete;
    GameContext &operator=(GameContext &&) = delete;

    // ── 基础设施子系统指针 ──
    Platform::Window *Window = nullptr;
    ConfigManager *Config = nullptr;
    Logger::Logger *Logging = nullptr;
    GameTimer *MainTimer = nullptr;
    Event::MessageDispatcher *Dispatcher = nullptr;

    // ── 调度与数据层指针 ──
    Scheduler::FrameDriver *FrameDriver = nullptr;
    ECS::Registry *Registry = nullptr;

    // ── 渲染子系统指针 ──
    Renderer::D3D12DeviceContext *DeviceContext = nullptr;
    Renderer::CameraManager *CameraMgr = nullptr;
    Resource::DescriptorHeapCollection *DescriptorHeaps = nullptr;
    Renderer::FrameResourceManager *FrameResourceManager = nullptr;
    Resource::GeometryResourceManager *GeometryResourceManager = nullptr;

    Resource::MaterialManager *MaterialMgr = nullptr;
    Resource::TextureManager *TextureMgr = nullptr;

    Input::InputManager *InputMgr = nullptr;

    // PreRender 阶段的临时结果（每帧重置）
    Renderer::CullingResult cullingResult; // 改为 CullingResult 类型
    Renderer::LODResult lodResult;         // 改为 LODResult 类型
    Renderer::RenderQueue renderQueue;

    Renderer::CullingSystem *CullingSystem = nullptr;
    Renderer::LODSystem *LODSystem = nullptr;
    Renderer::RenderItemBuilder *RenderItemBuilder = nullptr;

    D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress = 0;
    Resource::TextureHandle testTextureHandle; // 测试纹理句柄
    D3D12_GPU_DESCRIPTOR_HANDLE testTextureSRVHandle = {0};

    // ── 便捷访问方法 ──
    bool IsValid() const;
    const char *GetInvalidReason() const;

    mutable const char *m_invalidReason = nullptr;

    UINT GetBackBufferIndex() const;
    ID3D12Resource *GetBackBuffer() const;
    uint64_t GetFenceValue(D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);
    uint64_t GetCompletedFence() const;
    uint64_t GetNextFence() const;

    void FlushAllQueues();

    // ── 命令系统便捷方法（单一声明）──
    template <D3D12_COMMAND_LIST_TYPE Type>
    typename Renderer::CommandAllocatorPool<Type>::Handle GetAllocatorHandle(uint64_t currentCompleted);

    template <D3D12_COMMAND_LIST_TYPE Type>
    typename ID3D12CommandAllocator *GetAllocator(const typename Renderer::CommandAllocatorPool<Type>::Handle &handle);

    template <D3D12_COMMAND_LIST_TYPE Type>
    typename Renderer::CommandListPool<Type>::Handle AcquireCommandListHandle(ID3D12CommandAllocator *allocator);

    template <D3D12_COMMAND_LIST_TYPE Type>
    Renderer::CommandList GetCommandList(const typename Renderer::CommandListPool<Type>::Handle &handle);

    template <D3D12_COMMAND_LIST_TYPE Type>
    void ReleaseCommandList(const typename Renderer::CommandListPool<Type>::Handle &handle);

    template <D3D12_COMMAND_LIST_TYPE Type>
    void ReleaseAllocator(const typename Renderer::CommandAllocatorPool<Type>::Handle &handle, uint64_t fenceValue);

    uint64_t GetNextSequence();
};

} // namespace Boot
} // namespace DX12Engine

// 注意：模板实现已经在 GameContext.cpp 中通过显式实例化完成
// 不需要 .inl 文件，因为所有使用的类型都已经显式实例化了