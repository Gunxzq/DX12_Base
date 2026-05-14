#pragma once

#include "Common/d3dUtil.h"
#include "Renderer/Core/Command/CommandList/CommandList.h"
#include "System/ECS/Registry.h"
#include "System/Resource/GpuResourceManager.h"
#include <array>
#include <memory>
#include <wrl/client.h>

namespace DX12Engine::ECS {
struct MeshComponent;
struct TransformComponent;
} // namespace DX12Engine::ECS

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

/**
 * @brief 不透明物体渲染器（引擎辅助层）
 *
 * 职责：
 * 1. 管理底层 D3D12 状态（PSO, RootSignature, CBV）
 * 2. 提供细粒度的渲染辅助方法供游戏层 System 调用
 */
class OpaqueRenderer {
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

    /**
     * @brief 开始录制不透明物体的绘制命令
     * @note 设置 PSO、根签名和全局常量缓冲区
     */
    void BeginFrame(CommandList &cmdList, uint32_t backBufferIndex);

    /**
     * @brief 绘制单个 Mesh
     * @param cmdList 当前命令列表
     * @param mesh ECS 网格组件
     * @param transform ECS 变换组件
     * @param backBufferIndex 当前帧索引
     */
    void DrawMesh(CommandList &cmdList, const DX12Engine::ECS::MeshComponent &mesh,
                  const DX12Engine::ECS::TransformComponent &transform, uint32_t backBufferIndex);

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
    void UpdateConstantBuffer(uint32_t backBufferIndex);

    // ========================================================================
    // 成员变量
    // ========================================================================

    D3D12DeviceContext *m_context = nullptr;

    // 根签名 & PSO
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    // 着色器字节码
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;

    // 常量缓冲区资源（每帧一个，用于三缓冲）
    struct FrameResources {
        Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer;
        void *mappedData = nullptr;
        uint64_t fenceValue = 0;
    };
    std::array<FrameResources, 3> m_frameResources;
    uint32_t m_currentFrameIndex = 0;

    // 投影矩阵（窗口 Resize 时更新）
    DirectX::XMMATRIX m_projectionMatrix;
};

} // namespace DX12Engine::Renderer