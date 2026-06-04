#include "LightManager.h"
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

void LightManager::Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;
    m_descriptorHeaps = descriptorHeaps;
    Clear();

    // 初始化内部 RingBuffer
    m_lightBuffer.Initialize(device, DEFAULT_LIGHT_BUFFER_SIZE);
    m_dirShadowBuffer.Initialize(device, DEFAULT_SHADOW_BUFFER_SIZE);

    // 创建阴影数据 StructuredBuffer (UPLOAD 堆，每帧 UpdateAndUpload 写入)
    {
        size_t dirSize = MAX_LIGHTS * sizeof(DirLightShadowConstants);
        m_dirShadowDataBufferHandle = GpuResourceManager::GetInstance().CreateBuffer(
            device, dirSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        // 分配 SRV 槽位：t11 (DirShadowData)
        m_shadowDataSrvBaseSlot = descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);
        if (m_shadowDataSrvBaseSlot != UINT32_MAX) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

            // t11: DirShadowData
            srvDesc.Buffer.NumElements = MAX_LIGHTS;
            srvDesc.Buffer.StructureByteStride = sizeof(DirLightShadowConstants);
            device->CreateShaderResourceView(
                GpuResourceManager::GetInstance().GetResource(m_dirShadowDataBufferHandle), &srvDesc,
                descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, m_shadowDataSrvBaseSlot));

            m_shadowDataSRV = descriptorHeaps->GetGpuHandle(DescriptorHeapType::CbvSrvUav, m_shadowDataSrvBaseSlot);
        }
    }

    // 预分配阴影贴图纹理 SRV 槽位（t14=Dir）
    {
        m_shadowMapSrvDirSlot = descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);
        if (m_shadowMapSrvDirSlot != UINT32_MAX) {
            m_shadowMapSRV = descriptorHeaps->GetGpuHandle(DescriptorHeapType::CbvSrvUav, m_shadowMapSrvDirSlot);
            // SRV 的具体内容在 CreateShadowMapForDirectionalLight 中创建/更新
        }
    }

    m_initialized = true;
}

