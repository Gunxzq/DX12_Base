#include "LightManager.h"
#include "Common/ThrowHelper.h"
#include "Renderer/Scene/CameraManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Struct/Descriptor.h"
#include <cstring>

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

static constexpr uint32_t MAX_LIGHTS = 256;
static constexpr uint32_t DEFAULT_LIGHT_BUFFER_SIZE = 64 * 1024;   // 64KB
static constexpr uint32_t DEFAULT_SHADOW_BUFFER_SIZE = 256 * 1024; // 256KB

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

void LightManager::Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps,
                              Resource::HeapTag heapTag) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;
    m_heapTag = heapTag;
    Clear();

    // 校验描述符堆有效性
    if (!descriptorHeaps || !descriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) {
        OutputDebugStringA("[LightManager] ERROR: DescriptorHeapCollection or CbvSrvUav heap is null!\n");
        return;
    }
    {
        char buf[128];
        sprintf_s(buf, "[LightManager] CbvSrvUav heap valid, capacity=%u, allocated=%u\n",
                  descriptorHeaps->GetHeapSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
                  descriptorHeaps->GetAllocatedCount(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
        OutputDebugStringA(buf);
    }

    // 初始化内部 RingBuffer
    m_lightBuffer.Initialize(device, DEFAULT_LIGHT_BUFFER_SIZE, L"LightManager_LightBuffer");
    m_shadowSampleBuffer.Initialize(device, DEFAULT_SHADOW_BUFFER_SIZE, L"LightManager_ShadowSampleBuffer");
    m_dirShadowRenderBuffer.Initialize(device, DEFAULT_SHADOW_BUFFER_SIZE, L"LightManager_DirShadowRenderBuffer");
    m_spotShadowRenderBuffer.Initialize(device, DEFAULT_SHADOW_BUFFER_SIZE, L"LightManager_SpotShadowRenderBuffer");
    m_pointShadowBuffer.Initialize(device, DEFAULT_SHADOW_BUFFER_SIZE, L"LightManager_PointShadowBuffer");

    // 创建阴影采样参数 StructuredBuffer (UPLOAD 堆，每帧 UpdateAndUpload 写入)
    {
        size_t paramsSize = MAX_LIGHTS * sizeof(ShadowParams);
        m_shadowParamsBufferHandle = GpuResourceManager::GetInstance().CreateBuffer(
            device, paramsSize, L"ShadowParams_Buffer", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        // 分配 SRV 槽位：t11 (ShadowParams)
        m_shadowDataSrvBaseSlot = descriptorHeaps->Allocate(PartitionType::Buffer);
        if (m_shadowDataSrvBaseSlot != UINT32_MAX) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

            // t11: ShadowParams
            srvDesc.Buffer.NumElements = MAX_LIGHTS;
            srvDesc.Buffer.StructureByteStride = sizeof(ShadowParams);
            device->CreateShaderResourceView(
                GpuResourceManager::GetInstance().GetResource(m_shadowParamsBufferHandle), &srvDesc,
                descriptorHeaps->GetCpuHandle(PartitionType::Buffer, m_shadowDataSrvBaseSlot));

            m_shadowDataSRV = descriptorHeaps->GetGpuHandle(PartitionType::Buffer, m_shadowDataSrvBaseSlot);
        }
    }

    // 预分配阴影贴图纹理 SRV 槽位（Shadow 分区，gShadowMaps[] 无界数组）
    {
        m_shadowMapSrvDirSlot = descriptorHeaps->Allocate(PartitionType::Shadow);
        if (m_shadowMapSrvDirSlot != UINT32_MAX) {
            // GetShadowMapSRV 返回 Shadow 分区起始 GPU 句柄（无界数组基地址）
            m_shadowMapSRV = descriptorHeaps->GetPartitionGpuHandle(PartitionType::Shadow, 0);
            // 方向光阴影贴图 SRV 的具体内容在 CreateShadowMapForDirectionalLight 中创建
        }
    }

    m_initialized = true;
}

void LightManager::Shutdown() {
    // 释放阴影贴图资源
    if (m_dirShadow.isValid) {
        ReleaseShadowMap(m_dirShadow, 0);
    }
    for (auto &res : m_spotShadowResources) {
        if (res.isValid) {
            ReleaseShadowMap(res, 0);
        }
    }
    m_spotShadowResources.clear();
    for (auto &res : m_pointShadowResources) {
        if (res.isValid) {
            for (int f = 0; f < 6; ++f) {
                if (res.dsvSlots[f] != UINT32_MAX)
                    m_descriptorHeaps->Free(PartitionType::Dsv, res.dsvSlots[f], 0);
            }
            if (res.srvBaseSlot != UINT32_MAX)
                m_descriptorHeaps->Free(PartitionType::Shadow, res.srvBaseSlot, 0);
            if (res.arrayHandle.IsValid())
                DepthStencilPool::GetInstance().Free(res.arrayHandle, 0);
        }
    }
    m_pointShadowResources.clear();

    // 释放阴影采样参数 StructuredBuffer
    auto &gpuMgr = GpuResourceManager::GetInstance();
    if (m_shadowParamsBufferHandle.IsValid()) {
        gpuMgr.Release(m_shadowParamsBufferHandle, 0);
        m_shadowParamsBufferHandle = {};
    }

    // 回收 GpuResourceManager 和 DescriptorHeap 的延迟释放
    if (m_descriptorHeaps) {
        m_descriptorHeaps->Reclaim(PartitionType::Dsv, 0);
        m_descriptorHeaps->Reclaim(PartitionType::Buffer, 0);

        // 释放阴影数据 SRV 槽
        if (m_shadowDataSrvBaseSlot != UINT32_MAX) {
            m_descriptorHeaps->Free(PartitionType::Buffer, m_shadowDataSrvBaseSlot, 0);
            m_shadowDataSrvBaseSlot = UINT32_MAX;
        }

        // 释放阴影贴图 SRV 槽
        if (m_shadowMapSrvDirSlot != UINT32_MAX) {
            m_descriptorHeaps->Free(PartitionType::Shadow, m_shadowMapSrvDirSlot, 0);
            m_shadowMapSrvDirSlot = UINT32_MAX;
        }
    }

    m_lightBuffer.Shutdown();
    m_shadowSampleBuffer.Shutdown();
    m_dirShadowRenderBuffer.Shutdown();
    m_spotShadowRenderBuffer.Shutdown();
    m_pointShadowBuffer.Shutdown();

    m_lightCBAddress = 0;
    m_shadowSampleAddress = 0;
    m_shadowDataSRV = {};
    m_shadowMapSRV = {};

    m_device = nullptr;
    m_descriptorHeaps = nullptr;
    m_initialized = false;
}

/**
 * @brief 更新并上传光源常量
 * @param fence 未来的fence
 * @param cameraPos
 * @date 2026-06-03
 */
void LightManager::UpdateAndUpload(uint64_t fence, const Camera &camera) {
    if (!m_initialized)
        return;

    // 如果光源数据有变化，重建常量
    if (m_lightDirty) {
        RebuildLightConstants();
    }

    // 方向光依赖相机视锥体，必须每帧重建；点/聚光灯仅在脏时重建
    if (m_shadowDirty || !m_dirLights.empty()) {
        RebuildShadowConstants(camera);
        m_shadowDirty = false;
    }

    // 只在脏数据变化时才上传，光源数据是低频更新的
    if (m_lightDirty) {
        m_lightCBAddress = m_lightBuffer.AllocateUpload(&m_lightConstants, sizeof(LightConstants), fence);
        m_lightDirty = false;
    }

    if (!m_shadowParams.empty()) {
        m_shadowSampleAddress = m_shadowSampleBuffer.AllocateUpload(
            m_shadowParams.data(), static_cast<uint32_t>(m_shadowParams.size() * sizeof(ShadowParams)), fence, 16);

        // 同步写入阴影采样参数 StructuredBuffer（UPLOAD 堆，直接 Map 写入）
        if (m_shadowParamsBufferHandle.IsValid()) {
            ID3D12Resource *resource = GpuResourceManager::GetInstance().GetResource(m_shadowParamsBufferHandle);
            if (resource) {
                void *mapped = nullptr;
                resource->Map(0, nullptr, &mapped);
                memcpy(mapped, m_shadowParams.data(), m_shadowParams.size() * sizeof(ShadowParams));
                resource->Unmap(0, nullptr);
            }
        }
    }
    // 上传方向光阴影 cbuffer（逐光源）并存储地址
    for (size_t i = 0; i < m_dirShadowCBConstants.size(); ++i) {
        D3D12_GPU_VIRTUAL_ADDRESS addr = m_dirShadowRenderBuffer.AllocateUpload(
            &m_dirShadowCBConstants[i], sizeof(DirLightShadowConstants), fence, 256);
        if (i == 0)
            m_dirShadow.cbvAddress = addr;
    }

    // 上传聚光灯阴影 cbuffer（逐光源）并存储地址
    for (size_t i = 0; i < m_spotShadowCBConstants.size(); ++i) {
        D3D12_GPU_VIRTUAL_ADDRESS addr = m_spotShadowRenderBuffer.AllocateUpload(
            &m_spotShadowCBConstants[i], sizeof(SpotLightShadowConstants), fence, 256);
        if (i < m_spotShadowResources.size())
            m_spotShadowResources[i].cbvAddress = addr;
    }

    // 上传点光源阴影常量并存储地址
    for (size_t i = 0; i < m_pointShadowConstants.size(); ++i) {
        D3D12_GPU_VIRTUAL_ADDRESS addr = m_pointShadowBuffer.AllocateUpload(
            &m_pointShadowConstants[i], sizeof(PointLightShadowConstants), fence, 256);
        if (i < m_pointShadowResources.size())
            m_pointShadowResources[i].cbvAddress = addr;
    }

    // 回收
    m_lightBuffer.Reclaim(fence);
    m_shadowSampleBuffer.Reclaim(fence);
    m_dirShadowRenderBuffer.Reclaim(fence);
    m_spotShadowRenderBuffer.Reclaim(fence);
    m_pointShadowBuffer.Reclaim(fence);
}

// ============================================================================
// 光源设置
// ============================================================================

void LightManager::Clear() {
    // 释放阴影贴图 GPU 资源
    if (m_dirShadow.isValid) {
        ReleaseShadowMap(m_dirShadow, 0);
    }
    for (auto &res : m_spotShadowResources) {
        if (res.isValid) {
            ReleaseShadowMap(res, 0);
        }
    }
    m_spotShadowResources.clear();
    for (auto &res : m_pointShadowResources) {
        if (res.isValid) {
            for (int f = 0; f < 6; ++f) {
                if (res.dsvSlots[f] != UINT32_MAX)
                    m_descriptorHeaps->Free(PartitionType::Dsv, res.dsvSlots[f], 0);
            }
            if (res.srvBaseSlot != UINT32_MAX)
                m_descriptorHeaps->Free(PartitionType::Shadow, res.srvBaseSlot, 0);
            if (res.arrayHandle.IsValid())
                DepthStencilPool::GetInstance().Free(res.arrayHandle, 0);
        }
    }
    m_pointShadowResources.clear();

    memset(&m_lightConstants, 0, sizeof(LightConstants));
    m_dirLights.clear();
    m_pointLights.clear();
    m_spotLights.clear();

    m_shadowParams.clear();
    m_dirShadowCBConstants.clear();
    m_spotShadowCBConstants.clear();
    m_pointShadowConstants.clear();
    m_lightDirty = true;
    m_lightDirty = true;
    m_shadowDirty = true;
}

void LightManager::ResetLightData() {
    m_dirLights.clear();
    m_pointLights.clear();
    m_spotLights.clear();
    memset(&m_lightConstants, 0, sizeof(LightConstants));
    m_lightDirty = true;
    m_shadowDirty = true;
}

void LightManager::SetDirectionalLight(const Light &light, uint32_t index) {
    if (index >= MAX_LIGHTS)
        return;

    Light fixedLight = light;
    fixedLight.Direction.w = 0.0f; // 确保 .w=0

    // 确保 m_dirLights 容量足够
    if (index >= m_dirLights.size()) {
        m_dirLights.resize(index + 1);
    }
    bool shadowChanged = index < m_dirLights.size() && (m_dirLights[index].CastShadow != fixedLight.CastShadow ||
                                                        m_dirLights[index].ShadowMapIndex != fixedLight.ShadowMapIndex);
    m_dirLights[index] = fixedLight;
    m_lightDirty = true;
    if (shadowChanged)
        m_shadowDirty = true;
}

void LightManager::SetAmbientLight(const DirectX::XMFLOAT4 &light) {
    m_lightConstants.AmbientLight = light;
    m_lightDirty = true;
}

uint32_t LightManager::AddPointLight(const Light &light) {
    Light fixedLight = light;
    fixedLight.Position.w = 0.0f; // 确保 .w=0，避免污染 DirectXMath 向量运算
    fixedLight.Direction.w = 0.0f;
    if (fixedLight.Range <= 0.0f)
        fixedLight.Range = 10.0f;

    uint32_t index = static_cast<uint32_t>(m_pointLights.size());
    m_pointLights.push_back(fixedLight);
    m_lightDirty = true;
    return index;
}

void LightManager::SetPointLight(uint32_t index, const Light &light) {
    if (index < m_pointLights.size()) {
        bool shadowChanged = (m_pointLights[index].CastShadow != light.CastShadow) ||
                             (m_pointLights[index].ShadowMapIndex != light.ShadowMapIndex);
        m_pointLights[index] = light;
        m_lightDirty = true;
        if (shadowChanged)
            m_shadowDirty = true;
    }
}

void LightManager::RemovePointLight(uint32_t index) {
    if (index < m_pointLights.size()) {
        // 释放对应的阴影贴图
        if (index < m_pointShadowResources.size() && m_pointShadowResources[index].isValid) {
            ReleaseShadowMap(m_pointShadowResources[index], 0);
        }
        m_pointLights.erase(m_pointLights.begin() + index);
        if (index < m_pointShadowResources.size())
            m_pointShadowResources.erase(m_pointShadowResources.begin() + index);
        m_lightDirty = true;
        m_shadowDirty = true;
    }
}

uint32_t LightManager::AddSpotLight(const Light &light) {
    Light fixedLight = light;
    fixedLight.Position.w = 0.0f; // 确保 .w=0
    fixedLight.Direction.w = 0.0f;
    if (fixedLight.Range <= 0.0f)
        fixedLight.Range = 10.0f;

    uint32_t index = static_cast<uint32_t>(m_spotLights.size());
    m_spotLights.push_back(fixedLight);
    m_lightDirty = true;
    return index;
}

void LightManager::SetSpotLight(uint32_t index, const Light &light) {
    if (index < m_spotLights.size()) {
        bool shadowChanged = (m_spotLights[index].CastShadow != light.CastShadow) ||
                             (m_spotLights[index].ShadowMapIndex != light.ShadowMapIndex);
        m_spotLights[index] = light;
        m_lightDirty = true;
        if (shadowChanged)
            m_shadowDirty = true;
    }
}

void LightManager::RemoveSpotLight(uint32_t index) {
    if (index < m_spotLights.size()) {
        m_spotLights.erase(m_spotLights.begin() + index);
        m_lightDirty = true;
        m_shadowDirty = true;
    }
}

const SpotShadowResources &LightManager::GetSpotShadowResource(uint32_t index) const {
    static SpotShadowResources invalid = {};
    return (index < m_spotShadowResources.size()) ? m_spotShadowResources[index] : invalid;
}

const PointShadowResources &LightManager::GetPointShadowResource(uint32_t index) const {
    static PointShadowResources invalid = {};
    return (index < m_pointShadowResources.size()) ? m_pointShadowResources[index] : invalid;
}

// ============================================================================
// 通用光源访问
// ============================================================================

uint32_t LightManager::GetLightCount(LightType type) const {
    switch (type) {
    case LightType::Directional:
        return m_lightConstants.NumDirLights;
    case LightType::Point:
        return m_lightConstants.NumPointLights;
    case LightType::Spot:
        return m_lightConstants.NumSpotLights;
    default:
        return 0;
    }
}

const Light *LightManager::GetLight(LightType type, uint32_t index) const {
    switch (type) {
    case LightType::Directional:
        return (index < m_dirLights.size()) ? &m_dirLights[index] : nullptr;
    case LightType::Point:
        return (index < m_pointLights.size()) ? &m_pointLights[index] : nullptr;
    case LightType::Spot:
        return (index < m_spotLights.size()) ? &m_spotLights[index] : nullptr;
    default:
        return nullptr;
    }
}

// ============================================================================
// 内部实现 — 重建常量数组
// ============================================================================

void LightManager::RebuildLightConstants() {
    uint32_t offset = 0;

    // 复制方向光到 Lights[0..NumDirLights-1]
    m_lightConstants.NumDirLights = static_cast<uint32_t>(m_dirLights.size());
    for (size_t i = 0; i < m_dirLights.size() && offset < MAX_LIGHTS; ++i) {
        Light light = m_dirLights[i];
        light.Type = 0.0f; // Directional
        m_lightConstants.Lights[offset++] = light;
    }

    // 复制点光源
    m_lightConstants.NumPointLights = static_cast<uint32_t>(m_pointLights.size());
    for (size_t i = 0; i < m_pointLights.size() && offset < MAX_LIGHTS; ++i) {
        Light light = m_pointLights[i];
        light.Type = 1.0f; // Point
        m_lightConstants.Lights[offset++] = light;
    }

    // 复制聚光灯
    m_lightConstants.NumSpotLights = static_cast<uint32_t>(m_spotLights.size());
    for (size_t i = 0; i < m_spotLights.size() && offset < MAX_LIGHTS; ++i) {
        Light light = m_spotLights[i];
        light.Type = 2.0f; // Spot
        m_lightConstants.Lights[offset++] = light;
    }

    // 注意：不在此处清除 m_lightDirty，由 UpdateAndUpload 在上传完成后统一清除
}

void LightManager::RebuildShadowConstants(const Camera &camera) {
    // 方向光阴影采样参数
    m_shadowParams.clear();
    m_dirShadowCBConstants.clear();

    for (size_t i = 0; i < m_dirLights.size(); ++i) {
        ShadowParams params = {};
        params.Type = 0; // Directional
        ComputeDirShadowMatrix(m_dirLights[i], params, camera);
        params.ShadowMapIndex = m_dirShadow.isValid ? m_dirShadow.srvSlot : UINT32_MAX;
        m_shadowParams.push_back(params);
    }

    // 方向光阴影 cbuffer（供 ShadowRenderer b1 渲染 Pass 使用，仅需 VP 矩阵）
    for (size_t i = 0; i < m_shadowParams.size(); ++i) {
        if (m_shadowParams[i].Type == 0) { // Directional
            DirLightShadowConstants cbConst = {};
            cbConst.LightViewProj = m_shadowParams[i].LightViewProj;
            m_dirShadowCBConstants.push_back(cbConst);
        }
    }

    // 点光源阴影采样参数
    for (size_t i = 0; i < m_pointLights.size(); ++i) {
        ShadowParams params = {};
        params.Type = 1; // Point
        const auto &light = m_pointLights[i];
        params.LightPosition = DirectX::XMFLOAT3(light.Position.x, light.Position.y, light.Position.z);
        params.Range = light.Range;
        params.Bias = light.ShadowBias;
        params.ShadowStrength = light.CastShadow;
        params.ShadowMapSize = 1024.0f; // 缓存贴图分辨率
        params.ShadowMapIndex = (i < m_pointShadowResources.size() && m_pointShadowResources[i].isValid)
                                    ? m_pointShadowResources[i].srvBaseSlot
                                    : UINT32_MAX;
        m_shadowParams.push_back(params);
    }

    // 点光源阴影常量（VP 矩阵，仅供渲染 Pass 使用，非采样）
    m_pointShadowConstants.clear();
    for (size_t i = 0; i < m_pointLights.size(); ++i) {
        PointLightShadowConstants constants = {};
        ComputePointShadowMatrices(m_pointLights[i], constants);
        m_pointShadowConstants.push_back(constants);
    }

    // 聚光灯阴影采样参数 + 渲染常量
    m_spotShadowCBConstants.clear();
    for (size_t i = 0; i < m_spotLights.size(); ++i) {
        ShadowParams params = {};
        params.Type = 2; // Spot
        ComputeSpotShadowMatrix(m_spotLights[i], params);
        uint32_t srvSlot = (i < m_spotShadowResources.size() && m_spotShadowResources[i].isValid)
                               ? m_spotShadowResources[i].srvSlot
                               : UINT32_MAX;
        params.ShadowMapIndex = srvSlot;
        params.ShadowMapSize = 1024.0f;
        m_shadowParams.push_back(params);

        // 渲染常量（供 ShadowRenderer b1 使用，仅需 VP 矩阵）
        SpotLightShadowConstants cbConst = {};
        cbConst.LightViewProj = params.LightViewProj;
        m_spotShadowCBConstants.push_back(cbConst);
    }

    // 更新各光源的 ShadowMapIndex，使其直接指向 gShadowParams[] 中的对应项
    uint32_t shadowIdx = 0;
    for (size_t i = 0; i < m_dirLights.size(); ++i, ++shadowIdx)
        m_dirLights[i].ShadowMapIndex = static_cast<float>(shadowIdx);
    for (size_t i = 0; i < m_pointLights.size(); ++i, ++shadowIdx)
        m_pointLights[i].ShadowMapIndex = static_cast<float>(shadowIdx);
    for (size_t i = 0; i < m_spotLights.size(); ++i, ++shadowIdx)
        m_spotLights[i].ShadowMapIndex = static_cast<float>(shadowIdx);
    m_lightDirty = true;
}

void LightManager::ComputeDirShadowMatrix(const Light &light, ShadowParams &outParams, const Camera &mainCamera) {
    using namespace DirectX;

    // 1. 限制阴影覆盖距离（根据场景调整）
    const float SHADOW_DISTANCE = 30.0f; // 相机前方 30 单位
    const float MAX_RANGE = 35.0f;       // 最大覆盖范围 35x35
    const float EXPAND = 1.0f;           // 包围盒扩大值

    // 创建临时相机，限制远平面
    Camera shadowCamera = mainCamera;
    shadowCamera.FarPlane = SHADOW_DISTANCE;

    // 2. 构建视锥体并获取角点
    Frustum cameraFrustum;
    cameraFrustum.BuildFromCamera(shadowCamera.Position, shadowCamera.Forward, shadowCamera.Up, shadowCamera.FOV,
                                  shadowCamera.AspectRatio, shadowCamera.NearPlane, shadowCamera.FarPlane);

    const auto &worldCorners = cameraFrustum.GetCorners();

    // 3. 光源方向
    XMVECTOR lightDir = XMLoadFloat4(&light.Direction);
    lightDir = XMVector3Normalize(lightDir);

    // 4. 计算视锥体中心
    XMVECTOR centerVec = XMVectorZero();
    for (int i = 0; i < 8; ++i) {
        centerVec = XMVectorAdd(centerVec, XMLoadFloat3(&worldCorners[i]));
    }
    centerVec = XMVectorScale(centerVec, 1.0f / 8.0f);

    // 5. 构建光源视图矩阵
    float distance = MAX_RANGE * 1.5f;
    XMVECTOR lightPos = XMVectorSubtract(centerVec, XMVectorScale(lightDir, distance));
    XMVECTOR target = centerVec;
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX lightView = XMMatrixLookAtLH(lightPos, target, up);

    // 6. 将角点转换到光源空间，计算 AABB
    XMVECTOR lightSpaceMin = XMVectorSet(FLT_MAX, FLT_MAX, FLT_MAX, 0.0f);
    XMVECTOR lightSpaceMax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0.0f);

    for (int i = 0; i < 8; ++i) {
        XMVECTOR corner = XMLoadFloat3(&worldCorners[i]);
        XMVECTOR lightSpaceCorner = XMVector3TransformCoord(corner, lightView);
        lightSpaceMin = XMVectorMin(lightSpaceMin, lightSpaceCorner);
        lightSpaceMax = XMVectorMax(lightSpaceMax, lightSpaceCorner);
    }

    // 7. 扩大包围盒并限制范围
    XMVECTOR expand = XMVectorSet(EXPAND, EXPAND, EXPAND * 2.0f, 0.0f);
    lightSpaceMin = XMVectorSubtract(lightSpaceMin, expand);
    lightSpaceMax = XMVectorAdd(lightSpaceMax, expand);

    float l = XMVectorGetX(lightSpaceMin);
    float r = XMVectorGetX(lightSpaceMax);
    float b = XMVectorGetY(lightSpaceMin);
    float t = XMVectorGetY(lightSpaceMax);
    float n = XMVectorGetZ(lightSpaceMin);
    float f = XMVectorGetZ(lightSpaceMax);

    // 限制最大范围
    float rangeX = r - l;
    float rangeY = t - b;
    if (rangeX > MAX_RANGE) {
        float centerX = (l + r) * 0.5f;
        l = centerX - MAX_RANGE * 0.5f;
        r = centerX + MAX_RANGE * 0.5f;
    }
    if (rangeY > MAX_RANGE) {
        float centerY = (b + t) * 0.5f;
        b = centerY - MAX_RANGE * 0.5f;
        t = centerY + MAX_RANGE * 0.5f;
    }

    if (r <= l)
        r = l + 1.0f;
    if (t <= b)
        t = b + 1.0f;
    if (f <= n)
        f = n + 1.0f;

    // 8. 构建正交投影矩阵
    XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);
    XMMATRIX viewProj = XMMatrixMultiply(lightView, lightProj);
    XMStoreFloat4x4(&outParams.LightViewProj, viewProj);

    // 9. 参数设置
    outParams.ShadowMapSize = 2048.0f;
    outParams.Bias = 0.0001f;
    outParams.NormalBias = 0.0001f;
    outParams.ShadowStrength = 1.0f;
    outParams.ShadowMapIndex = 0;
}

