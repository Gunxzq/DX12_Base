#pragma once

#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include <d3d12.h>
#include <vector>

namespace DX12Engine::Renderer {

// ============================================================================
// 光源管理器 - 管理场景中的静态光源数据
// ============================================================================
// 职责：
//   1. 存储场景中的光源数据（方向光、点光源、聚光灯）
//   2. 每帧将光源数据打包并上传到 GPU
//   3. 类似 CameraManager 的架构，用于管理全局光照
// ============================================================================

class LightManager {
public:
    static LightManager &GetInstance();

    LightManager(const LightManager &) = delete;
    LightManager &operator=(const LightManager &) = delete;
    LightManager() = default;
    ~LightManager() = default;

    // ========================================================================
    // 生命周期
    // ========================================================================

    void Initialize();
    void Shutdown();

    // ========================================================================
    // 光源设置
    // ========================================================================

    /**
     * @brief 清除所有光源
     */
    void Clear();

    /**
     * @brief 设置方向光（通常只有一个）
     * @param light 光源数据
     * @param index 方向光索引（0-255）
     */
    void SetDirectionalLight(const Light &light, uint32_t index = 0);

    /**
     * @brief 添加点光源
     * @param light 光源数据
     * @return 添加的光源索引
     */
    uint32_t AddPointLight(const Light &light);

    /**
     * @brief 添加聚光灯
     * @param light 光源数据
     * @return 添加的光源索引
     */
    uint32_t AddSpotLight(const Light &light);

    /**
     * @brief 修改点光源
     * @param index 光源索引
     * @param light 光源数据
     */
    void SetPointLight(uint32_t index, const Light &light);

    /**
     * @brief 修改聚光灯
     * @param index 光源索引
     * @param light 光源数据
     */
    void SetSpotLight(uint32_t index, const Light &light);

    /**
     * @brief 移除点光源
     * @param index 光源索引
     */
    void RemovePointLight(uint32_t index);

    /**
     * @brief 移除聚光灯
     * @param index 光源索引
     */
    void RemoveSpotLight(uint32_t index);

    /**
     * @brief 设置环境光
     * @param ambient 环境光颜色和强度（RGB + Intensity）
     */
    void SetAmbientLight(const DirectX::XMFLOAT4 &ambient);

    // ========================================================================
    // 数据访问
    // ========================================================================

    /**
     * @brief 获取光源常量数据（只读）
     */
    const LightConstants &GetLightConstants() const { return m_lightConstants; }

    /**
     * @brief 获取光源常量数据（可写）
     */
    LightConstants &GetLightConstants() { return m_lightConstants; }

    /**
     * @brief 获取方向光数量
     */
    uint32_t GetDirectionalLightCount() const { return m_lightConstants.NumDirLights; }

    /**
     * @brief 获取点光源数量
     */
    uint32_t GetPointLightCount() const { return m_lightConstants.NumPointLights; }

    /**
     * @brief 获取聚光灯数量
     */
    uint32_t GetSpotLightCount() const { return m_lightConstants.NumSpotLights; }

    /**
     * @brief 获取指定方向光
     */
    Light *GetDirectionalLight(uint32_t index = 0);

    /**
     * @brief 获取指定点光源
     */
    Light *GetPointLight(uint32_t index);

    /**
     * @brief 获取指定聚光灯
     */
    Light *GetSpotLight(uint32_t index);

    // ========================================================================
    // GPU 上传
    // ========================================================================

    /**
     * @brief 更新并上传光源数据到 GPU
     * @param frameResourceManager 帧资源管理器
     * @return GPU 虚拟地址
     */
    D3D12_GPU_VIRTUAL_ADDRESS UpdateAndUpload(FrameResourceManager *frameResourceManager);

    /**
     * @brief 获取当前光源常量缓冲的 GPU 地址
     */
    D3D12_GPU_VIRTUAL_ADDRESS GetLightCBAddress() const { return m_lightCBAddress; }

    // ========================================================================
    // 调试辅助
    // ========================================================================

    /**
     * @brief 创建测试光源（用于快速调试）
     */
    void CreateTestLights();

private:
    // 重建光源数组（用于维护连续性）
    void RebuildLights();

private:
    LightConstants m_lightConstants = {};           // 光源常量数据
    D3D12_GPU_VIRTUAL_ADDRESS m_lightCBAddress = 0; // GPU 地址

    // 用于管理动态光源的分离存储（可选）
    std::vector<Light> m_pointLights;
    std::vector<Light> m_spotLights;

    bool m_dirty = false; // 标记数据是否变化，需要重建数组
};

} // namespace DX12Engine::Renderer