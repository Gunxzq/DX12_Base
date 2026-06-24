#include "ReflectionProbeManager.h"
#include "Common/d3dx12.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Texture/TextureManager.h"
#include <algorithm>
#include <cmath>

using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

static constexpr uint32_t MAX_PROBES = 64; // 最大反射探针数量

/**
 * @brief 初始化反射探针管理器
 * @param device D3D12 设备
 * @param descriptorHeaps 描述符堆集合
 * @param textureManager 纹理管理器
 * @date 2026-06-23
 */
void ReflectionProbeManager::Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps,
                                        TextureManager *textureManager) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;
    m_textureManager = textureManager;

    if (!m_device || !m_descriptorHeaps || !m_textureManager) {
        return;
    }

    // 分配反射探针数组的描述符槽位
    m_cubemapArrayBaseSlot = m_descriptorHeaps->AllocateConsecutive(DescriptorHeapType::CbvSrvUav, MAX_PROBES);

    if (m_cubemapArrayBaseSlot == UINT32_MAX)
        return;

    // 反射探针数组的SRV描述符句柄
    m_cubemapArraySRV = m_descriptorHeaps->GetGpuHandle(DescriptorHeapType::CbvSrvUav, m_cubemapArrayBaseSlot);

    m_probeEntries.reserve(MAX_PROBES);
    m_initialized = true;
}

/**
 * @brief 关闭反射探针管理器
 * @date 2026-06-23
 */
void ReflectionProbeManager::Shutdown() {
    if (!m_initialized)
        return;

    for (auto &entry : m_probeEntries) {
        if (entry.resources.isValid)
            ReleaseCubemapResource(entry.resources, UINT64_MAX);
    }

    m_probeEntries.clear();

    // 释放所有驻留深度资源
    {
        auto &gpuMgr = GpuResourceManager::GetInstance();
        for (auto &[res, depthRes] : m_depthPool) {
            if (depthRes.gpuHandle.IsValid()) {
                gpuMgr.Release(depthRes.gpuHandle, UINT64_MAX);
            }
            if (depthRes.dsvSlot != UINT32_MAX && m_descriptorHeaps) {
                m_descriptorHeaps->Free(DescriptorHeapType::Dsv, depthRes.dsvSlot, UINT64_MAX);
            }
        }
    }
    m_depthPool.clear();

    if (m_cubemapArrayBaseSlot != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, m_cubemapArrayBaseSlot, UINT64_MAX);
        m_cubemapArrayBaseSlot = UINT32_MAX;
    }

    m_cubemapArraySRV = {};

    m_device = nullptr;
    m_descriptorHeaps = nullptr;
    m_textureManager = nullptr;

    m_initialized = false;
}

/**
 * @brief 添加反射探针
 * @param position 反射探针位置
 * @param captureRange 反射探针捕获范围 （单位：米）
 * @param resolution 反射探针分辨率
 * @param updatePriority 反射探针更新优先级
 * @return uint32_t 反射探针索引
 * @date 2026-06-23
 */
uint32_t ReflectionProbeManager::AddProbe(const DirectX::XMFLOAT3 &position, float captureRange, uint32_t resolution,
                                          uint8_t updatePriority) {
    if (!m_initialized || m_probeEntries.size() >= MAX_PROBES)
        return UINT32_MAX;

    resolution = std::clamp(resolution, 64u, 1024u); // 确保分辨率在64到1024之间
    uint32_t res = 64;                               // 初始化分辨率为64
    while (res < resolution)                         // 找到大于等于目标分辨率的最小2的幂次方
        res <<= 1;

    ProbeEntry entry;
    entry.position = position;
    entry.captureRange = std::max(captureRange, 1.0f); // 确保捕获范围至少为1.0f
    entry.resolution = res;
    entry.updatePriority = std::min(updatePriority, uint8_t(3)); // 确保优先级在0到3之间
    entry.updateCounter = 0;
    entry.needsCapture = true;
    entry.isActive = true;
    entry.resources = AllocateCubemapResource(res);

    if (!entry.resources.isValid) {
        return UINT32_MAX;
    }

    // 为该分辨率获取/共享驻留深度资源
    AcquireDepthResource(res);

    uint32_t index = static_cast<uint32_t>(m_probeEntries.size());
    m_probeEntries.push_back(entry);
    return index;
}

/**
 * @brief 移除反射探针
 * @param probeIndex 反射探针索引
 * @date 2026-06-23
 */
void ReflectionProbeManager::RemoveProbe(uint32_t probeIndex) {
    if (probeIndex >= m_probeEntries.size())
        return;

    auto &entry = m_probeEntries[probeIndex];

    // 释放该探针对分辨率的深度引用
    ReleaseDepthResource(entry.resolution);

    entry.isActive = false; // 标记为无效状态

    if (entry.resources.isValid) {
        ReleaseCubemapResource(entry.resources, UINT64_MAX); // 释放资源
    }
}

/**
 * @brief 更新反射探针位置
 * @param probeIndex 反射探针索引
 * @param position 反射探针位置
 * @attention 动态移动的镜面物体或编辑器使用
 * @date 2026-06-23
 */