void LightManager::Shutdown() {
    // 释放阴影贴图资源
    if (m_dirShadow.isValid) {
        ReleaseShadowMap(m_dirShadow, 0);
    }

    // 释放阴影数据 StructuredBuffer
    auto &gpuMgr = GpuResourceManager::GetInstance();
    if (m_dirShadowDataBufferHandle.IsValid()) {
        gpuMgr.Release(m_dirShadowDataBufferHandle, 0);
        m_dirShadowDataBufferHandle = {};
    }

    // 回收 GpuResourceManager 和 DescriptorHeap 的延迟释放
    if (m_descriptorHeaps) {
        m_descriptorHeaps->Reclaim(DescriptorHeapType::Dsv, 0);
        m_descriptorHeaps->Reclaim(DescriptorHeapType::CbvSrvUav, 0);

        // 释放阴影数据 SRV 槽
        if (m_shadowDataSrvBaseSlot != UINT32_MAX) {
            m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, m_shadowDataSrvBaseSlot, 0);
            m_shadowDataSrvBaseSlot = UINT32_MAX;
        }

        // 释放阴影贴图 SRV 槽
        if (m_shadowMapSrvDirSlot != UINT32_MAX) {
            m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, m_shadowMapSrvDirSlot, 0);
            m_shadowMapSrvDirSlot = UINT32_MAX;
        }
    }

    m_lightBuffer.Shutdown();
    m_dirShadowBuffer.Shutdown();

    m_lightCBAddress = 0;
    m_dirShadowAddress = 0;
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
void LightManager::UpdateAndUpload(uint64_t fence, const DirectX::XMFLOAT3 &cameraPos) {
    if (!m_initialized)
        return;

    // 如果光源数据有变化，重建常量
    if (m_lightDirty) {
        RebuildLightConstants();
    }

    // if (m_shadowDirty) {
    RebuildShadowConstants(cameraPos);
    // }

    // 只在脏数据变化时才上传，光源数据是低频更新的
    if (m_lightDirty) {
        m_lightCBAddress = m_lightBuffer.AllocateUpload(&m_lightConstants, sizeof(LightConstants), fence);
        m_lightDirty = false;
    }

    // if (m_shadowDirty) {
    if (!m_dirShadowConstants.empty()) {
        m_dirShadowAddress = m_dirShadowBuffer.AllocateUpload(
            m_dirShadowConstants.data(),
            static_cast<uint32_t>(m_dirShadowConstants.size() * sizeof(DirLightShadowConstants)), fence);

        // 同步写入阴影数据 StructuredBuffer（UPLOAD 堆，直接 Map 写入）
        if (m_dirShadowDataBufferHandle.IsValid()) {
            ID3D12Resource *resource = GpuResourceManager::GetInstance().GetResource(m_dirShadowDataBufferHandle);
            if (resource) {
                void *mapped = nullptr;
                resource->Map(0, nullptr, &mapped);
                memcpy(mapped, m_dirShadowConstants.data(),
                       m_dirShadowConstants.size() * sizeof(DirLightShadowConstants));
                resource->Unmap(0, nullptr);
            }
        }
    }
    //     if (!m_pointShadowConstants.empty()) {
    //         m_pointShadowAddress = m_pointShadowBuffer.AllocateUpload(
    //             m_pointShadowConstants.data(),
    //             static_cast<uint32_t>(m_pointShadowConstants.size() * sizeof(PointLightShadowConstants)), fence);

    //         // 同步写入 Point Shadow StructuredBuffer
    //         if (m_pointShadowDataBufferHandle.IsValid()) {
    //             ID3D12Resource *resource =
    //             GpuResourceManager::GetInstance().GetResource(m_pointShadowDataBufferHandle); if (resource) {
    //                 void *mapped = nullptr;
    //                 resource->Map(0, nullptr, &mapped);
    //                 memcpy(mapped, m_pointShadowConstants.data(),
    //                        m_pointShadowConstants.size() * sizeof(PointLightShadowConstants));
    //                 resource->Unmap(0, nullptr);
    //             }
    //         }
    //     }
    //     if (!m_spotShadowConstants.empty()) {
    //         m_spotShadowAddress = m_spotShadowBuffer.AllocateUpload(
    //             m_spotShadowConstants.data(),
    //             static_cast<uint32_t>(m_spotShadowConstants.size() * sizeof(SpotLightShadowConstants)), fence);

    //         // 同步写入 Spot Shadow StructuredBuffer
    //         if (m_spotShadowDataBufferHandle.IsValid()) {
    //             ID3D12Resource *resource =
    //             GpuResourceManager::GetInstance().GetResource(m_spotShadowDataBufferHandle); if (resource) {
    //                 void *mapped = nullptr;
    //                 resource->Map(0, nullptr, &mapped);
    //                 memcpy(mapped, m_spotShadowConstants.data(),
    //                        m_spotShadowConstants.size() * sizeof(SpotLightShadowConstants));
    //                 resource->Unmap(0, nullptr);
    //             }
    //         }
    //     }
    //     m_shadowDirty = false;
    // }

    // 回收
    m_lightBuffer.Reclaim(fence);
    m_dirShadowBuffer.Reclaim(fence);
    // m_pointShadowBuffer.Reclaim(fence);
    // m_spotShadowBuffer.Reclaim(fence);
}

// ============================================================================
// 光源设置
// ============================================================================

void LightManager::Clear() {
    // // 释放阴影贴图资源
    // if (m_dirShadow.isValid) {
    //     ReleaseShadowMap(m_dirShadow, 0);
    // }
    // for (auto &res : m_pointShadowResources) {
    //     if (res.isValid) {
    //         ReleaseShadowMap(res, 0);
    //     }
    // }
    // for (auto &res : m_spotShadowResources) {
    //     if (res.isValid) {
    //         ReleaseShadowMap(res, 0);
    //     }
    // }
    // m_pointShadowResources.clear();
    // m_spotShadowResources.clear();

    memset(&m_lightConstants, 0, sizeof(LightConstants));
    m_dirLights.clear();
    m_pointLights.clear();
    m_spotLights.clear();

    m_dirShadowConstants.clear();
    // m_pointShadowConstants.clear();
    // m_spotShadowConstants.clear();
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
    m_dirLights[index] = fixedLight;
    m_lightDirty = true;
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
        m_pointLights[index] = light;
        m_lightDirty = true;
    }
}

