#pragma once

#include "LightResourceTypes.h"
#include "Renderer/FrameResources/RingBuffer.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Resource/Struct/ResourceHandle.h"
#include <d3d12.h>
#include <unordered_map>
#include <vector>

namespace DX12Engine::Resource {
class DescriptorHeapCollection;
class GpuResourceManager;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

enum class LightType : uint8_t { Directional, Point, Spot };

// 方向光阴影贴图（单张）
struct DirShadowResources {
    Resource::GpuResourceHandle textureHandle; // 来自 GpuResourceManager
    uint32_t dsvSlot;                          // 来自 DescriptorSlotAllocator (DSV堆)
    uint32_t srvSlot;                          // 来自 DescriptorSlotAllocator (SRV堆)
    uint32_t resolution;
    bool isValid;
};

// 点光源阴影贴图（立方体贴图，6 个面）
struct PointShadowResources {
    Resource::GpuResourceHandle textureHandle; // 立方体贴图资源
    uint32_t dsvSlot;                          // 可能需要 6 个 DSV 或使用 RTV 数组
    uint32_t srvSlot;                          // TextureCube SRV
    uint32_t resolution;
    bool isValid;
};

// 聚光源阴影贴图（单张）
struct SpotShadowResources {
    Resource::GpuResourceHandle textureHandle;
    uint32_t dsvSlot;
    uint32_t srvSlot;
    uint32_t resolution;
    bool isValid;
};

// ============================================================================
// 光源管理器 - 管理场景中的静态光源数据
// ============================================================================

class LightManager {

public:
    static LightManager &GetInstance();

    LightManager(const LightManager &) = delete;
    LightManager &operator=(const LightManager &) = delete;
    LightManager() = default;
    ~LightManager() = default;

    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descriptorHeaps);
    void Shutdown();

    void UpdateAndUpload(uint64_t fence, const DirectX::XMFLOAT3 &cameraPos = DirectX::XMFLOAT3(0, 0, 0));

    void Clear();

    void SetDirectionalLight(const Light &light, uint32_t index = 0);
    void SetAmbientLight(const DirectX::XMFLOAT4 &light);

    uint32_t AddPointLight(const Light &light);
    void SetPointLight(uint32_t index, const Light &light);
    void RemovePointLight(uint32_t index);

    uint32_t AddSpotLight(const Light &light);
    void SetSpotLight(uint32_t index, const Light &light);
    void RemoveSpotLight(uint32_t index);

    // void SetShadowParams(uint32_t lightIndex, LightType type, float bias, float normalBias, float strength,
    //                      uint32_t resolution);

    // ========================================================================
    // 数据访问
    // ========================================================================
    const LightConstants &GetLightConstants() const { return m_lightConstants; }
    LightConstants &GetLightConstants() { return m_lightConstants; }

    uint32_t GetDirectionalLightCount() const { return m_lightConstants.NumDirLights; }
    uint32_t GetPointLightCount() const { return m_lightConstants.NumPointLights; }
    uint32_t GetSpotLightCount() const { return m_lightConstants.NumSpotLights; }

    Light *GetDirectionalLight(uint32_t index = 0);
    Light *GetPointLight(uint32_t index);
    Light *GetSpotLight(uint32_t index);

    // ========================================================================
    // GPU 上传
    // ========================================================================
    D3D12_GPU_VIRTUAL_ADDRESS GetLightCBAddress() const { return m_lightCBAddress; }
    // D3D12_GPU_VIRTUAL_ADDRESS GetDirShadowAddress() const { return m_dirShadowAddress; }
    // D3D12_GPU_VIRTUAL_ADDRESS GetPointShadowAddress() const { return m_pointShadowAddress; }
    // D3D12_GPU_VIRTUAL_ADDRESS GetSpotShadowAddress() const { return m_spotShadowAddress; }

    // // ========================================================================
    // // 阴影贴图资源访问（供 ShadowRenderer 使用）
    // // ========================================================================
    // const DirShadowResources &GetDirShadowResources() const { return m_dirShadow; }
    // DirShadowResources &GetDirShadowResources() { return m_dirShadow; }
    // const PointShadowResources &GetPointShadowResources(uint32_t index) const { return m_pointShadowResources[index];
    // } const SpotShadowResources &GetSpotShadowResources(uint32_t index) const { return m_spotShadowResources[index];
    // }

    // // 阴影数据 StructuredBuffer 的 GPU 描述符句柄 (t11~t13, space1)
    // D3D12_GPU_DESCRIPTOR_HANDLE GetShadowDataSRV() const { return m_shadowDataSRV; }

    // // 阴影贴图纹理数组的 GPU 描述符句柄 (t14/t20/t26, space1)
    // D3D12_GPU_DESCRIPTOR_HANDLE GetShadowMapSRV() const { return m_shadowMapSRV; }

