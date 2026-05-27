#include "LightManager.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include <cstring>

using namespace DX12Engine::Renderer;

namespace DX12Engine::Renderer {

// ============================================================================
// 单例实现
// ============================================================================

LightManager &LightManager::GetInstance() {
    static LightManager instance;
    return instance;
}

// ============================================================================
// 生命周期
// ============================================================================

void LightManager::Initialize() {
    Clear();
    m_lightCBAddress = 0;
    m_dirty = false;
    m_pointLights.clear();
    m_spotLights.clear();
}

void LightManager::Shutdown() { m_lightCBAddress = 0; }

// ============================================================================
// 光源设置
// ============================================================================

void LightManager::Clear() {
    memset(&m_lightConstants, 0, sizeof(LightConstants));
    m_pointLights.clear();
    m_spotLights.clear();
    m_dirty = true;
}

void LightManager::SetDirectionalLight(const Light &light, uint32_t index) {
    if (index < 256) {
        // 确保索引不超过已有数量
        if (index >= m_lightConstants.NumDirLights) {
            m_lightConstants.NumDirLights = index + 1;
        }
        m_lightConstants.Lights[index] = light;
    }
}

uint32_t LightManager::AddPointLight(const Light &light) {
    uint32_t index = static_cast<uint32_t>(m_pointLights.size());
    m_pointLights.push_back(light);
    m_dirty = true;
    return index;
}

uint32_t LightManager::AddSpotLight(const Light &light) {
    uint32_t index = static_cast<uint32_t>(m_spotLights.size());
    m_spotLights.push_back(light);
    m_dirty = true;
    return index;
}

void LightManager::SetPointLight(uint32_t index, const Light &light) {
    if (index < m_pointLights.size()) {
        m_pointLights[index] = light;
        m_dirty = true;
    }
}

void LightManager::SetSpotLight(uint32_t index, const Light &light) {
    if (index < m_spotLights.size()) {
        m_spotLights[index] = light;
        m_dirty = true;
    }
}

void LightManager::RemovePointLight(uint32_t index) {
    if (index < m_pointLights.size()) {
        m_pointLights.erase(m_pointLights.begin() + index);
        m_dirty = true;
    }
}

void LightManager::RemoveSpotLight(uint32_t index) {
    if (index < m_spotLights.size()) {
        m_spotLights.erase(m_spotLights.begin() + index);
        m_dirty = true;
    }
}

void LightManager::SetAmbientLight(const DirectX::XMFLOAT4 &ambient) {
    // 环境光存储在 PassConstants 中，不在 LightConstants 里
    // 这个方法只是占位，实际环境光由调用方设置到 PassConstants
    // 保留此接口以便未来统一管理
}

// ============================================================================
// 数据访问
// ============================================================================

Light *LightManager::GetDirectionalLight(uint32_t index) {
    if (index < m_lightConstants.NumDirLights) {
        return &m_lightConstants.Lights[index];
    }
    return nullptr;
}

Light *LightManager::GetPointLight(uint32_t index) {
    if (index < m_pointLights.size()) {
        return &m_pointLights[index];
    }
    return nullptr;
}

Light *LightManager::GetSpotLight(uint32_t index) {
    if (index < m_spotLights.size()) {
        return &m_spotLights[index];
    }
    return nullptr;
}

// ============================================================================
// GPU 上传
// ============================================================================

void LightManager::RebuildLights() {
    // 重建光源数组：方向光 + 点光源 + 聚光灯
    // 方向光已经在 m_lightConstants.Lights[0..NumDirLights-1] 中
    uint32_t offset = m_lightConstants.NumDirLights;

    // 复制点光源
    for (size_t i = 0; i < m_pointLights.size() && offset + i < 256; ++i) {
        m_lightConstants.Lights[offset + i] = m_pointLights[i];
    }
    m_lightConstants.NumPointLights = static_cast<uint32_t>(m_pointLights.size());

    offset += m_lightConstants.NumPointLights;

    // 复制聚光灯
    for (size_t i = 0; i < m_spotLights.size() && offset + i < 256; ++i) {
        m_lightConstants.Lights[offset + i] = m_spotLights[i];
    }
    m_lightConstants.NumSpotLights = static_cast<uint32_t>(m_spotLights.size());

    m_dirty = false;
}

D3D12_GPU_VIRTUAL_ADDRESS LightManager::UpdateAndUpload(FrameResourceManager *frameResourceManager) {
    if (!frameResourceManager) {
        return 0;
    }

    // 如果光源数据有变化，重建数组
    if (m_dirty) {
        RebuildLights();
    }

    // 上传到 GPU
    m_lightCBAddress = frameResourceManager->AllocateLight(&m_lightConstants, sizeof(LightConstants));
    return m_lightCBAddress;
}

// ============================================================================
// 调试辅助
// ============================================================================

void LightManager::CreateTestLights() {
    Clear();

    // 方向光 0（主光）- 增强强度
    Light dirLight;
    dirLight.Strength = DirectX::XMFLOAT4(1.5f, 1.2f, 1.0f, 0.0f); // 增强强度
    dirLight.Direction = DirectX::XMFLOAT4(0.57735f, -0.57735f, 0.57735f, 0.0f);
    SetDirectionalLight(dirLight, 0);

    // 添加一个暖色点光源（右前方）- 增强
    Light pointLight0;
    pointLight0.Strength = DirectX::XMFLOAT4(2.0f, 1.0f, 0.5f, 0.0f); // 增强并变暖
    pointLight0.Position = DirectX::XMFLOAT4(3.0f, 2.0f, 4.0f, 0.0f);
    pointLight0.FalloffStart = 1.0f;
    pointLight0.FalloffEnd = 20.0f;
    pointLight0.Range = 20.0f;
    AddPointLight(pointLight0);

    // 添加一个冷色点光源（左前方）- 增强
    Light pointLight1;
    pointLight1.Strength = DirectX::XMFLOAT4(0.5f, 1.0f, 2.0f, 0.0f); // 增强并变冷
    pointLight1.Position = DirectX::XMFLOAT4(-3.0f, 2.0f, 4.0f, 0.0f);
    pointLight1.FalloffStart = 1.0f;
    pointLight1.FalloffEnd = 20.0f;
    pointLight1.Range = 20.0f;
    AddPointLight(pointLight1);

    // 添加一个背光补光（增强边缘光）
    Light backLight;
    backLight.Strength = DirectX::XMFLOAT4(1.0f, 1.0f, 1.5f, 0.0f);
    backLight.Position = DirectX::XMFLOAT4(0.0f, 3.0f, -5.0f, 0.0f);
    backLight.FalloffStart = 1.0f;
    backLight.FalloffEnd = 15.0f;
    backLight.Range = 15.0f;
    AddPointLight(backLight);

    // 跟随相机的光源（保持原有）
    Light followLight;
    followLight.Strength = DirectX::XMFLOAT4(1.2f, 1.2f, 1.5f, 0.0f);
    followLight.FalloffStart = 0.5f;
    followLight.FalloffEnd = 10.0f;
    followLight.Range = 10.0f;
    AddPointLight(followLight);
}

} // namespace DX12Engine::Renderer