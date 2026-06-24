#pragma once

#include "Resource/Struct/ResourceHandle.h"
#include "Resource/Struct/TextureHandle.h"
#include <DirectXMath.h>
#include <unordered_map>
#include <vector>

namespace DX12Engine {

namespace Resource {
class DescriptorHeapCollection;
class TextureManager;
} // namespace Resource

namespace Renderer {

struct ProbeRuntimeResources {
    Resource::TextureHandle cubemapHandle;
    uint32_t srvSlot = UINT32_MAX;
    uint32_t resolution = 0;
    bool isValid = false;
};

class ReflectionProbeManager {
public:
    ReflectionProbeManager() = default;
    ~ReflectionProbeManager() = default;

    ReflectionProbeManager(const ReflectionProbeManager &) = delete;
    ReflectionProbeManager &operator=(const ReflectionProbeManager &) = delete;

    // ---- 生命周期 ----
    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descriptorHeaps,
                    Resource::TextureManager *textureManager);
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
    D3D12_GPU_DESCRIPTOR_HANDLE GetProbeCubemapArraySRV() const { return m_cubemapArraySRV; }
    uint32_t GetActiveProbeCount() const { return static_cast<uint32_t>(m_probeEntries.size()); }

    /// 获取指定探针使用的深度缓冲区 DSV 槽位（同尺寸探针共享）
    uint32_t GetProbeDepthSlot(uint32_t probeIndex) const;

private:
    // ---- 内部数据结构 ----
    struct ProbeEntry {
        DirectX::XMFLOAT3 position;
        float captureRange;
        uint32_t resolution;
        uint8_t updatePriority;
        ProbeRuntimeResources resources;
        uint8_t updateCounter;
        bool needsCapture;
        bool isActive;
    };

    // ---- 驻留式深度资源（按分辨率共享） ----
    struct ProbeDepthResource {
        Resource::GpuResourceHandle gpuHandle;
        uint32_t dsvSlot = UINT32_MAX;
        uint32_t resolution = 0;
        uint32_t refCount = 0;
    };

    uint32_t AcquireDepthResource(uint32_t resolution);
    void ReleaseDepthResource(uint32_t resolution);

    // ---- 内部辅助 ----
    bool ShouldUpdateProbe(const ProbeEntry &entry, uint32_t frameCounter) const;
    ProbeRuntimeResources AllocateCubemapResource(uint32_t resolution);
    void ReleaseCubemapResource(ProbeRuntimeResources &resources, uint64_t fence);
    void CaptureProbe(ProbeEntry &entry);

private:
    ID3D12Device *m_device = nullptr;
    Resource::DescriptorHeapCollection *m_descriptorHeaps = nullptr;
    Resource::TextureManager *m_textureManager = nullptr;
    bool m_initialized = false;

    // 探针数据
    std::vector<ProbeEntry> m_probeEntries;

    // 驻留深度资源池（key = resolution）
    std::unordered_map<uint32_t, ProbeDepthResource> m_depthPool;

    // Cubemap 数组 SRV
    D3D12_GPU_DESCRIPTOR_HANDLE m_cubemapArraySRV = {};
    uint32_t m_cubemapArrayBaseSlot = UINT32_MAX;

    // 帧计数器
    uint32_t m_frameCounter = 0;
};

} // namespace Renderer
} // namespace DX12Engine
