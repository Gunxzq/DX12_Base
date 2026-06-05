// OffscreenRenderer.h
#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

namespace DX12Engine::Renderer {

class CommandList;
class D3D12DeviceContext;

// ============================================================================
// 离屏渲染器基类
//
// 语义：输出到纹理而不是 BackBuffer，用于阴影贴图、鸟瞰图、反射探针等
// ============================================================================
class OffscreenRenderer {
public:
    virtual ~OffscreenRenderer() = default;

    // ========================================================================
    // 生命周期
    // ========================================================================

    virtual void SetDeviceContext(D3D12DeviceContext *context) = 0;
    virtual void Initialize() = 0;
    virtual void Shutdown() = 0;

    // ========================================================================
    // 离屏渲染核心接口
    // ========================================================================

    // 开始离屏渲染（设置 RTV/DSV、清除、设置视口）
    // 调用前：资源处于 SRV 状态
    // 调用后：资源处于 RENDER_TARGET / DEPTH_WRITE 状态
    virtual void BeginOffscreen(CommandList &cmdList) = 0;

    // 结束离屏渲染（可选：转换资源状态回 SRV）
    virtual void EndOffscreen(CommandList &cmdList) = 0;

    // ========================================================================
    // 输出纹理访问
    // ========================================================================

    // 获取输出纹理的 SRV（供其他渲染器/UI 采样）
    virtual D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSRV() const = 0;

    // 获取输出纹理的 RTV（用于 Clear 等操作）
    virtual D3D12_CPU_DESCRIPTOR_HANDLE GetOutputRTV() const = 0;

    // 获取深度缓冲的 DSV（如果有）
    virtual D3D12_CPU_DESCRIPTOR_HANDLE GetDepthDSV() const = 0;

    // ========================================================================
    // 纹理信息
    // ========================================================================

    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;
    virtual bool IsValid() const = 0;

    // 重置离屏资源（当尺寸变化时）
    virtual void Resize(uint32_t width, uint32_t height) = 0;
};

} // namespace DX12Engine::Renderer