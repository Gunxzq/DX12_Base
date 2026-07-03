#include "ReflectionProbeManager.h"
#include "Common/d3dx12.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Core/DescriptorHeapCollection.h"
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
void ReflectionProbeManager::Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;

    if (!m_device || !m_descriptorHeaps) {
        return;
    }

    // 分配反射探针数组的描述符槽位
    m_cubemapArrayBaseSlot = m_descriptorHeaps->AllocateConsecutive(PartitionType::Buffer, MAX_PROBES);

    if (m_cubemapArrayBaseSlot == UINT32_MAX)
        return;

    // 反射探针数组的SRV描述符句柄
    m_cubemapArraySRV = m_descriptorHeaps->GetGpuHandle(PartitionType::Buffer, m_cubemapArrayBaseSlot);

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
            ReleaseCubemapResource(entry.resources, UINT64_MAX); // 释放Cubemap资源
        if (entry.depthHandle.IsValid()) {
            DepthStencilPool::GetInstance().Free(entry.depthHandle, UINT64_MAX);
            entry.depthHandle = {};
        }
    }

    m_probeEntries.clear();

    if (m_cubemapArrayBaseSlot != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(PartitionType::Buffer, m_cubemapArrayBaseSlot, UINT64_MAX);
        m_cubemapArrayBaseSlot = UINT32_MAX;
    }

    m_cubemapArraySRV = {};

    m_device = nullptr;
    m_descriptorHeaps = nullptr;

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

    {
        DepthStencilDesc depthDesc = {};
        depthDesc.width = res;
        depthDesc.height = res;
        depthDesc.format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.arraySize = 6; // 与 Cubemap RTV 的 6 个面匹配
        depthDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;
        depthDesc.clearValue = clearValue;
        depthDesc.name = L"Probe_Depth";

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = 0;
        dsvDesc.Texture2DArray.ArraySize = 6;

        entry.depthHandle = DepthStencilPool::GetInstance().Allocate(depthDesc, &dsvDesc);
    }

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

    if (entry.depthHandle.IsValid()) {
        DepthStencilPool::GetInstance().Free(entry.depthHandle, UINT64_MAX);
        entry.depthHandle = {};
    }

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

DirectX::XMFLOAT3 ReflectionProbeManager::GetProbePosition(uint32_t probeIndex) const {
    if (probeIndex >= m_probeEntries.size()) {
        return {};
    }
    return m_probeEntries[probeIndex].position;
}

float ReflectionProbeManager::GetProbeCaptureRange(uint32_t probeIndex) const {
    if (probeIndex >= m_probeEntries.size()) {
        return 0.0f;
    }
    return m_probeEntries[probeIndex].captureRange;
}

ProbeRuntimeResources ReflectionProbeManager::AllocateCubemapResource(uint32_t resolution) {
    ProbeRuntimeResources resources;
    resources.resolution = resolution;
    resources.isValid = false;

    if (!m_initialized || !m_device || !m_descriptorHeaps) {
        return resources;
    }

    auto &rtPool = RenderTargetPool::GetInstance();

    RenderTargetDesc rtDesc = {};
    rtDesc.width = resolution;
    rtDesc.height = resolution;
    rtDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtDesc.arraySize = 6;
    rtDesc.mipLevels = 1;
    rtDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clearValue.Color[0] = 0.2f;
    clearValue.Color[1] = 0.2f;
    clearValue.Color[2] = 0.3f;
    clearValue.Color[3] = 1.0f;
    rtDesc.clearValue = clearValue;
    rtDesc.name = L"Probe_Cubemap";

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtvDesc.Texture2DArray.MipSlice = 0;
    rtvDesc.Texture2DArray.FirstArraySlice = 0;
    rtvDesc.Texture2DArray.ArraySize = 6;

    resources.rtHandle = rtPool.Allocate(rtDesc, &rtvDesc);
    if (!resources.rtHandle.IsValid()) {
        return resources;
    }

    ID3D12Resource *resource = rtPool.GetResource(resources.rtHandle);
    if (!resource) {
        rtPool.Free(resources.rtHandle, 0);
        resources.rtHandle = {};
        return resources;
    }

    // 分配 SRV 槽位并创建 TEXTURECUBE SRV
    uint32_t srvSlot = m_descriptorHeaps->Allocate(PartitionType::Buffer);
    if (srvSlot == UINT32_MAX) {
        rtPool.Free(resources.rtHandle, 0);
        resources.rtHandle = {};
        return resources;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels = 1;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_descriptorHeaps->GetCpuHandle(PartitionType::Buffer, srvSlot);
    m_device->CreateShaderResourceView(resource, &srvDesc, cpuHandle);

    resources.srvSlot = srvSlot;
    resources.isValid = true;

    // 在 Cubemap Array 基槽写入 TextureCubeArray SRV（供着色器 gReflectionCubemapArray 采样）
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC arraySrvDesc = {};
        arraySrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        arraySrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        arraySrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        arraySrvDesc.TextureCubeArray.MostDetailedMip = 0;
        arraySrvDesc.TextureCubeArray.MipLevels = 1;
        arraySrvDesc.TextureCubeArray.First2DArrayFace = 0;
        arraySrvDesc.TextureCubeArray.NumCubes = 1;
        arraySrvDesc.TextureCubeArray.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE arrayCpuHandle =
            m_descriptorHeaps->GetCpuHandle(PartitionType::Buffer, m_cubemapArrayBaseSlot);
        m_device->CreateShaderResourceView(resource, &arraySrvDesc, arrayCpuHandle);
    }

    return resources;
}

// ========================================================================
// 释放 Cubemap 资源
// ========================================================================
void ReflectionProbeManager::ReleaseCubemapResource(ProbeRuntimeResources &resources, uint64_t fence) {
    if (!resources.isValid)
        return;

    if (resources.rtHandle.IsValid()) {
        RenderTargetPool::GetInstance().Free(resources.rtHandle, fence);
        resources.rtHandle = {};
    }

    if (resources.srvSlot != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(PartitionType::Buffer, resources.srvSlot, fence);
        resources.srvSlot = UINT32_MAX;
    }

    resources = {};
}

uint32_t ReflectionProbeManager::GetProbeDepthSlot(uint32_t probeIndex) const {
    if (probeIndex >= m_probeEntries.size())
        return UINT32_MAX;

    return m_probeEntries[probeIndex].depthHandle.dsvSlot;
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
