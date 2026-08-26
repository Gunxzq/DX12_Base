#include "SkyboxManager.h"
#include "Common/ThrowHelper.h"
#include "Logger/Logger.h" // 2026-08-19：Map 失败防御日志（统一 Map/Unmap 加固）
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include <DirectXMath.h>

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ========================================================================
// 单例实现
// ========================================================================

SkyboxManager &SkyboxManager::GetInstance() {
    static SkyboxManager instance;
    return instance;
}

// ========================================================================
// 生命周期
// ========================================================================

void SkyboxManager::Initialize(ID3D12Device *device, DescriptorHeapCollection *descHeaps, Resource::HeapTag heapTag) {
    m_device = device;
    m_descHeaps = descHeaps;
    m_heapTag = heapTag;
    m_initialized = (device != nullptr && descHeaps != nullptr);
}

void SkyboxManager::Shutdown() {
    auto &gpuMgr = GpuResourceManager::GetInstance();

    if (m_cbBuffer.IsValid()) {
        gpuMgr.Release(m_cbBuffer, 0);
        m_cbBuffer = {};
    }
    if (m_textureResource.IsValid()) {
        // 纹理资源由 AssetManager/TextureManager 管理，Manager 不释放
        m_textureResource = {};
    }

    m_geometryHandle = {};
    m_cubeSrvIndex = UINT32_MAX;
    m_objectCBAddress = 0;
    m_device = nullptr;
    m_descHeaps = nullptr;
    m_initialized = false;
}

// ========================================================================
// 天空盒设置
// ========================================================================

void SkyboxManager::SetSkybox(GpuResourceHandle textureResource, GeometryHandle geometryHandle) {
    if (!m_initialized || !textureResource.IsValid() || !geometryHandle.IsValid())
        return;

    m_textureResource = textureResource;
    m_geometryHandle = geometryHandle;

    // 创建 Cubemap SRV（每次 SetSkybox 重新创建）
    CreateCubeSRV();

    // 分配持久 CB（首次调用时分配，后续复用）
    if (m_objectCBAddress == 0)
        AllocateObjectCB();
}

void SkyboxManager::ClearSkybox() {
    m_textureResource = {};
    m_geometryHandle = {};
    // 保留 m_cubeSrvIndex / m_cbBuffer — 下次 SetSkybox 可复用
}

// ========================================================================
// 数据访问
// ========================================================================

D3D12_GPU_DESCRIPTOR_HANDLE SkyboxManager::GetCubeSRV() const {
    // 真实天空盒优先；无天空盒时返回注入的空白 Cubemap fallback（BlankTextureProvider 提供）
    if (m_cubeSrvIndex != UINT32_MAX)
        return m_descHeaps->GetPartitionGpuHandle(PartitionType::Texture, m_cubeSrvIndex, m_heapTag);
    return m_fallbackCubeSRV;
}

// ========================================================================
// 内部辅助
// ========================================================================

void SkyboxManager::CreateCubeSRV() {
    if (!m_device || !m_descHeaps || !m_textureResource.IsValid())
        return;

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Resource *texRes = gpuMgr.GetResource(m_textureResource);
    if (!texRes)
        return;

    D3D12_RESOURCE_DESC rDesc = texRes->GetDesc();

    // 释放旧的 SRV 槽位
    if (m_cubeSrvIndex != UINT32_MAX) {
        m_descHeaps->Free(m_heapTag, PartitionType::Texture, m_cubeSrvIndex, UINT64_MAX);
    }
    uint32_t newSrvIdx = m_descHeaps->Allocate(m_heapTag, PartitionType::Texture);
    if (newSrvIdx == UINT32_MAX)
        return;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = rDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.TextureCube.MipLevels = rDesc.MipLevels;
    srvDesc.TextureCube.MostDetailedMip = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuH = m_descHeaps->GetPartitionCpuHandle(PartitionType::Texture, newSrvIdx, m_heapTag);
    m_device->CreateShaderResourceView(texRes, &srvDesc, cpuH);

    m_cubeSrvIndex = newSrvIdx;
}

void SkyboxManager::AllocateObjectCB() {
    if (!m_device || m_objectCBAddress != 0)
        return;

    auto &gpuMgr = GpuResourceManager::GetInstance();

    // 单位矩阵
    DirectX::XMFLOAT4X4 identity;
    DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());

    // 持久 UPLOAD 堆缓冲（64 字节，贯穿生命周期）
    m_cbBuffer = gpuMgr.CreateBuffer(m_device, sizeof(DirectX::XMFLOAT4X4), L"SkyboxManager_CB", D3D12_HEAP_TYPE_UPLOAD,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_cbBuffer.IsValid())
        return;

    ID3D12Resource *cbRes = gpuMgr.GetResource(m_cbBuffer);
    if (!cbRes) {
        gpuMgr.Release(m_cbBuffer, 0);
        m_cbBuffer = {};
        return;
    }

    void *mapped = nullptr;
    HRESULT mapHr = cbRes->Map(0, nullptr, &mapped);
    // 2026-08-19 防御加固：Map 失败时绝不 Unmap（#310）/memcpy（写 0x0），跳过并记录
    if (FAILED(mapHr) || !mapped) {
        Logger::Logger::GetInstance()->Warn("[SkyboxManager] ObjectCB Map failed: hr=0x{:08X}", (unsigned)mapHr);
        return;
    }
    memcpy(mapped, &identity, sizeof(DirectX::XMFLOAT4X4));
    cbRes->Unmap(0, nullptr);

    m_objectCBAddress = cbRes->GetGPUVirtualAddress();
}

} // namespace DX12Engine::Renderer
