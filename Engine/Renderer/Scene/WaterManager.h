#pragma once

#include "Renderer/FrameResources/RingBuffer.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include <DirectXMath.h>
#include <cstdint>
#include <d3d12.h>
#include <vector>

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
    uint32_t RegisterWaveParams(const WaveParams &params);
    const WaveParams &GetWaveParams(uint32_t index) const;

    /// 更新波浪偏移 + 上传 WaterConstants CB（Immediate 回调调用）
    void UpdateAndUpload(uint64_t fence);

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
};

} // namespace DX12Engine::Renderer