void LightManager::RemovePointLight(uint32_t index) {
    if (index < m_pointLights.size()) {
        // 释放对应的阴影贴图（fence 传 0 表示立即标记待释放，由 RingBuffer 的后续 Reclaim 回收）
        // if (index < m_pointShadowResources.size() && m_pointShadowResources[index].isValid) {
        //     ReleaseShadowMap(m_pointShadowResources[index], 0);
        // }
        // m_pointLights.erase(m_pointLights.begin() + index);
        // if (index < m_pointShadowResources.size()) {
        //     m_pointShadowResources.erase(m_pointShadowResources.begin() + index);
        // }
        m_lightDirty = true;
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
        m_spotLights[index] = light;
        m_lightDirty = true;
    }
}

void LightManager::RemoveSpotLight(uint32_t index) {
    if (index < m_spotLights.size()) {
        // 释放对应的阴影贴图
        // if (index < m_spotShadowResources.size() && m_spotShadowResources[index].isValid) {
        //     ReleaseShadowMap(m_spotShadowResources[index], 0);
        // }
        // m_spotLights.erase(m_spotLights.begin() + index);
        // if (index < m_spotShadowResources.size()) {
        //     m_spotShadowResources.erase(m_spotShadowResources.begin() + index);
        // }
        m_lightDirty = true;
    }
}

// void LightManager::SetShadowParams(uint32_t lightIndex, LightType type, float bias, float normalBias, float strength,
//                                    uint32_t resolution) {
//     switch (type) {
//     case LightType::Directional:
//         if (lightIndex < m_dirShadowConstants.size()) {
//             auto &c = m_dirShadowConstants[lightIndex];
//             c.Bias = bias;
//             c.NormalBias = normalBias;
//             c.ShadowStrength = strength;
//             c.ShadowMapSize = static_cast<float>(resolution);
//             m_shadowDirty = true;
//         }
//         break;
//     case LightType::Point:
//         if (lightIndex < m_pointShadowConstants.size()) {
//             auto &c = m_pointShadowConstants[lightIndex];
//             c.Bias = bias;
//             c.NormalBias = normalBias;
//             c.ShadowStrength = strength;
//             c.ShadowMapSize = static_cast<float>(resolution);
//             m_shadowDirty = true;
//         }
//         break;
//     case LightType::Spot:
//         if (lightIndex < m_spotShadowConstants.size()) {
//             auto &c = m_spotShadowConstants[lightIndex];
//             c.Bias = bias;
//             c.NormalBias = normalBias;
//             c.ShadowStrength = strength;
//             c.ShadowMapSize = static_cast<float>(resolution);
//             m_shadowDirty = true;
//         }
//         break;
//     }
// }

// ============================================================================
// 数据访问
// ============================================================================

Light *LightManager::GetDirectionalLight(uint32_t index) {
    if (index < m_dirLights.size()) {
        return &m_dirLights[index];
    }
    return nullptr;
}

Light *LightManager::GetPointLight(uint32_t index) {
    if (index < m_pointLights.size()) {
        return &m_pointLights[index];
    }
    return nullptr;
}

Light *LightManager::GetSpotLight(uint32_t index) {
    if (index < m_spotLights.size()) {
        return &m_spotLights[index];
    }
    return nullptr;
}

// ============================================================================
// 内部实现 — 重建常量数组
// ============================================================================

void LightManager::RebuildLightConstants() {
    uint32_t offset = 0;

    // 复制方向光到 Lights[0..NumDirLights-1]
    m_lightConstants.NumDirLights = static_cast<uint32_t>(m_dirLights.size());
    for (size_t i = 0; i < m_dirLights.size() && offset < MAX_LIGHTS; ++i) {
        m_lightConstants.Lights[offset++] = m_dirLights[i];
    }

    // 复制点光源
    m_lightConstants.NumPointLights = static_cast<uint32_t>(m_pointLights.size());
    for (size_t i = 0; i < m_pointLights.size() && offset < MAX_LIGHTS; ++i) {
        m_lightConstants.Lights[offset++] = m_pointLights[i];
    }

    // 复制聚光灯
    m_lightConstants.NumSpotLights = static_cast<uint32_t>(m_spotLights.size());
    for (size_t i = 0; i < m_spotLights.size() && offset < MAX_LIGHTS; ++i) {
        m_lightConstants.Lights[offset++] = m_spotLights[i];
    }

    // 注意：不在此处清除 m_lightDirty，由 UpdateAndUpload 在上传完成后统一清除
}

