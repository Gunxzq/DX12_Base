#include "CullingLayer.h"

#include <cstring> // memcpy（桶偏移表 UPLOAD 填充）

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

// ========================================================================
// CullingLayer 实现（P0 实装，2026-08-10）
//
// 门面转发到三层模块（SpatialHashGrid / CullingDataStore /
// CullingResourceManager / CullingRenderer）。Editor/Builder 调用点改走门面，
// 旧系统（InstanceCullingBuffer/OctreeSystem）保留作参考，稳定后移除。
// ========================================================================

namespace DX12Engine {
namespace Culling {

CullingLayer &CullingLayer::GetInstance() {
    static CullingLayer instance;
    return instance;
}

void CullingLayer::Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descHeaps,
                              Resource::HeapTag heapTag) {
    m_initialized = (device != nullptr && descHeaps != nullptr);
    m_resources.Initialize(device, descHeaps, heapTag);
    m_renderer.Initialize(device, descHeaps, heapTag, &m_dataStore, &m_resources);
}

void CullingLayer::Shutdown() {
    m_renderer.Shutdown();
    m_resources.Shutdown();
    m_dataStore.Clear();
    m_spatialIndex.Shutdown();
    m_world = nullptr;
    m_initialized = false;
}

void CullingLayer::SetPredictedFrustum(const Renderer::Frustum &frustum) {
    m_predictedFrustum = frustum;
    // P0：空间粗筛查询输入（OctreeCullingSystem 的 QueryFrustum 消费）
}

void CullingLayer::BindWorld(ECS::World &world) {
    m_world = &world;
    // P0：绑定后由空间索引内部遍历（方案 C），多 World → sceneId 映射
}

void CullingLayer::RegisterSystems(Scheduler::FrameDriver &driver) {
    // P0：注册 PreCulling 空间粗筛 + Render 阶段 CS dispatch（task name 唯一化）
    // 实现细节由 Editor 的 RegisterEditorRenderSystems 迁移（见 Editor.cpp 调用点注释）
}

const char *CullingLayer::GetTaskNameSpatialQuery() { return "CullingSystem/OctreeQuery"; }
const char *CullingLayer::GetTaskNameCSDispatch() { return "CullingSystem/CSDispatch"; }

void CullingLayer::Upload(Renderer::FrameResourceManager *frameResMgr) { m_renderer.Upload(frameResMgr); }

void CullingLayer::SetFlatInstances(D3D12_GPU_VIRTUAL_ADDRESS instanceSegmentAddr, ID3D12Resource *instanceRes,
                                    uint32_t instanceCount, std::vector<uint32_t> &&bucketMap, uint64_t releaseFence) {
    // 数据扁平化（数据层）→ 容量守卫（资源层扩容）
    const uint32_t appendNeed =
        m_dataStore.SetFlatInstances(instanceSegmentAddr, instanceRes, instanceCount, std::move(bucketMap));
    if (appendNeed * sizeof(uint32_t) > m_resources.GetAppendCapacity() && m_initialized) {
        // releaseFence：旧 AppendBuffer 延迟释放（GPU 完成本帧 dispatch 后——#921 修复）
        m_resources.ResizeAppendBuffer(m_dataStore.GetInstanceCount(), m_dataStore.GetBucketMap(), releaseFence);
    }
    // 桶偏移表 UPLOAD 填充（DispatchCulling 每帧 COPY 到 gIndirectArgs 尾部）
    if (m_resources.GetBucketOffsetsUp().IsValid()) {
        auto *res = Resource::GpuResourceManager::GetInstance().GetResource(m_resources.GetBucketOffsetsUp());
        if (res) {
            void *mapped = nullptr;
            if (SUCCEEDED(res->Map(0, nullptr, &mapped))) {
                const auto &offsets = m_dataStore.GetBucketOffsets();
                memcpy(mapped, offsets.data(), sizeof(uint32_t) * offsets.size());
                res->Unmap(0, nullptr);
            }
        }
    }
}

void CullingLayer::EndFrame(uint64_t fence) { m_renderer.EndFrame(fence); }

const Renderer::CulledSet &CullingLayer::GetCoarseSet() const {
    // 粗筛结果缓存（Editor OctreeCullingSystem 查询/合并后经 SetCoarseSet 注入）
    return m_coarseSet;
}

D3D12_GPU_DESCRIPTOR_HANDLE CullingLayer::GetInstanceSRV() const { return m_renderer.GetInstanceSRV(); }

D3D12_GPU_DESCRIPTOR_HANDLE CullingLayer::GetBucketMapSRV() const { return m_renderer.GetBucketMapSRV(); }

D3D12_GPU_VIRTUAL_ADDRESS CullingLayer::GetBufferAddress() const { return m_dataStore.GetInstanceSegmentAddr(); }

const std::vector<uint32_t> &CullingLayer::GetBucketOffsets() const { return m_dataStore.GetBucketOffsets(); }

uint32_t CullingLayer::GetInstanceCount() const { return m_dataStore.GetInstanceCount(); }

uint32_t CullingLayer::GetBucketOffset(uint32_t bucketIndex) const { return m_dataStore.GetBucketOffset(bucketIndex); }

size_t CullingLayer::GetBucketMapSize() const { return m_dataStore.GetBucketMapSize(); }

uint32_t CullingLayer::GetLastVisibleCount() const { return m_renderer.GetLastVisibleCount(); }

D3D12_GPU_VIRTUAL_ADDRESS CullingLayer::GetAppendBufferAddress() const { return m_resources.GetAppendBufferAddress(); }

ID3D12Resource *CullingLayer::GetIndirectArgsResource() const { return m_resources.GetIndirectArgsResource(); }

void CullingLayer::SetBucketDrawArgs(uint32_t bucketIndex, uint32_t subMeshCount,
                                     const RenderItemCommon::SubMeshRange *ranges) {
    m_resources.SetBucketDrawArgs(bucketIndex, subMeshCount, ranges);
}

bool CullingLayer::CreateCullingPipeline(Renderer::CommandManager *cmdMgr) {
    return m_renderer.CreateCullingPipeline(cmdMgr);
}

void CullingLayer::DispatchCulling(Renderer::CommandList &cmd, const DirectX::XMVECTOR *planes,
                                   D3D12_GPU_DESCRIPTOR_HANDLE instanceSRV) {
    m_renderer.DispatchCulling(cmd, planes, instanceSRV);
}

bool CullingLayer::IsCullingReady() const { return m_renderer.IsCullingReady(); }

bool CullingLayer::IsValid() const { return m_renderer.IsValid(); }

} // namespace Culling
} // namespace DX12Engine
