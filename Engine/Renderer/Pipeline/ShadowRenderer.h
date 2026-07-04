#pragma once

#include "Common/d3dUtil.h"
#include "OffscreenRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Resource/Struct/GeometryHandle.h"
#include <wrl/client.h>

namespace DX12Engine::Resource {
class GeometryResourceManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

// ============================================================================
// 阴影贴图渲染器 — 继承自 OffscreenRenderer
// ============================================================================
class ShadowRenderer : public OffscreenRenderer {
public:
    ShadowRenderer() = default;
    ~ShadowRenderer() override = default;

    // ========================================================================
    // OffscreenRenderer 接口 — 生命周期
    // ========================================================================
    void SetDeviceContext(D3D12DeviceContext *context) override;
    void Initialize() override;
    void Shutdown() override;

    // ========================================================================
    // OffscreenRenderer 接口 — 离屏渲染核心
    // ========================================================================

    // 基类纯虚接口实现（阴影渲染器需额外参数，通过 SetShadowPassParams 设置）
    void BeginOffscreen(CommandList &cmdList) override;

    // 开始离屏阴影 Pass（设置 DSV、清除深度、设置视口/裁剪、绑定 PSO）
    void BeginOffscreen(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle, uint32_t width, uint32_t height);

    // 结束离屏阴影 Pass
    void EndOffscreen(CommandList &cmdList) override;

    // 预设阴影 Pass 参数
    void SetShadowPassParams(D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle,
                             uint32_t width, uint32_t height);

    // ========================================================================
    // OffscreenRenderer 接口 — 输出纹理访问
    // ========================================================================
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSRV() const override;
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputRTV() const override;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDepthDSV() const override;

    // ========================================================================
    // OffscreenRenderer 接口 — 纹理信息
    // ========================================================================
    uint32_t GetWidth() const override { return m_passWidth; }
    uint32_t GetHeight() const override { return m_passHeight; }
    bool IsValid() const override { return m_pso && m_rootSignature; }

    void Resize(uint32_t width, uint32_t height) override;

    // ========================================================================
    // 依赖注入
    // ========================================================================
    void SetGeometryResourceManager(Resource::GeometryResourceManager *mgr) { m_geometryManager = mgr; }

    // ========================================================================
    // 阴影绘制接口（统一实例化模式，单物体 instanceCount=1）
    // [TODO] View Instancing：后续将点光源阴影切换到 SV_ViewID 方式，
    //         m_pointInstancedPSO 改为单个 PSO + D3D12_VIEW_INSTANCING 描述，
    //         VS 通过 SV_ViewID 选择面 VP 矩阵。与曲面细分兼容。
    // ========================================================================
    void DrawInstanced(CommandList &cmdList, Resource::GeometryHandle geometryHandle,
                       D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress, uint32_t instanceCount);

    // 支持子网格偏移的实例化绘制
    void DrawIndexedInstancedSubmesh(CommandList &cmdList, Resource::GeometryHandle geometryHandle,
                                     D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress, uint32_t instanceCount,
                                     uint32_t startIndex, int32_t startVertex, uint32_t indexCount);

    // 获取 PSO（供外部调试）
    ID3D12PipelineState *GetPSO() const { return m_pso.Get(); }
    ID3D12RootSignature *GetRootSignature() const { return m_rootSignature.Get(); }
    ID3D12PipelineState *GetPointInstancedPSO() const { return m_pointInstancedPSO.Get(); }
    ID3D12PipelineState *GetPointGSPSO() const { return m_pointGSPSO.Get(); }
    ID3D12PipelineState *GetSpotPSO() const { return m_spotPSO.Get(); }

    // 内部状态访问（供 PointShadowRenderSystem 使用）
    bool IsInPass() const { return m_inPass; }
    void SetInPass(bool v) { m_inPass = v; }

    // ========================================================================
    // 便利方法：一站式执行完整阴影 Pass
    // ========================================================================
    template <typename DrawFunc>
    void ExecuteShadowPass(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                           D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle, uint32_t width, uint32_t height,
                           DrawFunc &&drawFunc) {
        BeginOffscreen(cmdList, lightCBAddress, dsvHandle, width, height);
        drawFunc();
        EndOffscreen(cmdList);
    }

private:
    // ========================================================================
    // 内部初始化
    // ========================================================================
    void LoadShaders();
    void CreateRootSignature();
    void CreatePSO();
    void LoadPointInstancedShaders();
    void CreatePointInstancedPSO();
    void LoadPointGSShaders();
    void CreatePointGSPSO();
    void LoadSpotShaders();
    void CreateSpotPSO();

    // ========================================================================
    // 成员变量
    // ========================================================================
    D3D12DeviceContext *m_context = nullptr;
    Resource::GeometryResourceManager *m_geometryManager = nullptr;

    // 方向光阴影根签名 & PSO
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;

    // 点光源阴影（实例化，单面）
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pointInstancedPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_pointInstancedVSBlob;

    // 点光源阴影（GS 展开）
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pointGSPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_pointGSVSBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_pointGSGSBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_pointGSPSBlob;

    // 聚光灯阴影（实例化）
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_spotPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> m_spotVSBlob;

    // 当前 Pass 状态
    bool m_inPass = false;

    // 当前离屏渲染参数
    uint32_t m_passWidth = 0;
    uint32_t m_passHeight = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE m_currentDsvHandle = {};

    // 缓存的阴影 Pass 参数
    D3D12_GPU_VIRTUAL_ADDRESS m_cachedLightCBAddress = 0;
};

} // namespace DX12Engine::Renderer
