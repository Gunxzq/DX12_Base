#pragma once

#include "Renderer/FrameResources/RingBuffer.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include <DirectXMath.h>
#include <cstdint>
#include <d3d12.h>
#include <vector>

namespace DX12Engine::ECS {
class Registry; // Registry.h 中声明为 class DX12ECS_API Registry（前向声明须与定义一致）
} // namespace DX12Engine::ECS

namespace DX12Engine::Renderer {

// ========================================================================
// WaveParams — 单水体波浪参数
// ========================================================================
struct WaveParams {
    float amplitude = 0.5f;
    float frequency = 1.0f;
    float speed = 0.5f;
    float direction = 0.0f; // 风向（弧度）

    // 运行时状态（每帧由 Update 累加）
    DirectX::XMFLOAT2 waveOffset = {0, 0};
};

// ========================================================================
// WaterManager — 水管理器（单例）
//
// 对齐 LightManager 模式：
//   - 自管 RingBuffer（每帧 Upload 波浪常量）
//   - 波浪参数注册与更新
//   - 环境贴图引用（来自 SkyboxManager，水体共享）
// ========================================================================
class WaterManager {
public:
    static WaterManager &GetInstance();

    WaterManager(const WaterManager &) = delete;
    WaterManager &operator=(const WaterManager &) = delete;

    WaterManager() = default;
    ~WaterManager() = default;

    // ========================================================================
    // 生命周期
    // ========================================================================
    void Initialize(ID3D12Device *device);
    void Shutdown();

    // ========================================================================
    // 波浪参数管理
    // ========================================================================
    /// 注册波浪参数（保留旧接口兼容；后续由 CollectFromECS 替代）
    uint32_t RegisterWaveParams(const WaveParams &params);
    const WaveParams &GetWaveParams(uint32_t index) const;

    /// 从 ECS Registry 收集水体数据，重建 m_waveParams
    /// 遍历所有 WaterComponent 实体，从组件字段提取波浪参数
    /// 调用时机：每帧 UpdateAndUpload 之前
    void CollectFromECS(DX12Engine::ECS::Registry *registry);

    /// 更新波浪偏移 + 上传 WaterConstants CB（Immediate 回调调用）
    void UpdateAndUpload(uint64_t fence);

    /// 设置岸线渐隐距离（深度空间，>0 时 water.hlsl 采样场景深度做岸线渐隐）
    /// Game 端未绑定深度 SRV，保持默认 0 自动降级为纯色水
    void SetFadeRange(float range) { m_fadeRange = range; }

    /// 设置世界 UV 平铺（worldPos.xz * UVTiling——纹理跨块连续，对齐波形世界坐标）
    void SetUVTiling(float tiling) { m_uvTiling = tiling; }

    // ========================================================================
    // 环境贴图（来自 SkyboxManager，SceneConstructor 注入）
    // ========================================================================
    void SetEnvironmentMap(D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV);
    D3D12_GPU_DESCRIPTOR_HANDLE GetEnvironmentMap() const { return m_envMapSRV; }
    bool HasEnvironmentMap() const { return m_envMapSRV.ptr != 0; }

    // ========================================================================
    // 数据访问（供 WaterRenderSystem 使用）
    // ========================================================================
    D3D12_GPU_VIRTUAL_ADDRESS GetWaterCBAddress() const { return m_waterCBAddress; }

private:
    bool m_initialized = false;
    ID3D12Device *m_device = nullptr;

    std::vector<WaveParams> m_waveParams; // waveParamIndex → params
    D3D12_GPU_DESCRIPTOR_HANDLE m_envMapSRV = {};

    // 自管 RingBuffer（每帧上传 WaterConstants）
    RingBuffer m_waterBuffer;
    D3D12_GPU_VIRTUAL_ADDRESS m_waterCBAddress = 0;

    // 岸线渐隐距离（深度空间，0=禁用降级；Editor 端设置，Game 端保持 0）
    float m_fadeRange = 0.0f;

    // 世界 UV 平铺（water.hlsl worldPos.xz * gUVTiling；默认 1.0 = 每世界单位一个纹理周期）
    float m_uvTiling = 1.0f;
};

} // namespace DX12Engine::Renderer