void LightManager::RebuildShadowConstants(const DirectX::XMFLOAT3 &cameraPos) {
    // 方向光阴影常量
    m_dirShadowConstants.clear();
    for (size_t i = 0; i < m_dirLights.size(); ++i) {
        DirLightShadowConstants constants = {};
        ComputeDirShadowMatrix(m_dirLights[i], constants, cameraPos);
        // 设置 ShadowMapIndex：gDirShadowMaps[] 纹理数组索引（目前只有一张方向光阴影贴图，索引为 0）
        constants.ShadowMapIndex = m_dirShadow.isValid ? 0u : UINT32_MAX;
        m_dirShadowConstants.push_back(constants);
    }

    // // 点光源阴影常量
    // m_pointShadowConstants.clear();
    // for (size_t i = 0; i < m_pointLights.size(); ++i) {
    //     PointLightShadowConstants constants = {};
    //     ComputePointShadowMatrices(m_pointLights[i], constants);
    //     // 设置 ShadowMapIndex
    //     if (i < m_pointShadowResources.size() && m_pointShadowResources[i].isValid) {
    //         constants.ShadowMapIndex = static_cast<int>(m_pointShadowResources[i].srvSlot);
    //     }
    //     m_pointShadowConstants.push_back(constants);
    // }

    // // 聚光灯阴影常量
    // m_spotShadowConstants.clear();
    // for (size_t i = 0; i < m_spotLights.size(); ++i) {
    //     SpotLightShadowConstants constants = {};
    //     ComputeSpotShadowMatrix(m_spotLights[i], constants);
    //     // 设置 ShadowMapIndex
    //     if (i < m_spotShadowResources.size() && m_spotShadowResources[i].isValid) {
    //         constants.ShadowMapIndex = static_cast<int>(m_spotShadowResources[i].srvSlot);
    //     }
    //     m_spotShadowConstants.push_back(constants);
    // }
}

void LightManager::ComputeDirShadowMatrix(const Light &light, DirLightShadowConstants &outConstants,
                                          const DirectX::XMFLOAT3 &cameraPos) {
    using namespace DirectX;

    XMVECTOR lightDir = XMLoadFloat4(&light.Direction);
    lightDir = XMVector3Normalize(lightDir);

    // ================================================================
    // 固定场景中心，不跟相机走
    // ================================================================
    XMFLOAT3 sceneCenter(0.0f, 8.0f, 0.0f); // 你的场景中心
    XMVECTOR centerVec = XMLoadFloat3(&sceneCenter);

    // 光源位置：场景中心向光源反方向移动
    float distance = 150.0f; // 足够远以覆盖整个场景
    XMVECTOR lightPos = XMVectorSubtract(centerVec, XMVectorScale(lightDir, distance));

    // 目标点：场景中心（固定）
    XMVECTOR target = centerVec;
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(lightPos, target, up);

    // 正交投影：覆盖场景范围
    float halfSize = 25.0f; // 覆盖 X:-25~25, Z:-25~25
    float nearPlane = 0.1f;
    float farPlane = 300.0f; // 足够远
    XMMATRIX proj = XMMatrixOrthographicLH(halfSize * 2.0f, halfSize * 2.0f, nearPlane, farPlane);

    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMStoreFloat4x4(&outConstants.LightViewProj, viewProj);

    outConstants.ShadowMapSize = 2048.0f;
    outConstants.Bias = 0.0005f;
    outConstants.NormalBias = 0.001f;
    outConstants.ShadowStrength = 1.0f;
    outConstants.ShadowMapIndex = 0;
}

// void LightManager::ComputePointShadowMatrices(const Light &light, PointLightShadowConstants &outConstants) {
//     using namespace DirectX;

//     // 显式构造 pos 并强制 .w = 0，避免 XMLoadFloat4 带入非零 .w 污染向量运算
//     XMVECTOR pos = XMVectorSet(light.Position.x, light.Position.y, light.Position.z, 0.0f);

//     // 防御：如果位置包含 NaN 或 Inf，使用原点
//     if (XMVector3IsNaN(pos) || XMVector3IsInfinite(pos)) {
//         pos = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
//     }

//     XMStoreFloat3(&outConstants.LightPosition, pos);

