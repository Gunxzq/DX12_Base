#pragma once

#include "Common/d3dUtil.h"
#include "ECS/Core/Registry.h"
#include "IRenderer.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Struct/GeometryHandle.h"
#include <array>
#include <memory>
#include <wrl/client.h>

namespace DX12Engine::ECS {
struct MeshComponent;
struct TransformComponent;
} // namespace DX12Engine::ECS

namespace DX12Engine::Resource {
class GeometryResourceManager;
class MaterialManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

/**
 * @brief 不透明物体渲染器（引擎辅助层）
 *
 * 职责：
 * 1. 管理底层 D3D12 状态（PSO, RootSignature, CBV）
 * 2. 提供细粒度的渲染辅助方法供游戏层 System 调用
 */
class OpaqueRenderer : public IRenderer {
public:
    OpaqueRenderer() = default;
    ~OpaqueRenderer() = default;

    // ========================================================================
    // 生命周期管理
    // ========================================================================

    void SetDeviceContext(D3D12DeviceContext *context);
    void Initialize();
    void OnResize(uint32_t width, uint32_t height);
    void Update(float deltaTime);

    // ========================================================================
    // 渲染辅助接口（供游戏层 System 调用）
    // ========================================================================

    void SetGeometryResourceManager(Resource::GeometryResourceManager *mgr) { m_geometryManager = mgr; }
    void SetMaterialManager(Resource::MaterialManager *mgr) { m_materialManager = mgr; }

    /**
     * @brief 开始录制不透明物体的绘制命令
     * @param cmdList 当前命令列表
     * @param passConstantsAddress 由上层（Game/Immediate Callback）计算并上传好的 PassConstants GPU 地址
     * @note 设置 PSO、根签名，并绑定 Pass Constant Buffer (b1)
     */
    void BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                    D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress);

    /**
     * @brief 绘制单个 Mesh
     * @param cmdList 当前命令列表
     * @param mesh ECS 网格组件
     * @param transform ECS 变换组件
     */
    void DrawMesh(CommandList &cmdList, DX12Engine::Resource::GeometryHandle geometryHandle,
                  const DirectX::XMMATRIX &worldMatrix, D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress,
                  D3D12_GPU_VIRTUAL_ADDRESS matCBAddress);

    /**
     * @brief 结束录制
     */
    void EndFrame();

private:
    // ========================================================================
    // 内部初始化
    // ========================================================================

    void CreateRootSignature();
    void CreatePSO();
    void LoadShaders();

    // ========================================================================
    // 成员变量
    // ========================================================================

    D3D12DeviceContext *m_context = nullptr;

    // 根签名 & PSO
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    Resource::GeometryResourceManager *m_geometryManager = nullptr;
    Resource::MaterialManager *m_materialManager = nullptr;

    // 着色器字节码
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;

    // 投影矩阵（窗口 Resize 时更新）
    DirectX::XMMATRIX m_projectionMatrix;
};

} // namespace DX12Engine::Renderer