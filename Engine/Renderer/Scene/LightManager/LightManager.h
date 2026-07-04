#pragma once

#include "LightResourceTypes.h"
#include "Renderer/FrameResources/RingBuffer.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Renderer/Scene/Camera.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "Resource/Pool/DepthStencilPool.h"
#include "Resource/Pool/RenderTargetPool.h"
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
    Resource::DepthStencilHandle handle;    // 来自 DepthStencilPool
    uint32_t dsvSlot;                        // 手动分配 DSV 槽
    uint32_t srvSlot;                        // 手动分配 SRV 槽 (Shadow分区)
    uint32_t resolution;
    bool isValid;
};

// 点光源阴影贴图（2D 纹理数组 6 slice，兼容 6 遍渲染 / GS / View Instancing）
struct PointShadowResources {
    Resource::DepthStencilHandle arrayHandle; // DepthStencilPool 句柄 (arraySize=6)
    uint32_t dsvSlots[6];                     // 逐 slice DSV 描述符槽（手动分配）
    uint32_t srvBaseSlot = UINT32_MAX;        // Shadow 分区 6 连续 SRV 槽基址
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

    void UpdateAndUpload(uint64_t fence, const Camera &camera);

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
    D3D12_GPU_VIRTUAL_ADDRESS GetShadowSampleAddress() const { return m_shadowSampleAddress; }

    // 方向光阴影渲染常量地址（供 ShadowRenderer b1 使用）
    D3D12_GPU_VIRTUAL_ADDRESS GetDirShadowRenderAddress() const { return m_dirShadowRenderAddress; }

    // 点光源阴影完整 cbPointShadow 地址（View Instancing VS 通过 SV_ViewID 选取矩阵，仅供渲染 Pass，非采样）
    D3D12_GPU_VIRTUAL_ADDRESS GetPointShadowAddress() const { return m_pointShadowAddress; }

    // 点光源阴影 — 逐面 256 字节对齐的 CBV 地址（旧 6 遍渲染过渡用）
    D3D12_GPU_VIRTUAL_ADDRESS GetPointShadowFaceAddress(uint32_t face) const {
        return (face < 6) ? m_pointShadowFaceAddress[face] : 0;
    }

    // ========================================================================
    // 阴影贴图资源访问（供 ShadowRenderer / OpaqueRenderer 使用）
    // ========================================================================
    const DirShadowResources &GetDirShadowResources() const { return m_dirShadow; }
    const std::vector<PointShadowResources> &GetPointShadowResources() const { return m_pointShadowResources; }

    // 阴影数据 StructuredBuffer 的 GPU 描述符句柄 (t11, space1)
    D3D12_GPU_DESCRIPTOR_HANDLE GetShadowDataSRV() const { return m_shadowDataSRV; }

    // 阴影贴图纹理的 GPU 描述符句柄 (t14, space1)
    D3D12_GPU_DESCRIPTOR_HANDLE GetShadowMapSRV() const { return m_shadowMapSRV; }

    // 描述符堆槽位查询（调试用）
    uint32_t GetShadowDataSrvSlot() const { return m_shadowDataSrvBaseSlot; }
    uint32_t GetShadowMapSrvDirSlot() const { return m_shadowMapSrvDirSlot; }

    // 注释：旧访问接口已移除，使用 m_shadowParams 统一访问

    bool HasDirShadow() const { return m_dirShadow.isValid && !m_shadowParams.empty(); }
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
    void CreateShadowMapForDirectionalLight(uint32_t lightIndex, uint32_t resolution, uint64_t completeFence);
    void CreateShadowMapForPointLight(uint32_t lightIndex, uint32_t resolution, uint64_t completeFence);
    // void CreateShadowMapForSpotLight(uint32_t lightIndex, uint32_t resolution);
    void ReleaseShadowMap(DirShadowResources &shadow, uint64_t fence);
    // void ReleaseShadowMap(PointShadowResources &shadow, uint64_t fence);
    // void ReleaseShadowMap(SpotShadowResources &shadow, uint64_t fence);

private:
    void RebuildLightConstants();

    // 重建阴影常量数组
    void RebuildShadowConstants(const Camera &camera);

    // 计算单个光源的阴影 VP 矩阵
    void ComputeDirShadowMatrix(const Light &light, ShadowParams &outParams, const Camera &camera);
    void ComputePointShadowMatrices(const Light &light, PointLightShadowConstants &outConstants);
    // void ComputeSpotShadowMatrix(const Light &light, SpotLightShadowConstants &outConstants);

private:
    // 光源常量
    std::vector<Light> m_pointLights;
    std::vector<Light> m_spotLights;
    std::vector<Light> m_dirLights;

    // 阴影常量（统一结构体，覆盖方向光与点光源采样参数）
    std::vector<ShadowParams> m_shadowParams;
    std::vector<DirLightShadowConstants> m_dirShadowCBConstants;
    std::vector<PointLightShadowConstants> m_pointShadowConstants;
    // std::vector<SpotLightShadowConstants> m_spotShadowConstants;
    LightConstants m_lightConstants = {}; // 光源常量数据

    // GPU 地址
    D3D12_GPU_VIRTUAL_ADDRESS m_lightCBAddress = 0;             // 光源常量缓冲区地址
    D3D12_GPU_VIRTUAL_ADDRESS m_shadowSampleAddress = 0;        // 阴影采样参数缓冲区地址 (gShadowParams)
    D3D12_GPU_VIRTUAL_ADDRESS m_dirShadowRenderAddress = 0;     // 方向光阴影渲染常量（ShadowRenderer b1）
    D3D12_GPU_VIRTUAL_ADDRESS m_pointShadowAddress = 0;         // 点光源阴影完整 cbPointShadow（渲染 Pass）
    D3D12_GPU_VIRTUAL_ADDRESS m_pointShadowFaceAddress[6] = {}; // 逐面 256 对齐 CBV（过渡用）
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
    RingBuffer m_shadowSampleBuffer;    // 统一阴影采样参数 (ShadowParams)
    RingBuffer m_dirShadowRenderBuffer; // 方向光阴影渲染常量 (ShadowRenderer b1)
    RingBuffer m_pointShadowBuffer;     // 点光源阴影 VP 矩阵（渲染 Pass）
    // RingBuffer m_spotShadowBuffer;

    // 阴影贴图资源（单张，非 vector）
    DirShadowResources m_dirShadow;
    std::vector<PointShadowResources> m_pointShadowResources;
    // std::vector<SpotShadowResources> m_spotShadowResources;

    // 阴影数据/贴图 SRV
    D3D12_GPU_DESCRIPTOR_HANDLE m_shadowDataSRV = {}; // t11 StructuredBuffer (ShadowParams)
    D3D12_GPU_DESCRIPTOR_HANDLE m_shadowMapSRV = {};  // t14 Texture2D (阴影贴图)

    // 阴影采样参数 StructuredBuffer 资源句柄 (UPLOAD 堆)
    Resource::GpuResourceHandle m_shadowParamsBufferHandle = {};

    // SRV 描述符槽位
    uint32_t m_shadowDataSrvBaseSlot = UINT32_MAX; // t11 基础槽位
    uint32_t m_shadowMapSrvDirSlot = UINT32_MAX;   // t14 方向光阴影贴图槽位
};
} // namespace DX12Engine::Renderer