//     // 6 个面的方向
//     static const XMVECTORF32 directions[6] = {
//         {{1.0f, 0.0f, 0.0f, 0.0f}},  // +X
//         {{-1.0f, 0.0f, 0.0f, 0.0f}}, // -X
//         {{0.0f, 1.0f, 0.0f, 0.0f}},  // +Y
//         {{0.0f, -1.0f, 0.0f, 0.0f}}, // -Y
//         {{0.0f, 0.0f, 1.0f, 0.0f}},  // +Z
//         {{0.0f, 0.0f, -1.0f, 0.0f}}, // -Z
//     };
//     static const XMVECTORF32 ups[6] = {
//         {{0.0f, 1.0f, 0.0f, 0.0f}}, {{0.0f, 1.0f, 0.0f, 0.0f}}, {{0.0f, 0.0f, -1.0f, 0.0f}},
//         {{0.0f, 0.0f, 1.0f, 0.0f}}, {{0.0f, 1.0f, 0.0f, 0.0f}}, {{0.0f, 1.0f, 0.0f, 0.0f}},
//     };

//     float nearPlane = 0.1f;
//     float farPlane = light.Range > 0.0f ? light.Range : 50.0f;
//     XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(90.0f), 1.0f, nearPlane, farPlane);

//     for (int i = 0; i < 6; ++i) {
//         XMVECTOR target = XMVectorAdd(pos, directions[i]);
//         XMVECTOR eyeDir = XMVectorSubtract(target, pos);
//         // 防御：确保 EyeDirection 非零
//         if (XMVector3Equal(eyeDir, XMVectorZero())) {
//             eyeDir = directions[i]; // fallback 到方向向量
//         }
//         XMMATRIX view = XMMatrixLookAtLH(pos, XMVectorAdd(pos, eyeDir), ups[i]);
//         XMMATRIX vp = XMMatrixMultiply(view, proj);
//         XMStoreFloat4x4(&outConstants.LightViewProj[i], XMMatrixTranspose(vp));
//     }

//     outConstants.ShadowMapSize = 1024.0f;
//     outConstants.Bias = 0.005f;
//     outConstants.NormalBias = 0.02f;
//     outConstants.ShadowStrength = 1.0f;
//     outConstants.Range = light.Range;
//     outConstants.ShadowMapIndex = -1;
// }

// void LightManager::ComputeSpotShadowMatrix(const Light &light, SpotLightShadowConstants &outConstants) {
//     using namespace DirectX;

//     XMVECTOR pos = XMLoadFloat4(&light.Position);
//     XMVECTOR dir = XMLoadFloat4(&light.Direction);
//     dir = XMVector3Normalize(dir);

//     XMVECTOR target = XMVectorAdd(pos, dir);
//     XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
//     if (fabsf(XMVectorGetY(dir)) > 0.99f) {
//         up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
//     }

//     XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
//     float nearPlane = 0.1f;
//     float farPlane = light.Range > 0.0f ? light.Range : 50.0f;
//     XMMATRIX proj = XMMatrixPerspectiveFovLH(light.SpotPower > 0.0f ? light.SpotPower :
//     XMConvertToRadians(45.0f), 1.0f,
//                                              nearPlane, farPlane);
//     XMMATRIX vp = XMMatrixMultiply(view, proj);

