#pragma once

#include "Common/d3dUtil.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Resource/Struct/GeometryHandle.h"
#include <wrl/client.h>

namespace DX12Engine::Resource {
class GeometryResourceManager;
class MaterialManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

/**
 * @brief 反射探针离屏渲染器（GS 实例化方案）
 *
 * 职责：将场景渲染到反射探针的 Cubemap 中。
 * 使用 Geometry Shader 将每个三角形复制 6 份，每份通过
 * SV_RenderTargetArrayIndex 输出到不同的 Cubemap 面。
 * 一次 Draw 完成所有 6 面的渲染。
 *
 * 调用约定：
 *   1. System 在 Render Phase 中调用
 *   2. 每帧可捕获多个探针（逐探针调用 BeginCapture / EndCapture）
 *   3. GS 内部处理所有 6 个面
 */
class ReflectionProbeRenderer {
public:
    ReflectionProbeRenderer() = default;
    ~ReflectionProbeRenderer() = default;

    ReflectionProbeRenderer(const ReflectionProbeRenderer &) = delete;
    ReflectionProbeRenderer &operator=(const ReflectionProbeRenderer &) = delete;

    // ========================================================================
    // 生命周期
    // ========================================================================

    void SetDeviceContext(D3D12DeviceContext *context);
    void Initialize();
    void Shutdown();

    // ========================================================================
    // 依赖注入
    // ========================================================================

    void SetGeometryResourceManager(Resource::GeometryResourceManager *mgr) { m_geometryManager = mgr; }
    void SetMaterialManager(Resource::MaterialManager *mgr) { m_materialManager = mgr; }

    // ========================================================================
    // 探针捕获接口（GS 方案：一次 BeginCapture 覆盖 Cubemap 所有 6 面）
    // ========================================================================

    /// 开始捕获一个探针（GS 内部渲染所有 6 面到 Cubemap RTV）
    void BeginCapture(CommandList &cmdList, ID3D12Resource *cubemapResource, D3D12_CPU_DESCRIPTOR_HANDLE cubemapRTV,
                      D3D12_CPU_DESCRIPTOR_HANDLE depthDSV, uint32_t faceWidth, uint32_t faceHeight,
                      D3D12_GPU_VIRTUAL_ADDRESS captureCBAddress, D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress,
                      D3D12_GPU_DESCRIPTOR_HANDLE materialBufferSRV, D3D12_GPU_DESCRIPTOR_HANDLE textureHeapStart);

    /// 结束当前探针的捕获
    void EndCapture(CommandList &cmdList);

    // ========================================================================
    // 统一实例化绘制（单物体 instanceCount=1）
    // ========================================================================

    void DrawInstanced(CommandList &cmdList, Resource::GeometryHandle geometryHandle,
                       D3D12_GPU_VIRTUAL_ADDRESS instanceBufferAddress, uint32_t instanceCount);

private:
    // ========================================================================
    // 内部初始化
    // ========================================================================

    void LoadShaders();
    void CreateRootSignature();
    void CreatePSO();

    // ========================================================================
    // 成员变量
    // ========================================================================

    D3D12DeviceContext *m_context = nullptr;
    Resource::GeometryResourceManager *m_geometryManager = nullptr;
    Resource::MaterialManager *m_materialManager = nullptr;

    // 根签名 & PSO
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    // 着色器字节码
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_gsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;

    // 当前捕获状态
    bool m_inCapture = false;
    ID3D12Resource *m_captureResource = nullptr;
    uint32_t m_faceWidth = 0;
    uint32_t m_faceHeight = 0;
};

} // namespace DX12Engine::Renderer
