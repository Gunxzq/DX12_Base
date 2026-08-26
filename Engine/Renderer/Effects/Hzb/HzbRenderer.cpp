#include "HzbRenderer.h"

#include "Common/d3dUtil.h" // windows.h（OutputDebugStringA）+ CD3DX12 辅助
#include "Logger/Logger.h"
#include "Renderer/RHI/Command/CommandList/CommandList.h"
#include "Renderer/Utils/ShaderUtils.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include <d3dx12.h>

using namespace DX12Engine::Renderer;

namespace DX12Engine {
namespace Renderer {

bool HzbRenderer::Initialize() {
    if (m_initialized)
        return true;
    if (!m_device || !m_descriptorHeaps)
        return false;

    if (!CreatePipeline()) {
        Logger::Logger::GetInstance()->Error("[HzbRenderer] compute pipeline creation failed");
        return false;
    }

    m_initialized = true;
    Logger::Logger::GetInstance()->Info("[HzbRenderer] HZB build pipeline created");
    return true;
}

void HzbRenderer::Shutdown() {
    m_pso.Reset();
    m_rootSig.Reset();
    m_device = nullptr;
    m_descriptorHeaps = nullptr;
    m_initialized = false;
}

// ========================================================================
// CS 编译 + 根签名 + Compute PSO
// ========================================================================

bool HzbRenderer::CreatePipeline() {
    // ── 编译 HZB 降采样 compute shader（cs_5_1，自写，不用 FidelityFX SPD） ──
    UINT flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_UNBOUNDED_DESCRIPTOR_TABLES;
    Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errors;
    HRESULT hr = CompileShaderFromFile(L"Shaders/HzbBuild.cs.hlsl", "CSMain", "cs_5_1", flags, csBlob, &errors);
    if (FAILED(hr)) {
        if (errors)
            OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
        Logger::Logger::GetInstance()->Error("[HzbRenderer] Compile HzbBuild.cs.hlsl failed: {:#x}", (unsigned)hr);
        return false;
    }

    // ── 根签名：b0 root constants（srcW/srcH/srcMip/dstMip）
    //    + t0 深度 SRV + u0 目标 mip UAV + u1 源 mip UAV（规则 12：全部 Init） ──
    {
        CD3DX12_ROOT_PARAMETER params[4];
        params[0].InitAsConstants(4, 0); // b0: uint4（srcW, srcH, srcMip, dstMip）

        CD3DX12_DESCRIPTOR_RANGE ranges[3];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // t0: 深度图 SRV
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0); // u0: HZB 目标 mip UAV
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1); // u1: HZB 源 mip UAV

        params[1].InitAsDescriptorTable(1, &ranges[0]);
        params[2].InitAsDescriptorTable(1, &ranges[1]);
        params[3].InitAsDescriptorTable(1, &ranges[2]);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
        rootSigDesc.Init(4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        Microsoft::WRL::ComPtr<ID3DBlob> serialized, sigErrors;
        hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &sigErrors);
        if (FAILED(hr) || !serialized) {
            if (sigErrors)
                OutputDebugStringA(static_cast<const char *>(sigErrors->GetBufferPointer()));
            Logger::Logger::GetInstance()->Error("[HzbRenderer] SerializeRootSignature failed: {:#x}", (unsigned)hr);
            return false;
        }
        hr = m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&m_rootSig));
        if (FAILED(hr))
            return false;
    }

    // ── Compute PSO ──
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_rootSig.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
        if (FAILED(hr))
            return false;
    }

    return true;
}