//     XMStoreFloat4x4(&outConstants.LightViewProj, XMMatrixTranspose(vp));
//     outConstants.ShadowMapSize = 2048.0f;
//     outConstants.Bias = 0.005f;
//     outConstants.NormalBias = 0.02f;
//     outConstants.ShadowStrength = 1.0f;
//     outConstants.SpotPower = light.SpotPower;
//     outConstants.ShadowMapIndex = -1;
// }

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

    auto &gpuMgr = GpuResourceManager::GetInstance();

    // 释放旧资源
    if (m_dirShadow.isValid) {
        ReleaseShadowMap(m_dirShadow, completeFence);
    }

    m_dirShadow = {};
    m_dirShadow.resolution = resolution;

    // 1. 创建 2D 深度纹理
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = resolution;
    desc.Height = resolution;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS; // 32-bit depth
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    m_dirShadow.textureHandle = gpuMgr.CreateTexture2D(m_device, desc, clearValue,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!m_dirShadow.textureHandle.IsValid()) {
        return;
    }

    // 2. 分配 DSV 槽并创建 DSV
    m_dirShadow.dsvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::Dsv);
    if (m_dirShadow.dsvSlot == UINT32_MAX) {
        gpuMgr.Release(m_dirShadow.textureHandle, 0);
        m_dirShadow = {};
        return;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;
    m_device->CreateDepthStencilView(gpuMgr.GetResource(m_dirShadow.textureHandle), &dsvDesc,
                                     m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Dsv, m_dirShadow.dsvSlot));

    // 3. 分配 SRV 槽并创建 SRV
    m_dirShadow.srvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);
    if (m_dirShadow.srvSlot == UINT32_MAX) {
        m_descriptorHeaps->Free(DescriptorHeapType::Dsv, m_dirShadow.dsvSlot, 0);
        gpuMgr.Release(m_dirShadow.textureHandle, 0);
        m_dirShadow = {};
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(
        gpuMgr.GetResource(m_dirShadow.textureHandle), &srvDesc,
        m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, m_dirShadow.srvSlot));

    // 同时在 t14 槽位创建 SRV（供 OpaqueRenderer 根签名 slot 7 使用）
    // 注：不能使用 CopyDescriptorsSimple，因为 Shader Visible 堆的 CPU 端是只读的
    if (m_shadowMapSrvDirSlot != UINT32_MAX) {
        m_device->CreateShaderResourceView(
            gpuMgr.GetResource(m_dirShadow.textureHandle), &srvDesc,
            m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav, m_shadowMapSrvDirSlot));
    }

    m_dirShadow.isValid = true;
    m_shadowDirty = true;
}

// void LightManager::CreateShadowMapForPointLight(uint32_t lightIndex, uint32_t resolution) {
//     if (!m_initialized || !m_descriptorHeaps)
//         return;

//     auto &gpuMgr = GpuResourceManager::GetInstance();

//     // 确保 shadow resources 数组大小匹配
//     if (lightIndex >= m_pointShadowResources.size()) {
//         m_pointShadowResources.resize(lightIndex + 1);
//     }

//     // 释放旧资源
//     if (m_pointShadowResources[lightIndex].isValid) {
//         ReleaseShadowMap(m_pointShadowResources[lightIndex], 0);
//     }

//     auto &shadow = m_pointShadowResources[lightIndex];
//     shadow = {};
//     shadow.resolution = resolution;

//     // 1. 创建 CubeMap 纹理（ArraySize=6）
//     D3D12_RESOURCE_DESC desc = {};
//     desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
//     desc.Width = resolution;
//     desc.Height = resolution;
//     desc.DepthOrArraySize = 6; // CubeMap 6 面
//     desc.MipLevels = 1;
//     desc.Format = DXGI_FORMAT_R32_TYPELESS;
//     desc.SampleDesc.Count = 1;
//     desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
//     desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

//     D3D12_CLEAR_VALUE clearValue = {};
//     clearValue.Format = DXGI_FORMAT_D32_FLOAT;
//     clearValue.DepthStencil.Depth = 1.0f;

//     shadow.textureHandle = gpuMgr.CreateTexture2D(m_device, desc, D3D12_RESOURCE_STATE_DEPTH_WRITE);
//     if (!shadow.textureHandle.IsValid()) {
//         return;
//     }

//     // 2. 分配 DSV 槽（整个 cube 用 1 个 DSV，渲染时用 DSVDesc 指定面）
//     shadow.dsvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::Dsv);
//     if (shadow.dsvSlot == UINT32_MAX) {
//         gpuMgr.Release(shadow.textureHandle, 0);
//         shadow = {};
//         return;
//     }
//     D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
//     dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
//     dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
//     dsvDesc.Texture2DArray.FirstArraySlice = 0;
//     dsvDesc.Texture2DArray.ArraySize = 6;
//     dsvDesc.Texture2DArray.MipSlice = 0;
//     m_device->CreateDepthStencilView(gpuMgr.GetResource(shadow.textureHandle), &dsvDesc,
//                                      m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Dsv, shadow.dsvSlot));

//     // 3. 分配 SRV 槽并创建 Cube SRV
//     shadow.srvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);
//     if (shadow.srvSlot == UINT32_MAX) {
//         m_descriptorHeaps->Free(DescriptorHeapType::Dsv, shadow.dsvSlot, 0);
//         gpuMgr.Release(shadow.textureHandle, 0);
//         shadow = {};
//         return;
//     }
//     D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
//     srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
//     srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
//     srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
//     srvDesc.TextureCube.MostDetailedMip = 0;
//     srvDesc.TextureCube.MipLevels = 1;
//     m_device->CreateShaderResourceView(gpuMgr.GetResource(shadow.textureHandle), &srvDesc,
//                                        m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav,
//                                        shadow.srvSlot));