void LightManager::ComputePointShadowMatrices(const Light &light, PointLightShadowConstants &outConstants) {
    using namespace DirectX;

    XMVECTOR lightPos = XMLoadFloat4(&light.Position);
    float range = light.Range > 0.0f ? light.Range : 10.0f;
    float nearZ = 1.0f;

    // 6 个面的方向 + 上向量
    static const XMVECTORF32 faceDirs[6] = {
        {{1, 0, 0, 0}}, {{-1, 0, 0, 0}}, {{0, 1, 0, 0}}, {{0, -1, 0, 0}}, {{0, 0, 1, 0}}, {{0, 0, -1, 0}},
    };
    static const XMVECTORF32 faceUps[6] = {
        {{0, 1, 0, 0}}, {{0, 1, 0, 0}}, {{0, 0, -1, 0}}, {{0, 0, 1, 0}}, {{0, 1, 0, 0}}, {{0, 1, 0, 0}},
    };

    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, nearZ, range);

    for (int face = 0; face < 6; ++face) {
        XMMATRIX view = XMMatrixLookAtLH(lightPos, XMVectorAdd(lightPos, faceDirs[face].v), faceUps[face].v);
        XMStoreFloat4x4(&outConstants.LightViewProj[face], XMMatrixMultiply(view, proj));
    }
}

