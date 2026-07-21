#pragma once

#include "Common/d3dUtil.h"
#include "PreviewContext.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/Struct/Descriptor.h"
#include <array>
#include <cstdint>
#include <functional>
#include <wrl/client.h>

namespace DX12Engine::Resource {
class RenderTargetPool;
} // namespace DX12Engine::Resource



namespace DX12Engine::Renderer { class D3D12DeviceContext; }

/**
 * @brief 预览渲染回调
 * 每帧由 PreviewManager 对每个 needsRender 的上下文直接调用，替代事件分发
 */
using PreviewRenderCallback = std::function<void(PreviewId id, PreviewContext &ctx)>;

/**
 * @brief 预览管理器 — 固定池化方案
 *
 * 维护固定数量的预览槽位，切换资产时复用槽位而非创建/销毁，
 * 避免异步加载回调与上下文生命周期之间的时序冲突。
 */
class PreviewManager {
public:
    /// 预览池大小（Detail + Thumbnail 共享）
    static constexpr uint32_t POOL_SIZE = 4;

    PreviewManager() = default;
    ~PreviewManager() { Shutdown(); }

    PreviewManager(const PreviewManager &) = delete;
    PreviewManager &operator=(const PreviewManager &) = delete;

    void Initialize(ID3D12Device *device, DX12Engine::Renderer::D3D12DeviceContext *context, DX12Engine::Resource::RenderTargetPool *rtPool);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    /**
     * @brief 注册预览渲染回调（替代事件分发模式）
     */
    void SetRenderCallback(PreviewRenderCallback callback);

    /**
     * @brief 获取一个预览槽位（复用策略）
     * @param oldPreviewId 当前正在预览的 ID（0 表示无），该槽位会被优先复用
     * @param type PreviewType::Detail — 使用独立 RT；PreviewType::Thumbnail — 使用纹理数组 slice
     * @param width  宽度（Detail 有效）
     * @param height 高度（Detail 有效）
     * @return PreviewId 槽位 ID，失败返回 0
     *
     * 优先复用 oldPreviewId 所在的槽位，避免多槽位同时渲染。
     */
    PreviewId AcquirePreview(PreviewId oldPreviewId, PreviewType type = PreviewType::Detail,
                             uint32_t width = 256, uint32_t height = 256);

    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSRV(PreviewId id) const;
    PreviewContext *GetContext(PreviewId id);

    /// 遍历所有活跃上下文并调用渲染回调
    void RenderPreviews();

private:
    struct PreviewSlot {
        PreviewId id = 0;
        PreviewContext ctx;
        bool inUse = false;
    };

    ID3D12Device *m_device = nullptr;
    DX12Engine::Renderer::D3D12DeviceContext *m_context = nullptr;
    DX12Engine::Resource::RenderTargetPool *m_rtPool = nullptr;

    PreviewRenderCallback m_renderCallback;

    std::array<PreviewSlot, POOL_SIZE> m_slots;
    PreviewId m_nextId = 1;
    uint32_t m_nextSlot = 0; // 轮转索引，用于简单的 LRU 近似
    bool m_initialized = false;
};