//     shadow.isValid = true;
//     m_shadowDirty = true;
// }

// void LightManager::CreateShadowMapForSpotLight(uint32_t lightIndex, uint32_t resolution) {
//     if (!m_initialized || !m_descriptorHeaps)
//         return;

//     auto &gpuMgr = GpuResourceManager::GetInstance();

//     // 确保 shadow resources 数组大小匹配
//     if (lightIndex >= m_spotShadowResources.size()) {
//         m_spotShadowResources.resize(lightIndex + 1);
//     }

//     // 释放旧资源
//     if (m_spotShadowResources[lightIndex].isValid) {
//         ReleaseShadowMap(m_spotShadowResources[lightIndex], 0);
//     }

//     auto &shadow = m_spotShadowResources[lightIndex];
//     shadow = {};
//     shadow.resolution = resolution;

//     // 1. 创建 2D 深度纹理
//     D3D12_RESOURCE_DESC desc = {};
//     desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
//     desc.Width = resolution;
//     desc.Height = resolution;
//     desc.DepthOrArraySize = 1;
//     desc.MipLevels = 1;
//     desc.Format = DXGI_FORMAT_R32_TYPELESS;
//     desc.SampleDesc.Count = 1;
//     desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
//     desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

//     D3D12_CLEAR_VALUE clearValue = {};
//     clearValue.Format = DXGI_FORMAT_D32_FLOAT;
//     clearValue.DepthStencil.Depth = 1.0f;

//     shadow.textureHandle = gpuMgr.CreateTexture2D(m_device, desc, D3D12_RESOURCE_STATE_DEPTH_WRITE);
//     if (!shadow.textureHandle.IsValid()) {
//         return;
//     }

//     // 2. 分配 DSV 槽并创建 DSV
//     shadow.dsvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::Dsv);
//     if (shadow.dsvSlot == UINT32_MAX) {
//         gpuMgr.Release(shadow.textureHandle, 0);
//         shadow = {};
//         return;
//     }
//     D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
//     dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
//     dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
//     dsvDesc.Texture2D.MipSlice = 0;
//     m_device->CreateDepthStencilView(gpuMgr.GetResource(shadow.textureHandle), &dsvDesc,
//                                      m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::Dsv, shadow.dsvSlot));

//     // 3. 分配 SRV 槽并创建 SRV
//     shadow.srvSlot = m_descriptorHeaps->Allocate(DescriptorHeapType::CbvSrvUav);
//     if (shadow.srvSlot == UINT32_MAX) {
//         m_descriptorHeaps->Free(DescriptorHeapType::Dsv, shadow.dsvSlot, 0);
//         gpuMgr.Release(shadow.textureHandle, 0);
//         shadow = {};
//         return;
//     }
//     D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
//     srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
//     srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
//     srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
//     srvDesc.Texture2D.MipLevels = 1;
//     m_device->CreateShaderResourceView(gpuMgr.GetResource(shadow.textureHandle), &srvDesc,
//                                        m_descriptorHeaps->GetCpuHandle(DescriptorHeapType::CbvSrvUav,
//                                        shadow.srvSlot));

//     shadow.isValid = true;
//     m_shadowDirty = true;
// }

void LightManager::ReleaseShadowMap(DirShadowResources &shadow, uint64_t completedFence) {
    if (!shadow.isValid)
        return;

    auto &gpuMgr = GpuResourceManager::GetInstance();

    if (shadow.textureHandle.IsValid()) {
        gpuMgr.Release(shadow.textureHandle, completedFence);
    }
    if (shadow.dsvSlot != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(DescriptorHeapType::Dsv, shadow.dsvSlot, completedFence);
    }
    if (shadow.srvSlot != UINT32_MAX && m_descriptorHeaps) {
        m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, shadow.srvSlot, completedFence);
    }

    shadow = {};
}

// void LightManager::ReleaseShadowMap(PointShadowResources &shadow, uint64_t fence) {
//     if (!shadow.isValid)
//         return;

