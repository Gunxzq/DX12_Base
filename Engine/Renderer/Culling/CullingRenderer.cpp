#include "CullingRenderer.h"

#include "Common/d3dUtil.h" // CD3DX12_RESOURCE_BARRIER / CD3DX12_ROOT_SIGNATURE_DESC
#include "Logger/Logger.h"
#include "Renderer/Utils/ShaderUtils.h" // CompileShaderFromFile
#include <cassert>
#include <cstring> // memcpy

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

// ========================================================================
// CullingRenderer 实现（P3 迁移，2026-08-10）
//
// 迁移自 InstanceCullingBuffer（InstanceCullingBuffer.cpp）：
//   Upload（65-106）/ CreateComputePipeline（631-687）/ CreateCullingPipeline（689-720）/
//   EndFrame（315-329）/ DispatchCulling（722-970）/ readback（976-1024）/
//   CreateSRV/CreateBucketMapSRV（349-397）。
// 资源句柄经资源层（CullingResourceManager）获取，数据经数据层（CullingDataStore）。
// 旧系统（InstanceCullingBuffer）保留作参考，稳定后移除。
// ========================================================================

namespace DX12Engine {
namespace Culling {

void CullingRenderer::Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descHeaps,
                                 Resource::HeapTag heapTag, CullingDataStore *dataStore,
                                 CullingResourceManager *resources) {
    m_device = device;
    m_descHeaps = descHeaps;
    m_heapTag = heapTag;
    m_dataStore = dataStore;
    m_resources = resources;
    m_initialized = (device != nullptr && descHeaps != nullptr);
}

void CullingRenderer::Shutdown() {
    // 释放本帧临时 SRV 槽位（与 InstanceCullingBuffer::Shutdown 一致）
    if (m_descHeaps && m_srvIndex != UINT32_MAX) {
        m_descHeaps->Free(m_heapTag, PartitionType::Buffer, m_srvIndex, UINT64_MAX);
        m_srvIndex = UINT32_MAX;
    }
    if (m_descHeaps && m_bucketMapSrvIndex != UINT32_MAX) {
        m_descHeaps->Free(m_heapTag, PartitionType::Buffer, m_bucketMapSrvIndex, UINT64_MAX);
        m_bucketMapSrvIndex = UINT32_MAX;
    }
    m_bucketMapSegmentAddr = 0;
    m_computeRootSig.Reset();
    m_computePSO.Reset();
    m_lastVisibleCount = 0;
    m_hasDispatched = false;
    m_device = nullptr;
    m_descHeaps = nullptr;
    m_initialized = false;
}

bool CullingRenderer::Upload(Renderer::FrameResourceManager *frameResMgr) {
    // CullData 上传（2026-08-10 拆分定案——上传能力在剔除层）：CS 剔除只读精简 CullData
    // （worldPos/boundingRadius/globalInstanceIndex + 桶段，48B Cache 友好），不再读全量 InstanceData。
    // 数据由 Editor FrameSync 生成（门面 SetCullData → CullingDataStore），此处上传为 CS gCullData。
    if (!m_initialized || !frameResMgr || !m_dataStore)
        return false;
    const auto &cullData = m_dataStore->GetCullData();
    if (cullData.empty())
        return false;

    // 临时 SRV 槽位（本帧 dispatch 绑定，EndFrame 释放；heapTag 与使用方堆域一致——规则 17）
    if (m_srvIndex != UINT32_MAX) {
        m_descHeaps->Free(m_heapTag, PartitionType::Buffer, m_srvIndex, UINT64_MAX);
        m_srvIndex = UINT32_MAX;
    }
    m_srvIndex = frameResMgr->AllocateTemporarySrvSlot(m_heapTag);
    if (m_srvIndex == UINT32_MAX) {
        Logger::Logger::GetInstance()->Error("[CullingRenderer] Buffer SRV partition exhausted");
        return false;
    }

    // CullData 自管 RingBuffer 写入（2026-08-10 剥离：不再经 FrameResourceManager 帧段——
    // 规避 fence 回收依赖/共享 RingBuffer 竞争；段地址固定，每帧 Map 重写，消除频闪竞态）。
    // 类比 LightManager shadowParams 自管 UPLOAD 缓冲模式（生命周期自管）。
    const uint32_t cullBytes = static_cast<uint32_t>(cullData.size() * sizeof(CullData));
    if (!m_resources->GetCullDataBuffer().IsValid() && !m_resources->CreateCullDataResources())
        return false;
    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();
    auto *cullRes = gpuMgr.GetResource(m_resources->GetCullDataBuffer());
    if (!cullRes)
        return false;
    void *mapped = nullptr;
    if (FAILED(cullRes->Map(0, nullptr, &mapped)) || !mapped) {
        Logger::Logger::GetInstance()->Error("[CullingRenderer] CullData Map failed (buffer dangling?)");
        return false;
    }
    memcpy(mapped, cullData.data(), cullBytes);
    cullRes->Unmap(0, nullptr);
    CreateCullDataSRV(cullRes, m_resources->GetCullDataBufferAddr()); // 固定地址（FirstElement=0，段不滚动）

    // 方案 B：上传 EntityBucketMap（扁平桶归属，uint32 数组）到同一 RingBuffer 并创建 SRV（t1 槽位）
    const auto &bucketMap = m_dataStore->GetBucketMap();
    if (!bucketMap.empty()) {
        const uint32_t mapBytes = static_cast<uint32_t>(bucketMap.size() * sizeof(uint32_t));
        m_bucketMapSegmentAddr = frameResMgr->Allocate("InstanceCulling", bucketMap.data(), mapBytes);
        if (m_bucketMapSegmentAddr == 0) {
            Logger::Logger::GetInstance()->Error(
                "[CullingRenderer] InstanceCulling bucketMap RingBuffer alloc failed ({}B)", mapBytes);
            return false;
        }
        if (m_bucketMapSrvIndex != UINT32_MAX) {
            m_descHeaps->Free(m_heapTag, PartitionType::Buffer, m_bucketMapSrvIndex, UINT64_MAX);
            m_bucketMapSrvIndex = UINT32_MAX;
        }
        m_bucketMapSrvIndex = frameResMgr->AllocateTemporarySrvSlot(m_heapTag);
        if (m_bucketMapSrvIndex == UINT32_MAX) {
            Logger::Logger::GetInstance()->Error("[CullingRenderer] bucketMap SRV partition exhausted");
            return false;
        }
        CreateBucketMapSRV(frameResMgr->GetBufferResource("InstanceCulling"), m_bucketMapSegmentAddr);
    }
    return true;
}

void CullingRenderer::EndFrame(uint64_t fence) {
    if (m_descHeaps && m_srvIndex != UINT32_MAX) {
        m_descHeaps->Free(m_heapTag, PartitionType::Buffer, m_srvIndex, fence);
        m_srvIndex = UINT32_MAX;
    }
    if (m_descHeaps && m_bucketMapSrvIndex != UINT32_MAX) {
        m_descHeaps->Free(m_heapTag, PartitionType::Buffer, m_bucketMapSrvIndex, fence);
        m_bucketMapSrvIndex = UINT32_MAX;
    }
    // 段地址不在帧末清零（2026-08-10 修复）：m_instanceSegmentAddr/m_bucketMapSegmentAddr 指向
    // FrameResourceManager RingBuffer 当前段，生命周期由帧 fence 管理（段在 fence 完成前不回收）。
    // 此前清零导致下一帧立即回调的 Upload 守卫（段地址 == 0）return false →
    // SRV 未创建 → dispatch 永 skipReady → CS 未执行 → IndirectDrawIndexed(<0,0>) 画面空
    // （WireframeDebugDraw.md:146 规范：立即回调上传 → 渲染阶段消费）。
}

bool CullingRenderer::CreateCullingPipeline(Renderer::CommandManager *cmdMgr) {
    if (!m_initialized || !m_device || !m_descHeaps || !m_dataStore || !m_resources)
        return false;
    // 0 实例（如 async_test.scene：只有 camera，无块实体）：释放旧场景残留的 L2b 资源
    // （PSO/UAV/CullParams），否则 IsCullingReady() 残留 true → dispatch 绑定已释放的 CBV → GBV #961/TDR
    if (m_dataStore->GetInstanceCount() == 0) {
        m_resources->ReleaseCullingResources();
        m_computeRootSig.Reset();
        m_computePSO.Reset();
        return false;
    }
    if (!m_resources->CreateUAVs(m_dataStore->GetInstanceCount(), m_dataStore->GetBucketMap()))
        return false;
    if (!CreateComputePipeline()) {
        Logger::Logger::GetInstance()->Error("[CullingRenderer] L2b compute pipeline creation failed");
        return false;
    }
    // 预创建 CullParams UPLOAD 缓冲（主线程，避免 DispatchCulling 首次惰性创建在 Render 线程
    // 与后台 gpuWork CreateBuffer 并发；对齐 SkyboxManager AllocateObjectCB 持久 UPLOAD 模式）
    if (!m_resources->GetCullParamsUp().IsValid()) {
        m_resources->SetCullParamsUp(Resource::GpuResourceManager::GetInstance().CreateBuffer(
            m_device, 256, L"InstanceCulling_CullParams", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ));
    }
    // 分配确认日志（只读本层句柄字段）：valid=false 说明 CreateBuffer 返回 Invalid，
    // 后续 DispatchCulling 必然 cbAddr=0 跳帧——用于区分"从未创建"与"创建后被释放"
    {
        const uint32_t raw = static_cast<uint32_t>(m_resources->GetCullParamsUp());
        const uint32_t idx = raw & 0x3FFFFFu;      // 低 22 位 index
        const uint32_t gen = (raw >> 22) & 0x3FFu; // 高 10 位 generation
        Logger::Logger::GetInstance()->Info(
            "[CullingRenderer] CullParams buffer allocated: valid={} handle=0x{:08X} (index={}, generation={})",
            m_resources->GetCullParamsUp().IsValid(), raw, idx, gen);
    }
    return true;
}

bool CullingRenderer::CreateComputePipeline() {
    if (!m_device)
        return false;

    // ── 编译剔除 compute shader（cs_5_1，与引擎现有 shader 目标一致） ──
    UINT flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errors;
    HRESULT hr = CompileShaderFromFile(L"Shaders/InstanceCulling.cs.hlsl", "CSMain", "cs_5_1", flags, csBlob, &errors);
    if (FAILED(hr)) {
        if (errors)
            OutputDebugStringA((const char *)errors->GetBufferPointer());
        Logger::Logger::GetInstance()->Error("[CullingRenderer] Compile InstanceCulling.cs.hlsl failed: {:#x}",
                                             (unsigned)hr);
        return false;
    }

    // ── 根签名：b0 CullParams(CBV) + t0 实例SRV(表) + t1 bucketMapSRV(表) + u0 Append(表) + u1 IndirectArgs(表) ──
    {
        CD3DX12_ROOT_PARAMETER params[5];
        params[0].InitAsConstantBufferView(0); // b0: CullParams（视锥 6 平面 + 实例数）
        CD3DX12_DESCRIPTOR_RANGE ranges[4];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0: 实例 StructuredBuffer
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // t1: EntityBucketMap（方案 B 扁平桶归属）
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0); // u0: AppendBuffer
        ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1); // u1: IndirectArgs
        params[1].InitAsDescriptorTable(1, &ranges[0]);
        params[2].InitAsDescriptorTable(1, &ranges[1]);
        params[3].InitAsDescriptorTable(1, &ranges[2]);
        params[4].InitAsDescriptorTable(1, &ranges[3]);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
        // 规则 #12：必须用 Init()，禁止手动赋值字段
        rootSigDesc.Init(5, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, sigErrors;
        D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &sigErrors);
        if (!serialized)
            return false;
        hr = m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&m_computeRootSig));
        if (FAILED(hr))
            return false;
    }

    // ── Compute PSO ──
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_computeRootSig.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_computePSO));
        if (FAILED(hr))
            return false;
    }

    Logger::Logger::GetInstance()->Info("[CullingRenderer] L2b compute pipeline created");
    return true;
}