void ReflectionProbeManager::UpdateProbePosition(uint32_t probeIndex, const DirectX::XMFLOAT3 &position) {
    if (probeIndex >= m_probeEntries.size())
        return;

    auto &entry = m_probeEntries[probeIndex];
    entry.position = position;
    entry.needsCapture = true;
}

/**
 * @brief 设置反射探针更新优先级
 * @param probeIndex 反射探针索引
 * @param priority 反射探针更新优先级
 * @attention 0：实时更新，1：每3帧更新一次，2：每10帧更新一次，3：仅在需要时更新
 * @date 2026-06-23
 */
void ReflectionProbeManager::SetProbePriority(uint32_t probeIndex, uint8_t priority) {
    if (probeIndex >= m_probeEntries.size())
        return;
    m_probeEntries[probeIndex].updatePriority = std::min(priority, uint8_t(3));
}

/**
 * @brief 设置反射探针捕获范围
 * @param probeIndex 反射探针索引
 * @param range 反射探针捕获范围 （单位：米）
 * @attention 确保捕获范围至少为1.0f
 * @date 2026-06-23
 */
void ReflectionProbeManager::SetProbeCaptureRange(uint32_t probeIndex, float range) {
    if (probeIndex >= m_probeEntries.size())
        return;
    m_probeEntries[probeIndex].captureRange = std::max(range, 1.0f);
}

/**
 * @brief 更新反射探针
 * @param deltaTime 时间间隔
 * @param frameCounter 帧计数器
 * @date 2026-06-23
 */
void ReflectionProbeManager::Update(float deltaTime, uint32_t frameCounter) {
    if (!m_initialized) {
        return;
    }

    m_frameCounter = frameCounter;

    for (uint32_t i = 0; i < m_probeEntries.size(); ++i) {
        auto &entry = m_probeEntries[i];
        if (!entry.isActive) {
            continue;
        }

        if (ShouldUpdateProbe(entry, frameCounter)) {
            if (!entry.resources.isValid) {
                entry.resources = AllocateCubemapResource(entry.resolution); // 创建纹理
            }

            if (entry.resources.isValid) {
                CaptureProbe(entry); // 捕获反射探针
            }
        }
    }
}

/**
 * @brief 判断是否需要更新反射探针
 * @param entry  反射探针条目
 * @param frameCounter  帧计数器
 * @return bool  是否需要更新
 * @date 2026-06-23
 */
bool ReflectionProbeManager::ShouldUpdateProbe(const ProbeEntry &entry, uint32_t frameCounter) const {
    if (entry.updatePriority == 0) {
        return true;
    }

    if (entry.updatePriority == 3) {
        return entry.needsCapture;
    }

    uint32_t interval = (entry.updatePriority == 1) ? 3 : 10;
    return (frameCounter % interval == 0) || entry.needsCapture;
}

/**
 * @brief 查找最近的反射探针
 * @param position 位置
 * @return uint32_t 最近的反射探针索引
 * @attention 如果没有有效反射探针，返回UINT32_MAX
 * @date 2026-06-23
 */
uint32_t ReflectionProbeManager::FindClosestProbe(const DirectX::XMFLOAT3 &position) const {
    if (m_probeEntries.empty()) {
        return UINT32_MAX;
    }

    uint32_t bestIndex = UINT32_MAX;
    float bestDistance = FLT_MAX;

    for (uint32_t i = 0; i < m_probeEntries.size(); ++i) {
        const auto &entry = m_probeEntries[i];
        if (!entry.isActive || !entry.resources.isValid) {
            continue;
        }

        float dx = entry.position.x - position.x;
        float dy = entry.position.y - position.y;
        float dz = entry.position.z - position.z;
        float distSq = dx * dx + dy * dy + dz * dz;

        if (distSq < bestDistance) {
            bestDistance = distSq;
            bestIndex = i;
        }
    }

    return bestIndex;
}
const ProbeRuntimeResources &ReflectionProbeManager::GetProbeResources(uint32_t probeIndex) const {
    static ProbeRuntimeResources s_invalid;
    if (probeIndex >= m_probeEntries.size()) {
        return s_invalid;
    }
    return m_probeEntries[probeIndex].resources;
}