//     auto &gpuMgr = GpuResourceManager::GetInstance();

//     if (shadow.textureHandle.IsValid()) {
//         gpuMgr.Release(shadow.textureHandle, fence);
//     }
//     if (shadow.dsvSlot != UINT32_MAX && m_descriptorHeaps) {
//         m_descriptorHeaps->Free(DescriptorHeapType::Dsv, shadow.dsvSlot, fence);
//     }
//     if (shadow.srvSlot != UINT32_MAX && m_descriptorHeaps) {
//         m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, shadow.srvSlot, fence);
//     }

//     shadow = {};
// }

// void LightManager::ReleaseShadowMap(SpotShadowResources &shadow, uint64_t fence) {
//     if (!shadow.isValid)
//         return;

//     auto &gpuMgr = GpuResourceManager::GetInstance();

//     if (shadow.textureHandle.IsValid()) {
//         gpuMgr.Release(shadow.textureHandle, fence);
//     }
//     if (shadow.dsvSlot != UINT32_MAX && m_descriptorHeaps) {
//         m_descriptorHeaps->Free(DescriptorHeapType::Dsv, shadow.dsvSlot, fence);
//     }
//     if (shadow.srvSlot != UINT32_MAX && m_descriptorHeaps) {
//         m_descriptorHeaps->Free(DescriptorHeapType::CbvSrvUav, shadow.srvSlot, fence);
//     }

//     shadow = {};
// }

// ============================================================================
// 调试辅助
// ============================================================================

void LightManager::CreateTestLights() {
    Clear();

    // 设置环境光
    m_lightConstants.AmbientLight = DirectX::XMFLOAT4{0.4f, 0.45f, 0.5f, 1.0f}; // 环境光-暖色

    // ========================================================================
    // 方向光 0 — 模拟太阳光（从左上方斜照，产生明显阴影）
    // ========================================================================
    Light dirLight = {};
    dirLight.Strength = DirectX::XMFLOAT4(5.0f, 4.5f, 4.0f, 0.0f);     // 更强的暖色太阳光
    dirLight.Direction = DirectX::XMFLOAT4(0.4f, -0.85f, 0.35f, 0.0f); // 左上方斜照（约 30° 仰角）
    dirLight.ShadowMapIndex = 0; // 使用 gDirShadows[0]，表示该光源投射阴影
    SetDirectionalLight(dirLight, 0);

    // 暖色点光源（右前方）
    Light pointLight0 = {};
    pointLight0.Strength = DirectX::XMFLOAT4(1.0f, 0.6f, 0.3f, 0.0f);
    pointLight0.Position = DirectX::XMFLOAT4(3.0f, 2.0f, 4.0f, 0.0f);
    pointLight0.FalloffStart = 1.0f;
    pointLight0.FalloffEnd = 20.0f;
    pointLight0.Range = 20.0f;
    AddPointLight(pointLight0);

    // 冷色点光源（左前方）
    Light pointLight1 = {};
    pointLight1.Strength = DirectX::XMFLOAT4(0.3f, 0.6f, 1.0f, 0.0f);
    pointLight1.Position = DirectX::XMFLOAT4(-3.0f, 2.0f, 4.0f, 0.0f);
    pointLight1.FalloffStart = 1.0f;
    pointLight1.FalloffEnd = 20.0f;
    pointLight1.Range = 20.0f;
    AddPointLight(pointLight1);

    // 背光补光
    Light backLight = {};
    backLight.Strength = DirectX::XMFLOAT4(0.5f, 0.5f, 0.8f, 0.0f);
    backLight.Position = DirectX::XMFLOAT4(0.0f, 3.0f, -5.0f, 0.0f);
    backLight.FalloffStart = 1.0f;
    backLight.FalloffEnd = 15.0f;
    backLight.Range = 15.0f;
    AddPointLight(backLight);

    // 跟随相机的光源
    Light followLight = {};
    followLight.Strength = DirectX::XMFLOAT4(0.8f, 0.8f, 1.0f, 0.0f);
    followLight.Position = DirectX::XMFLOAT4(0.0f, 3.0f, 0.0f, 0.0f);
    followLight.Direction = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    followLight.FalloffStart = 0.5f;
    followLight.FalloffEnd = 10.0f;
    followLight.Range = 10.0f;
    AddPointLight(followLight);
}

} // namespace DX12Engine::Renderer