void LightManager::ComputeSpotShadowMatrix(const Light &light, ShadowParams &outParams) {
    using namespace DirectX;

    XMVECTOR pos = XMLoadFloat4(&light.Position);
    XMVECTOR dir = XMLoadFloat4(&light.Direction);
    dir = XMVector3Normalize(dir);

    // 方向为零向量时跳过（防止 LookAtLH/PerspectiveFovLH 断言）
    if (XMVector3Equal(dir, XMVectorZero()))
        return;

    XMVECTOR target = XMVectorAdd(pos, dir);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (fabsf(XMVectorGetY(dir)) > 0.99f) {
        up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }

    XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
    float nearPlane = 0.1f;
    float farPlane = light.Range > 0.1f ? light.Range : 50.0f;
    // 确保 far > near，防止 XMMatrixPerspectiveFovLH 断言
    if (farPlane <= nearPlane + 0.001f)
        farPlane = nearPlane + 10.0f;
    float fov = XMConvertToRadians(45.0f); // 固定视场角 45°
    XMMATRIX proj = XMMatrixPerspectiveFovLH(fov, 1.0f, nearPlane, farPlane);
    XMMATRIX vp = XMMatrixMultiply(view, proj);

    XMStoreFloat4x4(&outParams.LightViewProj, vp);
    outParams.LightPosition = XMFLOAT3(light.Position.x, light.Position.y, light.Position.z);
    outParams.Range = light.Range;
    outParams.ShadowMapSize = 1024.0f;
    outParams.Bias = light.ShadowBias > 0.0f ? light.ShadowBias : 0.005f;
    outParams.NormalBias = 0.02f;
    outParams.ShadowStrength = light.CastShadow;
}

