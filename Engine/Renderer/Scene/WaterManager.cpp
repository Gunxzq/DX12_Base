#include "WaterManager.h"
#include "Resource/GpuResourceManager.h"
#include <cmath>

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

static constexpr uint32_t WATER_BUFFER_SIZE = 64 * 1024; // 64KB

WaterManager &WaterManager::GetInstance() {
    static WaterManager instance;
    return instance;
}

void WaterManager::Initialize(ID3D12Device *device) {
    m_device = device;
    m_waveParams.clear();
    m_envMapSRV = {};

    // 初始化内部 RingBuffer
    m_waterBuffer.Initialize(device, WATER_BUFFER_SIZE, L"WaterManager_Buffer");
    m_initialized = true;
}

void WaterManager::Shutdown() {
    m_waveParams.clear();
    m_envMapSRV = {};
    m_waterBuffer.Shutdown();
    m_waterCBAddress = 0;
    m_device = nullptr;
    m_initialized = false;
}

uint32_t WaterManager::RegisterWaveParams(const WaveParams &params) {
    m_waveParams.push_back(params);
    return static_cast<uint32_t>(m_waveParams.size() - 1);
}

const WaveParams &WaterManager::GetWaveParams(uint32_t index) const {
    static WaveParams defaultParams;
    if (index >= m_waveParams.size())
        return defaultParams;
    return m_waveParams[index];
}

void WaterManager::UpdateAndUpload(uint64_t fence) {
    if (!m_initialized)
        return;

    // 更新波浪偏移
    float deltaTime = 1.0f / 60.0f; // 近似值，后续从 GameTimer 传入更精确值
    for (auto &wp : m_waveParams) {
        float dx = cosf(wp.direction) * wp.speed * deltaTime;
        float dy = sinf(wp.direction) * wp.speed * deltaTime;
        wp.waveOffset.x += dx;
        wp.waveOffset.y += dy;
    }

    // 填充 WaterConstants
    WaterConstants waterCB = {};
    waterCB.Time = 0; // TODO: 从外部传入精确时间
    waterCB.WaveAmplitude = 0.5f;
    waterCB.WaveSpeed = 1.5f;
    waterCB.WaveFrequency = 2.0f;
    waterCB.RefractionStrength = 0.3f;
    waterCB.FresnelPower = 2.0f;
    waterCB.FoamIntensity = 0.5f;

    // 上传到 RingBuffer
    m_waterCBAddress = m_waterBuffer.AllocateUpload(&waterCB, sizeof(WaterConstants), fence);
    if (m_waterCBAddress == 0) {
        // 分配失败，扩张 RingBuffer
        m_waterBuffer.Initialize(m_device, WATER_BUFFER_SIZE * 2, L"WaterManager_Buffer");
        m_waterCBAddress = m_waterBuffer.AllocateUpload(&waterCB, sizeof(WaterConstants), fence);
    }
}

void WaterManager::SetEnvironmentMap(D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV) { m_envMapSRV = envMapSRV; }

} // namespace DX12Engine::Renderer