void HzbRenderer::Execute(CommandList &cmd, ID3D12Resource *depthRes, D3D12_GPU_DESCRIPTOR_HANDLE depthSRV,
                          ID3D12Resource *hzbRes, const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> &mipUAVs,
                          uint32_t mipCount, uint32_t width, uint32_t height) {
    if (!m_initialized || !depthRes || !hzbRes || depthSRV.ptr == 0)
        return;
    // 守卫必须在入口屏障之前（规则 26：early-return 先于任何录制，保证对称）
    if (mipCount < 2 || mipUAVs.size() < static_cast<size_t>(mipCount) || width == 0 || height == 0)
        return;

    ID3D12GraphicsCommandList *native = cmd.Get();
    if (!native)
        return;

    // 描述符堆：HZB UAV 与深度 SRV 同堆域（规则 17）
    ID3D12DescriptorHeap *heaps[] = {m_descriptorHeaps->GetHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, m_heapTag)};
    if (!heaps[0])
        return;
    native->SetDescriptorHeaps(1, heaps);

    // ── 入口屏障（对称：出口恢复） ──
    D3D12_RESOURCE_BARRIER depthIn = CD3DX12_RESOURCE_BARRIER::Transition(
        depthRes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    native->ResourceBarrier(1, &depthIn);
    D3D12_RESOURCE_BARRIER hzbIn = CD3DX12_RESOURCE_BARRIER::Transition(hzbRes, D3D12_RESOURCE_STATE_COMMON,
                                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    native->ResourceBarrier(1, &hzbIn);

    native->SetComputeRootSignature(m_rootSig.Get());
    native->SetPipelineState(m_pso.Get());

    // ── HZB 构建链：mip0 = 深度图 1:1 拷贝；mip i>=1 = 2×2 max 降采样（mip i-1）──
    // 标准 HZB（UE/Frostbite）：mip0 是有效层级（= 深度图拷贝），遮挡测试从 mip0 起选层采样
    for (uint32_t i = 0; i < mipCount; ++i) {
        // 源尺寸：i==0 → 深度图全分辨率；i>=1 → HZB mip(i-1) 尺寸（D3D floor 语义）
        const uint32_t srcW = (i == 0) ? width : MipSize(width, i - 1);
        const uint32_t srcH = (i == 0) ? height : MipSize(height, i - 1);
        // 目标尺寸：i==0 → 拷贝模式（与源同尺寸，1:1）；i>=1 → 降采样（floor 减半）
        const uint32_t dstW = (i == 0) ? srcW : std::max(1u, srcW >> 1);
        const uint32_t dstH = (i == 0) ? srcH : std::max(1u, srcH >> 1);

        // root constants（b0: uint4 = {srcW, srcH, srcMip, dstMip}）
        uint32_t cbData[4] = {srcW, srcH, (i == 0) ? 0u : i - 1, i};
        native->SetComputeRoot32BitConstants(0, 4, cbData, 0);

        // t0: 深度图 SRV（i==0 拷贝模式读取；i>=1 不访问但绑定有效句柄）
        native->SetComputeRootDescriptorTable(1, depthSRV);
        // u0: 目标 mip i UAV
        native->SetComputeRootDescriptorTable(2, mipUAVs[i]);
        // u1: 源 mip UAV（i==0 拷贝模式不访问，绑 mip0 UAV 兜底）
        native->SetComputeRootDescriptorTable(3, (i >= 1) ? mipUAVs[i - 1] : mipUAVs[0]);

        const uint32_t groupsX = (dstW + 7) / 8;
        const uint32_t groupsY = (dstH + 7) / 8;
        native->Dispatch(groupsX, groupsY, 1);

        // 级间 UAV barrier（下一级读本级的写——同资源读写可见性）
        if (i < mipCount - 1) {
            D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(hzbRes);
            native->ResourceBarrier(1, &uavBarrier);
        }
    }

    // ── 出口屏障（对称恢复，规则 10） ──
    D3D12_RESOURCE_BARRIER hzbOut = CD3DX12_RESOURCE_BARRIER::Transition(hzbRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                         D3D12_RESOURCE_STATE_COMMON);
    native->ResourceBarrier(1, &hzbOut);
    D3D12_RESOURCE_BARRIER depthOut = CD3DX12_RESOURCE_BARRIER::Transition(
        depthRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    native->ResourceBarrier(1, &depthOut);
}

} // namespace Renderer
} // namespace DX12Engine
