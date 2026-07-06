#pragma once

#include "Resource/Pool/DepthStencilPool.h"
#include "Resource/Pool/RenderTargetPool.h"
#include "Resource/Core/GpuHandlePool.h"
#include <DirectXMath.h>
#include <vector>

namespace DX12Engine {

namespace Resource {
class DescriptorHeapCollection;
} // namespace Resource

namespace Renderer {

struct ProbeRuntimeResources {
    Resource::RenderTargetHandle rtHandle; // RTV池句柄（包含资源 + RTV槽）
    uint32_t srvSlot = UINT32_MAX;         // Cubemap SRV 槽位（6 个面）
    uint32_t resolution = 0;               // 探针分辨率（必须为 2 的幂）
    bool isValid = false;
};

class ReflectionProbeManager {
public:
    ReflectionProbeManager() = default;
    ~ReflectionProbeManager() = default;

    ReflectionProbeManager(const ReflectionProbeManager &) = delete;
    ReflectionProbeManager &operator=(const ReflectionProbeManager &) = delete;

    // ---- 生命周期 ----
    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descriptorHeaps);
    void Shutdown();

    // ---- 探针管理 ----
    uint32_t AddProbe(const DirectX::XMFLOAT3 &position, float captureRange = 50.0f, uint32_t resolution = 256,
                      uint8_t updatePriority = 1);
    void RemoveProbe(uint32_t probeIndex);
    void UpdateProbePosition(uint32_t probeIndex, const DirectX::XMFLOAT3 &position);
    void SetProbePriority(uint32_t probeIndex, uint8_t priority);
    void SetProbeCaptureRange(uint32_t probeIndex, float range);

    // ---- 每帧更新（在 PrePass 阶段调用） ----
    void Update(float deltaTime, uint32_t frameCounter);

    // ---- 查询（CPU 端绑定使用，线程安全：只读） ----
    uint32_t FindClosestProbe(const DirectX::XMFLOAT3 &position) const;
    const ProbeRuntimeResources &GetProbeResources(uint32_t probeIndex) const;
    DirectX::XMFLOAT3 GetProbePosition(uint32_t probeIndex) const;
    float GetProbeCaptureRange(uint32_t probeIndex) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetProbeCubemapArraySRV() const { return m_cubemapArraySRV; }
    uint32_t GetActiveProbeCount() const { return static_cast<uint32_t>(m_probeEntries.size()); }

    /// 获取指定探针使用的深度缓冲区 DSV 槽位
    uint32_t GetProbeDepthSlot(uint32_t probeIndex) const;

private:
    // ---- 内部数据结构 ----
    struct ProbeEntry {
        DirectX::XMFLOAT3 position;
        float captureRange;
        uint32_t resolution;
        uint8_t updatePriority;
        ProbeRuntimeResources resources;
        Resource::DepthStencilHandle depthHandle;
        uint8_t updateCounter;
        bool needsCapture;
        bool isActive;
    };

    // ---- 内部辅助 ----
    bool ShouldUpdateProbe(const ProbeEntry &entry, uint32_t frameCounter) const;

    ProbeRuntimeResources AllocateCubemapResource(uint32_t resolution);
    void ReleaseCubemapResource(ProbeRuntimeResources &resources, uint64_t fence);

    void CaptureProbe(ProbeEntry &entry);

private:
    ID3D12Device *m_device = nullptr;
    Resource::DescriptorHeapCollection *m_descriptorHeaps = nullptr;
    bool m_initialized = false;

    // 探针数据
    std::vector<ProbeEntry> m_probeEntries;

    // Cubemap 数组 SRV
    D3D12_GPU_DESCRIPTOR_HANDLE m_cubemapArraySRV = {};
    uint32_t m_cubemapArrayBaseSlot = UINT32_MAX;

    // 帧计数器
    uint32_t m_frameCounter = 0;
};

} // namespace Renderer
} // namespace DX12Engine