ProbeRuntimeResources ReflectionProbeManager::AllocateCubemapResource(uint32_t resolution) {
    ProbeRuntimeResources resources;
    resources.resolution = resolution;
    resources.isValid = false;

    if (!m_initialized || !m_device || !m_descriptorHeaps) {
        return resources;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = resolution;
    desc.Height = resolution;
    desc.DepthOrArraySize = 6;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearValue.Color[0] = 0.2f;
    clearValue.Color[1] = 0.2f;
    clearValue.Color[2] = 0.3f;
    clearValue.Color[3] = 1.0f;

    GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(m_device, desc, clearValue, D3D12_RESOURCE_STATE_COMMON);

    if (!gpuHandle.IsValid()) {
        return resources;
    }

    uint32_t srvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);
    if (srvSlot == UINT32_MAX) {
        gpuMgr.Release(gpuHandle, 0);
        return resources;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

    ID3D12Resource *resource = gpuMgr.GetResource(gpuHandle);
    if (!resource) {
        m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, srvSlot, 0);
        gpuMgr.Release(gpuHandle, 0);
        return resources;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, srvSlot);
    m_device->CreateShaderResourceView(resource, &srvDesc, cpuHandle);

    TextureHandle texHandle = m_textureManager->RegisterTexture(gpuHandle, srvSlot);
    if (!texHandle.IsValid()) {
        m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, srvSlot, 0);
        gpuMgr.Release(gpuHandle, 0);
        return resources;
    }

    resources.cubemapHandle = texHandle;
    resources.srvSlot = srvSlot;
    resources.isValid = true;

    return resources;
}

/**
 * @brief 释放反射探针资源
 * @param resources 反射探针资源
 * @param fence
 * @date 2026-06-23
 */
void ReflectionProbeManager::ReleaseCubemapResource(ProbeRuntimeResources &resources, uint64_t fence) {
    if (!resources.isValid)
        return;

    auto &gpuMgr = GpuResourceManager::GetInstance();

    if (resources.cubemapHandle.IsValid() && m_textureManager) {
        GpuResourceHandle gpuHandle = m_textureManager->GetGpuHandle(resources.cubemapHandle);
        if (gpuHandle.IsValid()) {
            gpuMgr.Release(gpuHandle, fence);
        }
        m_textureManager->Release(resources.cubemapHandle, fence);
    }
    if (resources.srvSlot != UINT32_MAX && m_descriptorHeaps)
        m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, resources.srvSlot, fence);

    resources = {};
}

// ========================================================================
// 驻留深度资源管理
// ========================================================================

uint32_t ReflectionProbeManager::AcquireDepthResource(uint32_t resolution) {
    if (!m_initialized || !m_device || !m_descriptorHeaps)
        return UINT32_MAX;

    auto it = m_depthPool.find(resolution);
    if (it != m_depthPool.end()) {
        // 已有该分辨率的深度资源，增加引用计数
        ++it->second.refCount;
        return it->second.dsvSlot;
    }

    // 创建新的深度资源（Texture2D, D32_FLOAT）
    auto &gpuMgr = GpuResourceManager::GetInstance();

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = resolution;
    desc.Height = resolution;
    desc.DepthOrArraySize = 1;             // 单 slice，逐面渲染共享
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    GpuResourceHandle gpuHandle = gpuMgr.CreateTexture2D(m_device, desc, clearValue, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (!gpuHandle.IsValid()) {
        return UINT32_MAX;
    }

    // 从 GpuResourceManager 获取 ID3D12Resource 指针用于创建 DSV
    ID3D12Resource *resource = gpuMgr.GetResource(gpuHandle);
    if (!resource) {
        gpuMgr.Release(gpuHandle, 0);
        return UINT32_MAX;
    }

    uint32_t dsvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::Dsv);
    if (dsvSlot == UINT32_MAX) {
        gpuMgr.Release(gpuHandle, 0);
        return UINT32_MAX;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Dsv, dsvSlot);
    m_device->CreateDepthStencilView(resource, nullptr, dsvHandle);

    ProbeDepthResource depthRes;
    depthRes.gpuHandle = gpuHandle;
    depthRes.dsvSlot = dsvSlot;
    depthRes.resolution = resolution;
    depthRes.refCount = 1;

    m_depthPool[resolution] = std::move(depthRes);
    return dsvSlot;
}

void ReflectionProbeManager::ReleaseDepthResource(uint32_t resolution) {
    auto it = m_depthPool.find(resolution);
    if (it == m_depthPool.end())
        return;

    if (it->second.refCount > 0) {
        --it->second.refCount;
    }

    if (it->second.refCount == 0) {
        // 没有探针再使用此分辨率，释放资源
        if (it->second.gpuHandle.IsValid()) {
            GpuResourceManager::GetInstance().Release(it->second.gpuHandle, UINT64_MAX);
        }
        if (it->second.dsvSlot != UINT32_MAX && m_descriptorHeaps) {
            m_descriptorHeaps->Free(DescriptorHeapType::Dsv, it->second.dsvSlot, UINT64_MAX);
        }
        m_depthPool.erase(it);
    }
}

uint32_t ReflectionProbeManager::GetProbeDepthSlot(uint32_t probeIndex) const {
    if (probeIndex >= m_probeEntries.size())
        return UINT32_MAX;

    uint32_t res = m_probeEntries[probeIndex].resolution;
    auto it = m_depthPool.find(res);
    if (it == m_depthPool.end())
        return UINT32_MAX;

    return it->second.dsvSlot;
}

/**
 * @brief 捕获反射探针
 * @param entry 反射探针条目
 * @date 2026-06-23
 */
void ReflectionProbeManager::CaptureProbe(ProbeEntry &entry) {
    if (!m_initialized || !m_device)
        return;
    entry.needsCapture = false;
}

} // namespace DX12Engine::Renderer
