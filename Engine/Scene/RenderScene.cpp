#include "RenderScene.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Renderer/Scene/ReflectionProbeManager/ReflectionProbeManager.h"
#include "Renderer/Effects/AO/AmbientOcclusionManager.h"

using namespace DX12Engine::Renderer;

namespace DX12Engine::Scene {

// ========================================================================
// 场景切换
// ========================================================================

void RenderScene::OnScenePreUnload() {
    // 场景卸载前：驱动各管理器清除旧场景数据
    // 管理器保持单例，差异化更新而非销毁重建
    if (m_lightMgr) {
        m_lightMgr->Clear();
    }
    if (m_reflectionProbeMgr) {
        m_reflectionProbeMgr->Shutdown();
    }
    // AmbientOcclusionManager 的 RT 由 OnResize 管理，场景切换时无需清除
}

} // namespace DX12Engine::Scene