// ============================================================================
// 阴影贴图资源管理
// ============================================================================

/**
 * @brief 创建方向光阴影贴图
 * @param lightIndex
 * @param resolution
 * @date 2026-06-04
 */
void LightManager::CreateShadowMapForDirectionalLight(uint32_t lightIndex, uint32_t resolution,
                                                      uint64_t completeFence) {
    if (!m_initialized || !m_descriptorHeaps)
        return;

    auto &dsPool = DepthStencilPool::GetInstance();

    // 释放旧资源
    if (m_dirShadow.isValid) {
        ReleaseShadowMap(m_dirShadow, completeFence);
    }

    m_dirShadow = {};
    m_dirShadow.resolution = resolution;

    // 1. 通过 DepthStencilPool 创建深度纹理
    DepthStencilDesc dsDesc = {};
    dsDesc.width = resolution;
    dsDesc.height = resolution;
    dsDesc.format = DXGI_FORMAT_D32_FLOAT;
    dsDesc.arraySize = 1;
    dsDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    dsDesc.sampleDesc.Count = 1;
    dsDesc.name = L"ShadowMap_Depth";

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    dsDesc.clearValue = clearValue;

    m_dirShadow.handle = dsPool.Allocate(dsDesc, m_heapTag);
    if (!m_dirShadow.handle.IsValid()) {
        return;
    }

    ID3D12Resource *depthTexture = dsPool.GetResource(m_dirShadow.handle);

    // 2. 分配 DSV 槽并创建 DSV
    m_dirShadow.dsvSlot = m_descriptorHeaps->Allocate(PartitionType::Dsv);
    if (m_dirShadow.dsvSlot == UINT32_MAX) {
        dsPool.Free(m_dirShadow.handle, 0);
        m_dirShadow = {};
        return;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;
    m_device->CreateDepthStencilView(depthTexture, &dsvDesc,
                                     m_descriptorHeaps->GetCpuHandle(PartitionType::Dsv, m_dirShadow.dsvSlot));

    // 3. 分配 SRV 槽并创建 SRV（Shadow 分区，Texture2DArray 1 slice）
    m_dirShadow.srvSlot = m_descriptorHeaps->Allocate(PartitionType::Shadow);
    if (m_dirShadow.srvSlot == UINT32_MAX) {
        m_descriptorHeaps->Free(PartitionType::Dsv, m_dirShadow.dsvSlot, 0);
        dsPool.Free(m_dirShadow.handle, 0);
        m_dirShadow = {};
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = 1;
    m_device->CreateShaderResourceView(depthTexture, &srvDesc,
                                       m_descriptorHeaps->GetCpuHandle(PartitionType::Shadow, m_dirShadow.srvSlot));

    // 如果 Shadow 分区已有预分配槽位，也创建该槽位的 SRV（供 gShadowMaps[] 无界数组索引）
    if (m_shadowMapSrvDirSlot != UINT32_MAX) {
        m_device->CreateShaderResourceView(
            depthTexture, &srvDesc, m_descriptorHeaps->GetCpuHandle(PartitionType::Shadow, m_shadowMapSrvDirSlot));
    }

    m_dirShadow.isValid = true;
    m_shadowDirty = true;
}

void LightManager::CreateShadowMapForSpotLight(uint32_t lightIndex, uint32_t resolution, uint64_t completeFence) {
    if (!m_initialized || !m_descriptorHeaps)
        return;

    // 确保 shadow resources 数组大小匹配
    if (lightIndex >= m_spotShadowResources.size()) {
        m_spotShadowResources.resize(lightIndex + 1);
    }

    // 释放旧资源
    auto &prev = m_spotShadowResources[lightIndex];
    if (prev.isValid) {
        ReleaseShadowMap(prev, completeFence);
    }

    auto &shadow = m_spotShadowResources[lightIndex];
    shadow = {};
    shadow.resolution = resolution;

    auto &dsPool = DepthStencilPool::GetInstance();

    DepthStencilDesc dsDesc = {};
    dsDesc.width = resolution;
    dsDesc.height = resolution;
    dsDesc.format = DXGI_FORMAT_D32_FLOAT;
    dsDesc.arraySize = 1;
    dsDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    dsDesc.sampleDesc.Count = 1;
    dsDesc.name = L"SpotShadowMap_Depth";

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    dsDesc.clearValue = clearValue;

    shadow.handle = dsPool.Allocate(dsDesc, m_heapTag);
    if (!shadow.handle.IsValid())
        return;

    ID3D12Resource *depthTexture = dsPool.GetResource(shadow.handle);

    shadow.dsvSlot = m_descriptorHeaps->Allocate(PartitionType::Dsv);
    if (shadow.dsvSlot == UINT32_MAX) {
        dsPool.Free(shadow.handle, 0);
        shadow = {};
        return;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;
    m_device->CreateDepthStencilView(depthTexture, &dsvDesc,
                                     m_descriptorHeaps->GetCpuHandle(PartitionType::Dsv, shadow.dsvSlot));

    shadow.srvSlot = m_descriptorHeaps->Allocate(PartitionType::Shadow);
    if (shadow.srvSlot == UINT32_MAX) {
        m_descriptorHeaps->Free(PartitionType::Dsv, shadow.dsvSlot, 0);
        dsPool.Free(shadow.handle, 0);
        shadow = {};
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = 1;
    m_device->CreateShaderResourceView(depthTexture, &srvDesc,
                                       m_descriptorHeaps->GetCpuHandle(PartitionType::Shadow, shadow.srvSlot));

    shadow.isValid = true;
    m_shadowDirty = true;
}

void LightManager::CreateShadowMapForPointLight(uint32_t lightIndex, uint32_t resolution, uint64_t completeFence) {
    if (!m_initialized)
        return;

    // 确保 shadow resources 数组大小匹配
    if (lightIndex >= m_pointShadowResources.size()) {
        m_pointShadowResources.resize(lightIndex + 1);
    }

    // 释放旧资源
    auto &prev = m_pointShadowResources[lightIndex];
    if (prev.isValid) {
        // 释放逐 slice DSV 槽
        for (int f = 0; f < 6; ++f) {
            if (prev.dsvSlots[f] != UINT32_MAX)
                m_descriptorHeaps->Free(PartitionType::Dsv, prev.dsvSlots[f], 0);
            prev.dsvSlots[f] = UINT32_MAX;
        }
        // 释放数组纹理
        if (prev.arrayHandle.IsValid())
            DepthStencilPool::GetInstance().Free(prev.arrayHandle, completeFence);
        if (prev.srvBaseSlot != UINT32_MAX)
            m_descriptorHeaps->Free(PartitionType::Shadow, prev.srvBaseSlot, 0);
    }

    auto &shadow = m_pointShadowResources[lightIndex];
    shadow = {};
    for (auto &s : shadow.dsvSlots)
        s = UINT32_MAX;
    shadow.resolution = resolution;

    // 分配 2D 纹理数组（6 slice）作为点光源阴影贴图
    auto &dsPool = DepthStencilPool::GetInstance();
    DepthStencilDesc dsDesc = {};
    dsDesc.width = resolution;
    dsDesc.height = resolution;
    dsDesc.format = DXGI_FORMAT_D32_FLOAT;
    dsDesc.arraySize = 6;
    dsDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    dsDesc.sampleDesc.Count = 1;
    dsDesc.name = L"PointShadow_Array";

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    dsDesc.clearValue = clearValue;

    // 全数组 DSV（用于 ClearDepthStencilView）
    D3D12_DEPTH_STENCIL_VIEW_DESC arrayDsvDesc = {};
    arrayDsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    arrayDsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    arrayDsvDesc.Texture2DArray.MipSlice = 0;
    arrayDsvDesc.Texture2DArray.FirstArraySlice = 0;
    arrayDsvDesc.Texture2DArray.ArraySize = 6;

    shadow.arrayHandle = dsPool.Allocate(dsDesc, m_heapTag, &arrayDsvDesc);
    if (!shadow.arrayHandle.IsValid()) {
        return;
    }

    // 创建逐 slice DSV
    ID3D12Resource *resource = dsPool.GetResource(shadow.arrayHandle);
    for (int face = 0; face < 6; ++face) {
        shadow.dsvSlots[face] = m_descriptorHeaps->Allocate(PartitionType::Dsv);
        if (shadow.dsvSlots[face] == UINT32_MAX) {
            // 失败时释放已创建的
            for (int f = 0; f < face; ++f) {
                m_descriptorHeaps->Free(PartitionType::Dsv, shadow.dsvSlots[f], 0);
                shadow.dsvSlots[f] = UINT32_MAX;
            }
            dsPool.Free(shadow.arrayHandle, completeFence);
            shadow.arrayHandle = {};
            return;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
            m_descriptorHeaps->GetCpuHandle(PartitionType::Dsv, shadow.dsvSlots[face]);

        D3D12_DEPTH_STENCIL_VIEW_DESC perSliceDesc = {};
        perSliceDesc.Format = DXGI_FORMAT_D32_FLOAT;
        perSliceDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        perSliceDesc.Texture2DArray.MipSlice = 0;
        perSliceDesc.Texture2DArray.FirstArraySlice = face;
        perSliceDesc.Texture2DArray.ArraySize = 1;

        m_device->CreateDepthStencilView(resource, &perSliceDesc, dsvHandle);
    }

    // 分配 SRV 槽并创建 Texture2DArray SRV（6 slice，供 gShadowMaps[] 索引）
    {
        shadow.srvBaseSlot = m_descriptorHeaps->Allocate(PartitionType::Shadow);
        if (shadow.srvBaseSlot == UINT32_MAX) {
            // 失败回滚
            for (int f = 0; f < 6; ++f) {
                m_descriptorHeaps->Free(PartitionType::Dsv, shadow.dsvSlots[f], 0);
                shadow.dsvSlots[f] = UINT32_MAX;
            }
            dsPool.Free(shadow.arrayHandle, completeFence);
            shadow.arrayHandle = {};
            return;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = 6;
        m_device->CreateShaderResourceView(resource, &srvDesc,
                                           m_descriptorHeaps->GetCpuHandle(PartitionType::Shadow, shadow.srvBaseSlot));
    }

    shadow.isValid = true;
    m_shadowDirty = true;

    // 更新对应光源的 ShadowMapIndex，指向 Shadow 分区 SRV 基址
    if (lightIndex < m_pointLights.size()) {
        m_pointLights[lightIndex].ShadowMapIndex = static_cast<float>(shadow.srvBaseSlot);
        m_lightDirty = true;
    }
}

void LightManager::ReleaseShadowMap(DirShadowResources &shadow, uint64_t completedFence) {
    if (!shadow.isValid)
        return;

    auto &dsPool = DepthStencilPool::GetInstance();

    if (shadow.handle.IsValid()) {
        dsPool.Free(shadow.handle, completedFence);
    }
    if (shadow.dsvSlot != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(PartitionType::Dsv, shadow.dsvSlot, completedFence);
    }
    if (shadow.srvSlot != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(PartitionType::Shadow, shadow.srvSlot, completedFence);
    }

    shadow = {};
}

void LightManager::ReleaseShadowMap(SpotShadowResources &shadow, uint64_t completedFence) {
    if (!shadow.isValid)
        return;

    auto &dsPool = DepthStencilPool::GetInstance();

    if (shadow.handle.IsValid()) {
        dsPool.Free(shadow.handle, completedFence);
    }
    if (shadow.dsvSlot != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(PartitionType::Dsv, shadow.dsvSlot, completedFence);
    }
    if (shadow.srvSlot != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(PartitionType::Shadow, shadow.srvSlot, completedFence);
    }

    shadow = {};
}

void LightManager::ReleaseShadowMap(PointShadowResources &shadow, uint64_t completedFence) {
    if (!shadow.isValid)
        return;

    auto &dsPool = DepthStencilPool::GetInstance();

    for (int f = 0; f < 6; ++f) {
        if (shadow.dsvSlots[f] != UINT32_MAX && m_descriptorHeaps) {
            m_descriptorHeaps->Free(PartitionType::Dsv, shadow.dsvSlots[f], completedFence);
        }
        shadow.dsvSlots[f] = UINT32_MAX;
    }
    if (shadow.srvBaseSlot != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(PartitionType::Shadow, shadow.srvBaseSlot, completedFence);
    }
    if (shadow.arrayHandle.IsValid()) {
        dsPool.Free(shadow.arrayHandle, completedFence);
    }

    shadow = {};
}

bool LightManager::HasShadow(LightType type) const {
    switch (type) {
    case LightType::Directional:
        if (!m_dirShadow.isValid)
            return false;
        for (auto &l : m_dirLights)
            if (l.CastShadow > 0.5f)
                return true;
        return false;
    case LightType::Point:
        if (m_pointShadowResources.empty())
            return false;
        for (size_t i = 0; i < m_pointLights.size(); ++i)
            if (m_pointLights[i].CastShadow > 0.5f && i < m_pointShadowResources.size() &&
                m_pointShadowResources[i].isValid)
                return true;
        return false;
    case LightType::Spot:
        if (m_spotShadowResources.empty())
            return false;
        for (size_t i = 0; i < m_spotLights.size(); ++i)
            if (m_spotLights[i].CastShadow > 0.5f && i < m_spotShadowResources.size() &&
                m_spotShadowResources[i].isValid)
                return true;
        return false;
    default:
        return false;
    }
}

// ============================================================================
// 调试辅助
// ============================================================================

void LightManager::CreateTestLights() {
    Clear();

    // 设置环境光（微弱，便于观察阴影）
    m_lightConstants.AmbientLight = DirectX::XMFLOAT4{0.05f, 0.05f, 0.1f, 1.0f};

    // 方向光 0
    {
        Light dirLight = {};
        dirLight.Strength = DirectX::XMFLOAT4(4.5f, 4.0f, 3.5f, 0.0f);
        dirLight.Direction = DirectX::XMFLOAT4(0.4f, -0.85f, 0.35f, 0.0f);
        dirLight.ShadowMapIndex = 0;
        dirLight.CastShadow = 1.0f;
        SetDirectionalLight(dirLight, 0);
    }

    // 暖色点光源（投射阴影，位于地面中心正上方）
    {
        Light pointLight0 = {};
        pointLight0.Strength = DirectX::XMFLOAT4(25.0f, 18.0f, 8.0f, 0.0f);
        pointLight0.Position = DirectX::XMFLOAT4(0.0f, 35.0f, 0.0f, 0.0f);
        pointLight0.FalloffStart = 5.0f;
        pointLight0.FalloffEnd = 30.0f;
        pointLight0.Range = 20.0f;
        pointLight0.CastShadow = 1.0f;
        pointLight0.ShadowBias = 0.005f;
        AddPointLight(pointLight0);
    }

    // 聚光灯（从右上前方斜照，投射阴影）
    Light spotLight = {};
    spotLight.Strength = DirectX::XMFLOAT4(200.0f, 180.0f, 120.0f, 0.0f);
    spotLight.Position = DirectX::XMFLOAT4(15.0f, 45.0f, -15.0f, 0.0f);
    spotLight.Direction = DirectX::XMFLOAT4(-0.3f, -1.0f, 0.3f, 0.0f);
    spotLight.FalloffStart = 1.0f;
    spotLight.FalloffEnd = 30.0f;
    spotLight.Range = 30.0f;
    spotLight.SpotPower = 8.0f; // 锥体衰减指数
    spotLight.CastShadow = 1.0f;
    spotLight.ShadowBias = 0.005f;
    AddSpotLight(spotLight);

    // // 冷色点光源（左前方）— 注释，只保留主点光源
    // Light pointLight1 = {};
    // pointLight1.Strength = DirectX::XMFLOAT4(0.3f, 0.6f, 1.0f, 0.0f);
    // pointLight1.Position = DirectX::XMFLOAT4(-3.0f, 2.0f, 4.0f, 0.0f);
    // pointLight1.FalloffStart = 1.0f;
    // pointLight1.FalloffEnd = 20.0f;
    // pointLight1.Range = 20.0f;
    // AddPointLight(pointLight1);
    //
    // // 背光补光
    // Light backLight = {};
    // backLight.Strength = DirectX::XMFLOAT4(0.5f, 0.5f, 0.8f, 0.0f);
    // backLight.Position = DirectX::XMFLOAT4(0.0f, 3.0f, -5.0f, 0.0f);
    // backLight.FalloffStart = 1.0f;
    // backLight.FalloffEnd = 15.0f;
    // backLight.Range = 15.0f;
    // AddPointLight(backLight);
    //
    // // 跟随相机的光源
    // Light followLight = {};
    // followLight.Strength = DirectX::XMFLOAT4(0.8f, 0.8f, 1.0f, 0.0f);
    // followLight.Position = DirectX::XMFLOAT4(0.0f, 3.0f, 0.0f, 0.0f);
    // followLight.Direction = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    // followLight.FalloffStart = 0.5f;
    // followLight.FalloffEnd = 10.0f;
    // followLight.Range = 10.0f;
    // AddPointLight(followLight);
}

} // namespace DX12Engine::Renderer