void CullingRenderer::DispatchCulling(Renderer::CommandList &cmd, const DirectX::XMVECTOR *planes,
                                      D3D12_GPU_DESCRIPTOR_HANDLE instanceSRV) {
    // [Diag] dispatch 执行/拦截计数（120 帧节流打印一次并清零）：区分各守卫拦截原因
    static uint32_t s_diagOk = 0, s_diagReady = 0, s_diagNative = 0, s_diagRes = 0, s_diagCb = 0;
    static uint32_t s_diagFrame = 0;
    auto diagFlush = [](uint32_t ok, uint32_t ready, uint32_t native, uint32_t res, uint32_t cb) {
        Logger::Logger::GetInstance()->Info(
            "[CullingRenderer][Diag] dispatch: ok={} skipReady={} skipNative={} skipRes={} skipCbAddr={}", ok, ready,
            native, res, cb);
    };

    if (!m_initialized || !m_dataStore || !m_resources)
        return;

    // 实例 SRV / bucketMap SRV 未就绪 → 跳过 dispatch，避免绑定空描述符表（GBV #646）
    if (!IsCullingReady() || !planes || m_srvIndex == UINT32_MAX || m_bucketMapSrvIndex == UINT32_MAX) {
        ++s_diagReady;
        if ((++s_diagFrame % 120) == 1) {
            diagFlush(s_diagOk, s_diagReady, s_diagNative, s_diagRes, s_diagCb);
            s_diagOk = s_diagReady = s_diagNative = s_diagRes = s_diagCb = 0;
        }
        return;
    }
    auto *native = cmd.Get();
    if (!native) {
        ++s_diagNative;
        return;
    }
    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

    // [CullDiag] 内容安全断言：UAV 槽位有效性（堆损坏/越界写 → 异常值在此暴露）
    assert(m_resources->GetAppendUavIndex() != UINT32_MAX && m_resources->GetIndirectArgsUavIndex() != UINT32_MAX &&
           "[CullDiag] dispatch UAV slots invalid (heap corruption?)");
    const uint32_t instanceCount = m_dataStore->GetInstanceCount();
    assert(instanceCount <= 1000000u && "[CullDiag] dispatch instanceCount overflow (heap corruption?)");

    // 入口统一解析 GPU 资源指针并判空（防御：句柄有效但资源未就绪/重建竞态时跳过，避免空指针崩溃）
    auto *indirectArgsRes = gpuMgr.GetResource(m_resources->GetIndirectArgsHandle());
    auto *bucketArgsRes = gpuMgr.GetResource(m_resources->GetBucketArgsUp());
    auto *appendRes = gpuMgr.GetResource(m_resources->GetAppendBufferHandle());
    if (!indirectArgsRes || !bucketArgsRes || !appendRes) {
        ++s_diagRes;
        return;
    }

    // b0: CullParams（视锥 6 平面 + 实例数）——UPLOAD 缓冲已在 CreateCullingPipeline 主线程预创建。
    // 提前到入口屏障之前准备：cbAddr==0 时直接 return，不留任何已录制的屏障（规则 10 对称屏障）
    struct CullParams {
        DirectX::XMFLOAT4 planes[6];
        uint32_t instanceCount;
        uint32_t pad[3];
    } params;
    for (int i = 0; i < 6; ++i)
        DirectX::XMStoreFloat4(&params.planes[i], planes[i]);
    params.instanceCount = instanceCount;

    // [Diag] 视锥平面诊断（节流 120 帧）：打印 6 平面 + 实例数（定位 CS 剔除帧间抖动）
    {
        static uint32_t s_planesDiagFrame = 0;
        if ((++s_planesDiagFrame % 120) == 1) {
            auto *logger = Logger::Logger::GetInstance();
            logger->Info(
                "[CullingRenderer][Diag] CullParams: instances={} "
                "p0=({:.3f},{:.3f},{:.3f},{:.3f}) p1=({:.3f},{:.3f},{:.3f},{:.3f}) "
                "p2=({:.3f},{:.3f},{:.3f},{:.3f}) p3=({:.3f},{:.3f},{:.3f},{:.3f}) "
                "p4=({:.3f},{:.3f},{:.3f},{:.3f}) p5=({:.3f},{:.3f},{:.3f},{:.3f})",
                params.instanceCount, params.planes[0].x, params.planes[0].y, params.planes[0].z, params.planes[0].w,
                params.planes[1].x, params.planes[1].y, params.planes[1].z, params.planes[1].w, params.planes[2].x,
                params.planes[2].y, params.planes[2].z, params.planes[2].w, params.planes[3].x, params.planes[3].y,
                params.planes[3].z, params.planes[3].w, params.planes[4].x, params.planes[4].y, params.planes[4].z,
                params.planes[4].w, params.planes[5].x, params.planes[5].y, params.planes[5].z, params.planes[5].w);
        }
    }
    // [Diag] CS 输入自检（节流 120 帧）：打印实例数——若正确但 CS 仍全剔除 → HLSL 布局错位
    {
        static uint32_t s_instDiagFrame = 0;
        if ((++s_instDiagFrame % 120) == 1) {
            Logger::Logger::GetInstance()->Info("[CullingRenderer][Diag] instance0: instances={}", instanceCount);
        }
    }
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = 0;
    const Resource::GpuResourceHandle cullParamsUp = m_resources->GetCullParamsUp();
    if (cullParamsUp.IsValid()) {
        void *mapped = nullptr;
        auto *cullRes = gpuMgr.GetResource(cullParamsUp);
        if (cullRes) { // 防御：句柄有效但资源未就绪（重建/释放竞态）时跳过，避免空指针
            cullRes->Map(0, nullptr, &mapped);
            memcpy(mapped, &params, sizeof(params));
            cullRes->Unmap(0, nullptr);
            cbAddr = cullRes->GetGPUVirtualAddress();
        }
    }
    if (cbAddr == 0) {
        ++s_diagCb;
        // CullParams CBV 无效——不能绑定地址 0 的 CBV，否则 GBV #961 / TDR
        const uint32_t raw = static_cast<uint32_t>(cullParamsUp);
        const uint32_t idx = raw & 0x3FFFFFu;      // 低 22 位 index
        const uint32_t gen = (raw >> 22) & 0x3FFu; // 高 10 位 generation
        Logger::Logger::GetInstance()->Error(
            "[CullingRenderer] CullParams CBV invalid (cbAddr=0), skip dispatch: valid={} handle=0x{:08X} "
            "(index={}, generation={}), instances={}",
            cullParamsUp.IsValid(), raw, idx, gen, instanceCount);
        return;
    }

    const uint32_t kMaxCullBuckets = CullingDataStore::kMaxCullBuckets;
    const auto &bucketOffsets = m_dataStore->GetBucketOffsets();

    // 每帧重写各桶 DRAW_INDEXED_ARGUMENTS 静态字段 + 清零 InstanceCount——UPLOAD 缓冲 CopyBufferRegion。
    // 规则 #10：对称屏障——本 pass 入口从 INDIRECT_ARGUMENT 转入，出口恢复 INDIRECT_ARGUMENT
    {
        // 入口屏障：AppendBuffer SRV（VS 消费后）→ UAV（本 pass CS 写）——GBV #942 对称修复
        D3D12_RESOURCE_BARRIER bAppendIn = CD3DX12_RESOURCE_BARRIER::Transition(
            appendRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        native->ResourceBarrier(1, &bAppendIn);

        D3D12_RESOURCE_BARRIER b1 = CD3DX12_RESOURCE_BARRIER::Transition(
            indirectArgsRes, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);
        native->ResourceBarrier(1, &b1);
        // 一次 COPY 前部全部桶段（IndexCount/InstanceCount/StartIndex/BaseVertex/StartInstance）
        if (bucketArgsRes) {
            native->CopyBufferRegion(indirectArgsRes, 0, bucketArgsRes, 0,
                                     sizeof(uint32_t) * 5 * RenderItemCommon::kMaxSubMeshRanges * kMaxCullBuckets);
        }
        // 桶偏移表（UPLOAD 前缀和，SetFlatInstances 填充）→ COPY 到 gIndirectArgs 尾部
        auto *bucketOffsetsRes = gpuMgr.GetResource(m_resources->GetBucketOffsetsUp());
        if (bucketOffsetsRes) {
            native->CopyBufferRegion(indirectArgsRes,
                                     sizeof(uint32_t) * 5 * RenderItemCommon::kMaxSubMeshRanges * kMaxCullBuckets,
                                     bucketOffsetsRes, 0, sizeof(uint32_t) * (kMaxCullBuckets + 1));
            // [Diag] 偏移表帧间一致性（2026-08-09 叠加根因验证）：打印 bucket 0/64/128 与 [L2cOffset] 对比
            if ((m_diagFrame % 60) == 1) {
                const uint32_t b0 = bucketOffsets.size() > 0 ? bucketOffsets[0] : 0u;
                const uint32_t b64 = bucketOffsets.size() > 64 ? bucketOffsets[64] : 0u;
                const uint32_t b128 = bucketOffsets.size() > 128 ? bucketOffsets[128] : 0u;
                Logger::Logger::GetInstance()->Info(
                    "[CullOffset][Diag] frame={} dispatch: b0={} b64={} b128={} offsetsSize={}", m_diagFrame, b0, b64,
                    b128, static_cast<int>(bucketOffsets.size()));
            }
        }
        // 非零桶计数 + total 清零（空桶跳过 2026-08-09）：CS InterlockedIncrement 递增计数与 total，
        // 每帧 dispatch 前必须清零（m_zeroUpload 4B 零 → kNonZeroCountAddr / kNonZeroTotalAddr）
        auto *zeroRes = gpuMgr.GetResource(m_resources->GetZeroUpload());
        if (zeroRes) {
            const uint32_t nonZeroCountAddr =
                5 * RenderItemCommon::kMaxSubMeshRanges * kMaxCullBuckets + (kMaxCullBuckets + 1);
            native->CopyBufferRegion(indirectArgsRes, sizeof(uint32_t) * nonZeroCountAddr, zeroRes, 0,
                                     sizeof(uint32_t));
            native->CopyBufferRegion(indirectArgsRes, sizeof(uint32_t) * (nonZeroCountAddr + kMaxCullBuckets + 1),
                                     zeroRes, 0, sizeof(uint32_t));
        }
        D3D12_RESOURCE_BARRIER b2 = CD3DX12_RESOURCE_BARRIER::Transition(
            indirectArgsRes, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        native->ResourceBarrier(1, &b2);
    }

    // 绑定 compute 根签名/PSO/资源
    ID3D12DescriptorHeap *heaps[] = {m_descHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_heapTag)};
    native->SetDescriptorHeaps(1, heaps);
    native->SetComputeRootSignature(m_computeRootSig.Get());
    native->SetPipelineState(m_computePSO.Get());

    native->SetComputeRootConstantBufferView(0, cbAddr);
    native->SetComputeRootDescriptorTable(1, instanceSRV.ptr ? instanceSRV : GetInstanceSRV());
    // 方案 B：t1 = EntityBucketMap（扁平桶归属，CS 按 bucketOffset..+bucketCount 遍历计数）
    native->SetComputeRootDescriptorTable(2, GetBucketMapSRV());
    native->SetComputeRootDescriptorTable(
        3, m_descHeaps->GetPartitionGpuHandle(PartitionType::Buffer, m_resources->GetAppendUavIndex(), m_heapTag));
    native->SetComputeRootDescriptorTable(
        4,
        m_descHeaps->GetPartitionGpuHandle(PartitionType::Buffer, m_resources->GetIndirectArgsUavIndex(), m_heapTag));

    // Dispatch（每实例一个 thread，64/组）
    const uint32_t groups = (instanceCount + 63) / 64;
    native->Dispatch(groups, 1, 1);

    // [Diag] dispatch 成功计数 + 节流打印
    m_hasDispatched = true; // 2026-08-09：标记已执行过至少一次（相机守卫首次兜底）
    ++s_diagOk;
    if ((++s_diagFrame % 120) == 1) {
        diagFlush(s_diagOk, s_diagReady, s_diagNative, s_diagRes, s_diagCb);
        s_diagOk = s_diagReady = s_diagNative = s_diagRes = s_diagCb = 0;
    }

    // 出口屏障：UNORDERED_ACCESS → INDIRECT_ARGUMENT（L2c 间接绘制消费）
    D3D12_RESOURCE_BARRIER b3 = CD3DX12_RESOURCE_BARRIER::Transition(
        gpuMgr.GetResource(m_resources->GetIndirectArgsHandle()), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    native->ResourceBarrier(1, &b3);

    // 出口屏障：AppendBuffer UAV → SRV（规则 10 对称——入口 SRV→UAV，出口 UAV→SRV；修复 GBV #942）
    D3D12_RESOURCE_BARRIER bAppendOut = CD3DX12_RESOURCE_BARRIER::Transition(
        appendRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    native->ResourceBarrier(1, &bAppendOut);

    // 验证 readback：把 gIndirectArgs 尾部的非零桶区（[0]=计数 + [1..kMaxCullBuckets]=列表）单次 COPY 到 READBACK 堆
    {
        auto *readbackRes = m_resources->GetVisibleReadback().IsValid()
                                ? gpuMgr.GetResource(m_resources->GetVisibleReadback())
                                : nullptr;
        if (readbackRes) {
            D3D12_RESOURCE_BARRIER bSrc = CD3DX12_RESOURCE_BARRIER::Transition(
                indirectArgsRes, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            native->ResourceBarrier(1, &bSrc);
            native->CopyBufferRegion(
                readbackRes, 0, indirectArgsRes,
                sizeof(uint32_t) * (5 * RenderItemCommon::kMaxSubMeshRanges * kMaxCullBuckets + (kMaxCullBuckets + 1)),
                sizeof(uint32_t) * (kMaxCullBuckets + 2));
            // 源恢复：COPY_SOURCE → INDIRECT_ARGUMENT（readback 保持 COPY_DEST，不参与 barrier）
            D3D12_RESOURCE_BARRIER bSrcBack = CD3DX12_RESOURCE_BARRIER::Transition(
                indirectArgsRes, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            native->ResourceBarrier(1, &bSrcBack);
        }
    }
    m_lastVisibleCount = 0; // 占位；精确值由门面/Editor 端 Readback 统计
}

bool CullingRenderer::IsCullingReady() const {
    return m_computePSO && m_resources && m_resources->IsCullingResourcesReady() &&
           m_resources->GetCullParamsUp().IsValid();
}

bool CullingRenderer::IsValid() const {
    return m_dataStore && m_dataStore->GetInstanceSegmentAddr() != 0 && m_srvIndex != UINT32_MAX;
}

D3D12_GPU_DESCRIPTOR_HANDLE CullingRenderer::GetInstanceSRV() const {
    if (!m_descHeaps || m_srvIndex == UINT32_MAX)
        return {};
    return m_descHeaps->GetPartitionGpuHandle(PartitionType::Buffer, m_srvIndex, m_heapTag);
}

D3D12_GPU_DESCRIPTOR_HANDLE CullingRenderer::GetBucketMapSRV() const {
    if (!m_descHeaps || m_bucketMapSrvIndex == UINT32_MAX)
        return {};
    return m_descHeaps->GetPartitionGpuHandle(PartitionType::Buffer, m_bucketMapSrvIndex, m_heapTag);
}

void CullingRenderer::CreateSRV(ID3D12Resource *ringRes, D3D12_GPU_VIRTUAL_ADDRESS segmentAddr) {
    if (!m_initialized || !m_device || !m_descHeaps || !ringRes || segmentAddr == 0 || m_srvIndex == UINT32_MAX ||
        !m_dataStore)
        return;

    // RingBuffer 单资源：段 GPU 地址 → 相对基址偏移 → SRV FirstElement
    const D3D12_GPU_VIRTUAL_ADDRESS baseAddr = ringRes->GetGPUVirtualAddress();
    const uint64_t byteOffset = segmentAddr >= baseAddr ? (segmentAddr - baseAddr) : 0;
    const uint32_t firstElement = static_cast<uint32_t>(byteOffset / sizeof(InstanceData));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = firstElement;
    srvDesc.Buffer.NumElements = m_dataStore->GetInstanceCount();
    srvDesc.Buffer.StructureByteStride = sizeof(InstanceData); // 单实例缓冲 160B（对齐陷阱：沿用 96B 会致 CS 读错位）

    auto cpuH = m_descHeaps->GetPartitionCpuHandle(PartitionType::Buffer, m_srvIndex, m_heapTag);
    m_device->CreateShaderResourceView(ringRes, &srvDesc, cpuH);

    // 每帧 Upload→CreateSRV 常态刷屏，节流 120 帧
    static uint32_t s_srvDiagFrame = 0;
    if ((++s_srvDiagFrame % 120) == 1) {
        Logger::Logger::GetInstance()->Info("[CullingRenderer] L2a SRV created: instances={} srvIndex={}",
                                            m_dataStore->GetInstanceCount(), m_srvIndex);
    }
}

void CullingRenderer::CreateCullDataSRV(ID3D12Resource *ringRes, D3D12_GPU_VIRTUAL_ADDRESS segmentAddr) {
    if (!m_initialized || !m_device || !m_descHeaps || !ringRes || segmentAddr == 0 || m_srvIndex == UINT32_MAX ||
        !m_dataStore)
        return;

    // 同一 RingBuffer 资源：段偏移 → FirstElement（CullData 48B 元素）
    const D3D12_GPU_VIRTUAL_ADDRESS baseAddr = ringRes->GetGPUVirtualAddress();
    const uint64_t byteOffset = segmentAddr >= baseAddr ? (segmentAddr - baseAddr) : 0;
    const uint32_t firstElement = static_cast<uint32_t>(byteOffset / sizeof(CullData));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = firstElement;
    srvDesc.Buffer.NumElements = static_cast<uint32_t>(m_dataStore->GetCullDataCount());
    srvDesc.Buffer.StructureByteStride = sizeof(CullData); // 48B（16B 对齐，Cache 友好——CS gCullData 绑定）

    auto cpuH = m_descHeaps->GetPartitionCpuHandle(PartitionType::Buffer, m_srvIndex, m_heapTag);
    m_device->CreateShaderResourceView(ringRes, &srvDesc, cpuH);

    // 每帧 Upload→CreateSRV 常态刷屏，节流 120 帧
    static uint32_t s_cullDiagFrame = 0;
    if ((++s_cullDiagFrame % 120) == 1) {
        Logger::Logger::GetInstance()->Info("[CullingRenderer] CullData SRV created: cullInstances={} srvIndex={}",
                                            static_cast<int>(m_dataStore->GetCullDataCount()), m_srvIndex);
    }
}

void CullingRenderer::CreateBucketMapSRV(ID3D12Resource *ringRes, D3D12_GPU_VIRTUAL_ADDRESS segmentAddr) {
    if (!m_initialized || !m_device || !m_descHeaps || !ringRes || segmentAddr == 0 ||
        m_bucketMapSrvIndex == UINT32_MAX || !m_dataStore)
        return;

    // 同一 RingBuffer 资源：段偏移 → FirstElement（uint32 元素）
    const D3D12_GPU_VIRTUAL_ADDRESS baseAddr = ringRes->GetGPUVirtualAddress();
    const uint64_t byteOffset = segmentAddr >= baseAddr ? (segmentAddr - baseAddr) : 0;
    const uint32_t firstElement = static_cast<uint32_t>(byteOffset / sizeof(uint32_t));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = firstElement;
    srvDesc.Buffer.NumElements = static_cast<uint32_t>(m_dataStore->GetBucketMapSize());
    srvDesc.Buffer.StructureByteStride = sizeof(uint32_t);

    auto cpuH = m_descHeaps->GetPartitionCpuHandle(PartitionType::Buffer, m_bucketMapSrvIndex, m_heapTag);
    m_device->CreateShaderResourceView(ringRes, &srvDesc, cpuH);
}

uint32_t CullingRenderer::ReadbackVisibleCount() {
    if (!m_resources || !m_resources->GetVisibleReadback().IsValid())
        return m_lastVisibleCount;
    auto *res = Resource::GpuResourceManager::GetInstance().GetResource(m_resources->GetVisibleReadback());
    if (!res)
        return m_lastVisibleCount;
    void *mapped = nullptr;
    if (FAILED(res->Map(0, nullptr, &mapped)) || !mapped)
        return m_lastVisibleCount;
    // 新布局（空桶跳过 2026-08-09）：readback = [0]=非零桶计数 + [1..kMaxCullBuckets]=列表 + [末尾]=total
    const uint32_t *p = static_cast<const uint32_t *>(mapped);
    const uint32_t nonZeroBuckets = p[0];
    const uint32_t total = p[CullingDataStore::kMaxCullBuckets + 1]; // 列表末尾 = 总可见实例数
    res->Unmap(0, nullptr);

    // [Diag] 非零桶统计（仅 Verify 调用，节流在调用方）
    Logger::Logger::GetInstance()->Info("[CullingRenderer][Diag] readback: total={} nonZeroBuckets={}", total,
                                        nonZeroBuckets);

    m_lastVisibleCount = total;
    return m_lastVisibleCount;
}

uint32_t CullingRenderer::ReadbackNonZeroBucketList(uint32_t *outNonZero, uint32_t capacity) {
    if (!m_resources || !m_resources->GetVisibleReadback().IsValid() || !outNonZero || capacity == 0)
        return 0;
    auto *res = Resource::GpuResourceManager::GetInstance().GetResource(m_resources->GetVisibleReadback());
    if (!res)
        return 0;
    void *mapped = nullptr;
    if (FAILED(res->Map(0, nullptr, &mapped)) || !mapped)
        return 0;
    // 布局（空桶跳过 2026-08-09）：[0]=非零桶计数 + [1..kMaxCullBuckets]=非零桶索引列表 + [末尾]=total
    const uint32_t *p = static_cast<const uint32_t *>(mapped);
    const uint32_t count = p[0];
    const uint32_t n = (count < capacity) ? count : capacity;
    for (uint32_t i = 0; i < n; ++i)
        outNonZero[i] = p[1 + i];
    res->Unmap(0, nullptr);
    return n;
}

} // namespace Culling
} // namespace DX12Engine
