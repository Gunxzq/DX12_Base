#include "PreviewManager.h"
#include "Common/Common.h"
#include "DebugUI/DebugUIManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Pool/RenderTargetPool.h"

using namespace DX12Engine;
using namespace DX12Engine::Renderer;



void PreviewManager::Initialize(ID3D12Device *device, D3D12DeviceContext *context, Resource::RenderTargetPool *rtPool) {
    if (m_initialized)
        return;
    m_device = device;
    m_context = context;
    m_rtPool = rtPool;
    if (!m_device || !m_context || !m_rtPool)
        return;
    m_initialized = true;
}

void PreviewManager::Shutdown() {
    if (!m_initialized)
        return;

    for (auto &slot : m_slots) {
        if (slot.ctx.renderTarget.IsValid())
            m_rtPool->Free(slot.ctx.renderTarget, 0);
        if (slot.ctx.imguiSrvCpu.ptr != 0)
            DebugUI::DebugUIManager::Get().FreeSrvDescriptor(slot.ctx.imguiSrvCpu, slot.ctx.outputSRV);
    }

    m_slots = {};
    m_renderCallback = nullptr;
    m_device = nullptr;
    m_context = nullptr;
    m_rtPool = nullptr;
    m_initialized = false;
}

void PreviewManager::SetRenderCallback(PreviewRenderCallback callback) {
    m_renderCallback = std::move(callback);
}

PreviewId PreviewManager::AcquirePreview(PreviewId oldPreviewId, PreviewType type, uint32_t width, uint32_t height) {
    if (!m_initialized)
        return 0;

    // 优先复用 oldPreviewId 所在的槽位
    uint32_t slotIndex = UINT32_MAX;
    if (oldPreviewId != 0) {
        for (uint32_t i = 0; i < POOL_SIZE; ++i) {
            if (m_slots[i].id == oldPreviewId) {
                slotIndex = i;
                break;
            }
        }
    }

    // 没找到则使用轮转策略
    if (slotIndex == UINT32_MAX) {
        slotIndex = m_nextSlot;
        m_nextSlot = (m_nextSlot + 1) % POOL_SIZE;
    }

    PreviewSlot &slot = m_slots[slotIndex];
    PreviewContext newCtx;
    newCtx.type = type;
    newCtx.width = width;
    newCtx.height = height;

    // 首次使用该槽位：分配 RT 和 SRV（后续复用不复分配）
    bool isFirstUse = (slot.id == 0);
    if (isFirstUse) {
        if (type == PreviewType::Detail) {
            Resource::RenderTargetDesc rtDesc = {};
            rtDesc.width = width;
            rtDesc.height = height;
            rtDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rtDesc.mipLevels = 1;
            rtDesc.arraySize = 1;
            rtDesc.sampleDesc = {1, 0};
            rtDesc.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            rtDesc.clearValue = {DXGI_FORMAT_R8G8B8A8_UNORM, {0.12f, 0.12f, 0.14f, 1.0f}};
            rtDesc.name = L"PreviewRT_Detail";

            newCtx.renderTarget = m_rtPool->Allocate(rtDesc);
            if (!newCtx.renderTarget.IsValid())
                return 0;
            newCtx.rtvHandle = m_rtPool->GetRtvHandle(newCtx.renderTarget);
        }

        // 在 ImGui 描述符堆中分配 SRV
        DebugUI::DebugUIManager::Get().AllocateSrvDescriptor(&newCtx.imguiSrvCpu, &newCtx.outputSRV);
        ID3D12Resource *rtRes = nullptr;
        if (type == PreviewType::Detail)
            rtRes = m_rtPool->GetResource(newCtx.renderTarget);
        if (rtRes && newCtx.imguiSrvCpu.ptr != 0) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            m_device->CreateShaderResourceView(rtRes, &srvDesc, newCtx.imguiSrvCpu);
        }

        slot.id = m_nextId++;
    } else {
        // 复用已有 RT 和 SRV
        newCtx.renderTarget = slot.ctx.renderTarget;
        newCtx.rtvHandle = slot.ctx.rtvHandle;
        newCtx.imguiSrvCpu = slot.ctx.imguiSrvCpu;
        newCtx.outputSRV = slot.ctx.outputSRV;
    }

    // 分配或复用 ID
    if (slot.id == 0)
        slot.id = m_nextId++;
    else
        newCtx.loadSequence = slot.ctx.loadSequence; // 保持序列号连续

    newCtx.valid = true;
    newCtx.needsRender = true;
    slot.ctx = newCtx;
    slot.inUse = true;

    return slot.id;
}

D3D12_GPU_DESCRIPTOR_HANDLE PreviewManager::GetOutputSRV(PreviewId id) const {
    for (const auto &slot : m_slots) {
        if (slot.id == id && slot.inUse)
            return slot.ctx.outputSRV;
    }
    return {};
}

PreviewContext *PreviewManager::GetContext(PreviewId id) {
    for (auto &slot : m_slots) {
        if (slot.id == id && slot.inUse)
            return &slot.ctx;
    }
    return nullptr;
}

void PreviewManager::RenderPreviews() {
    if (!m_initialized || !m_renderCallback)
        return;

    for (auto &slot : m_slots) {
        if (!slot.inUse || !slot.ctx.valid || !slot.ctx.needsRender)
            continue;

        m_renderCallback(slot.id, slot.ctx);
    }
}