    // const std::vector<DirLightShadowConstants> &GetDirShadowConstants() const { return m_dirShadowConstants; }
    // const std::vector<PointLightShadowConstants> &GetPointShadowConstants() const { return m_pointShadowConstants; }
    // const std::vector<SpotLightShadowConstants> &GetSpotShadowConstants() const { return m_spotShadowConstants; }

    // bool HasDirShadow() const { return m_dirShadow.isValid && !m_dirShadowConstants.empty(); }
    // bool HasPointShadow(uint32_t index) const {
    //     return index < m_pointShadowResources.size() && m_pointShadowResources[index].isValid &&
    //            index < m_pointShadowConstants.size();
    // }
    // bool HasSpotShadow(uint32_t index) const {
    //     return index < m_spotShadowResources.size() && m_spotShadowResources[index].isValid &&
    //            index < m_spotShadowConstants.size();
    // }

    // 调试辅助
    // ========================================================================
    void CreateTestLights();

    // 阴影贴图
    // void CreateShadowMapForDirectionalLight(uint32_t lightIndex, uint32_t resolution);
    // void CreateShadowMapForPointLight(uint32_t lightIndex, uint32_t resolution);
    // void CreateShadowMapForSpotLight(uint32_t lightIndex, uint32_t resolution);
    // void ReleaseShadowMap(DirShadowResources &shadow, uint64_t fence);
    // void ReleaseShadowMap(PointShadowResources &shadow, uint64_t fence);
    // void ReleaseShadowMap(SpotShadowResources &shadow, uint64_t fence);

private:
    void RebuildLightConstants();

    // // 重建阴影常量数组
    // void RebuildShadowConstants(const DirectX::XMFLOAT3 &cameraPos);

    // // 计算单个光源的阴影 VP 矩阵
    // void ComputeDirShadowMatrix(const Light &light, DirLightShadowConstants &outConstants,
    //                             const DirectX::XMFLOAT3 &cameraPos);
    // void ComputePointShadowMatrices(const Light &light, PointLightShadowConstants &outConstants);
    // void ComputeSpotShadowMatrix(const Light &light, SpotLightShadowConstants &outConstants);

private:
    // 光源常量
    std::vector<Light> m_pointLights;
    std::vector<Light> m_spotLights;
    std::vector<Light> m_dirLights;

    // 阴影常量
    // std::vector<DirLightShadowConstants> m_dirShadowConstants;
    // std::vector<PointLightShadowConstants> m_pointShadowConstants;
    // std::vector<SpotLightShadowConstants> m_spotShadowConstants;
    LightConstants m_lightConstants = {}; // 光源常量数据

    // GPU 地址
    D3D12_GPU_VIRTUAL_ADDRESS m_lightCBAddress = 0; // 光源常量缓冲区地址
    // D3D12_GPU_VIRTUAL_ADDRESS m_dirShadowAddress = 0;   // 方向光阴影缓冲区地址
    // D3D12_GPU_VIRTUAL_ADDRESS m_pointShadowAddress = 0; // 点光源阴影缓冲区地址
    // D3D12_GPU_VIRTUAL_ADDRESS m_spotShadowAddress = 0;  // 聚光灯阴影缓冲区地址

    // 脏标记
    bool m_lightDirty = true;
    bool m_shadowDirty = true;

    // 设备
    ID3D12Device *m_device = nullptr;
    Resource::DescriptorHeapCollection *m_descriptorHeaps = nullptr;
    bool m_initialized = false;

    // 内部 RingBuffer
    RingBuffer m_lightBuffer;
    // RingBuffer m_dirShadowBuffer;
    // RingBuffer m_pointShadowBuffer;
    // RingBuffer m_spotShadowBuffer;

    // 阴影贴图资源（单张，非 vector）
    // DirShadowResources m_dirShadow;
    // std::vector<PointShadowResources> m_pointShadowResources;
    // std::vector<SpotShadowResources> m_spotShadowResources;

    // // 阴影数据/贴图 SRV（供 OpaqueRenderer 绑定到根签名 slot 6, slot 7）
    // D3D12_GPU_DESCRIPTOR_HANDLE m_shadowDataSRV = {}; // t11~t13 StructuredBuffer
    // D3D12_GPU_DESCRIPTOR_HANDLE m_shadowMapSRV = {};  // t14/t20/t26 TextureArray

    // // 阴影数据 StructuredBuffer 资源句柄 (UPLOAD 堆)
    // Resource::GpuResourceHandle m_dirShadowDataBufferHandle = {};
    // Resource::GpuResourceHandle m_pointShadowDataBufferHandle = {};
    // Resource::GpuResourceHandle m_spotShadowDataBufferHandle = {};

    // SRV 描述符槽位
    uint32_t m_shadowDataSrvBaseSlot = UINT32_MAX; // t11 基础槽位（t11, t12, t13 连续）
    uint32_t m_shadowMapSrvDirSlot = UINT32_MAX;   // t14 方向光阴影贴图槽位
};
} // namespace DX12Engine::Renderer