#pragma once

#include "Resource/Core/DescriptorHeapCollection.h"
#include <d3d12.h>

namespace DX12Engine::Renderer {
class D3D12DeviceContext;
class LightManager;
class ReflectionProbeManager;
class AmbientOcclusionManager;
} // namespace DX12Engine::Renderer

namespace DX12Engine::Scene {

// ========================================================================
// RenderScene — 渲染上下文容器
//
// 逻辑上聚合、实际上离散的访问窗口。
// 不持有管理器 ownership，只持有引用，管理器保持单例不变。
// 共享基础设施（DescriptorHeaps、DeviceContext）由 RenderScene 统一持有，
// 消费者通过此 Scope 获取，不再各自保存指针。
// ========================================================================

class RenderScene {
public:
    RenderScene() = default;
    ~RenderScene() = default;

    RenderScene(const RenderScene &) = delete;
    RenderScene &operator=(const RenderScene &) = delete;

    // ====================================================================
    // 场景切换
    // ====================================================================

    /// 场景卸载前：驱动各管理器清除旧场景数据
    void OnScenePreUnload();

    // ====================================================================
    // 依赖注入：设置管理器引用和共享基础设施
    // ====================================================================

    void SetLightManager(Renderer::LightManager *mgr) { m_lightMgr = mgr; }
    void SetReflectionProbeManager(Renderer::ReflectionProbeManager *mgr) { m_reflectionProbeMgr = mgr; }
    void SetAmbientOcclusionManager(Renderer::AmbientOcclusionManager *mgr) { m_aoMgr = mgr; }
    void SetDescriptorHeaps(Resource::DescriptorHeapCollection *heaps) { m_descHeaps = heaps; }
    void SetDeviceContext(Renderer::D3D12DeviceContext *ctx) { m_deviceContext = ctx; }

    // ====================================================================
    // 访问器
    // ====================================================================

    Renderer::LightManager *GetLightManager() const { return m_lightMgr; }
    Renderer::ReflectionProbeManager *GetReflectionProbeManager() const { return m_reflectionProbeMgr; }
    Renderer::AmbientOcclusionManager *GetAmbientOcclusionManager() const { return m_aoMgr; }
    Resource::DescriptorHeapCollection *GetDescriptorHeaps() const { return m_descHeaps; }
    Renderer::D3D12DeviceContext *GetDeviceContext() const { return m_deviceContext; }

private:
    Renderer::LightManager *m_lightMgr = nullptr;
    Renderer::ReflectionProbeManager *m_reflectionProbeMgr = nullptr;
    Renderer::AmbientOcclusionManager *m_aoMgr = nullptr;
    Resource::DescriptorHeapCollection *m_descHeaps = nullptr;
    Renderer::D3D12DeviceContext *m_deviceContext = nullptr;
};

} // namespace DX12Engine::Scene