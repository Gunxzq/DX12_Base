#include "GridManager.h"
#include "Logger/Logger.h" // 2026-08-19：Map 失败防御日志（统一 Map/Unmap 加固）
#include "Resource/GpuResourceManager.h"

namespace DX12Engine::Renderer {

GridManager &GridManager::GetInstance() {
    static GridManager instance;
    return instance;
}

void GridManager::Initialize(ID3D12Device *device) {
    if (m_initialized)
        return;
    m_device = device;

    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

    // ── 常量缓冲 ──
    size_t cbSize = 256; // 对齐到 256 字节
    m_gridCB =
        gpuMgr.CreateBuffer(device, cbSize, L"Grid_CB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_gridCB.IsValid())
        return;

    ID3D12Resource *cbRes = gpuMgr.GetResource(m_gridCB);
    if (cbRes) {
        m_gridCBAddress = cbRes->GetGPUVirtualAddress();
    }

    // ── 单位 Quad VB（[-1,1] 范围，VS 中做缩放和偏移） ──
    struct Vert { float x, y, z; };
    Vert verts[4] = {
        {-1, 0, -1}, // 0: 左下
        { 1, 0, -1}, // 1: 右下
        { 1, 0,  1}, // 2: 右上
        {-1, 0,  1}, // 3: 左上
    };

    m_quadVB = gpuMgr.CreateBuffer(device, sizeof(verts), L"Grid_Quad_VB", D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_quadVB.IsValid())
        return;

    ID3D12Resource *vbRes = gpuMgr.GetResource(m_quadVB);
    if (vbRes) {
        void *mapped = nullptr;
        HRESULT mapHr = vbRes->Map(0, nullptr, &mapped);
        // 2026-08-19 防御加固：Map 失败时绝不 Unmap（#310）/memcpy（写 0x0），跳过并记录
        if (FAILED(mapHr) || !mapped) {
            Logger::Logger::GetInstance()->Warn("[GridManager] QuadVB Map failed: hr=0x{:08X}", (unsigned)mapHr);
        } else {
            memcpy(mapped, verts, sizeof(verts));
            vbRes->Unmap(0, nullptr);
        }
    }

    // ── Quad IB（2 个三角形） ──
    uint16_t indices[6] = {0, 1, 2, 0, 2, 3};

    m_quadIB = gpuMgr.CreateBuffer(device, sizeof(indices), L"Grid_Quad_IB", D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_quadIB.IsValid())
        return;

    ID3D12Resource *ibRes = gpuMgr.GetResource(m_quadIB);
    if (ibRes) {
        void *mapped = nullptr;
        HRESULT mapHr = ibRes->Map(0, nullptr, &mapped);
        // 2026-08-19 防御加固：Map 失败时绝不 Unmap（#310）/memcpy（写 0x0），跳过并记录
        if (FAILED(mapHr) || !mapped) {
            Logger::Logger::GetInstance()->Warn("[GridManager] QuadIB Map failed: hr=0x{:08X}", (unsigned)mapHr);
        } else {
            memcpy(mapped, indices, sizeof(indices));
            ibRes->Unmap(0, nullptr);
        }
    }

    m_initialized = true;
    m_gridDirty = true;
}

void GridManager::Shutdown() {
    if (!m_initialized)
        return;

    auto &gpuMgr = Resource::GpuResourceManager::GetInstance();

    if (m_quadIB.IsValid()) {
        gpuMgr.Release(m_quadIB, 0);
        m_quadIB = Resource::GpuResourceHandle::Invalid();
    }
    if (m_quadVB.IsValid()) {
        gpuMgr.Release(m_quadVB, 0);
        m_quadVB = Resource::GpuResourceHandle::Invalid();
    }
    if (m_gridCB.IsValid()) {
        gpuMgr.Release(m_gridCB, 0);
        m_gridCB = Resource::GpuResourceHandle::Invalid();
    }

    m_gridCBAddress = 0;
    m_cameraSnapX = 0;
    m_cameraSnapZ = 0;
    m_device = nullptr;
    m_initialized = false;
    m_gridDirty = true;
}

void GridManager::SetMinorSpacing(float spacing) {
    if (m_minorSpacing != spacing) {
        m_minorSpacing = spacing;
        m_gridDirty = true;
    }
}

void GridManager::SetMajorSpacing(float spacing) {
    if (m_majorSpacing != spacing) {
        m_majorSpacing = spacing;
        m_gridDirty = true;
    }
}

void GridManager::SetCameraPosition(float camX, float camZ) {
    if (m_minorSpacing <= 0.0f)
        return;
    float snapX = std::roundf(camX / m_minorSpacing) * m_minorSpacing;
    float snapZ = std::roundf(camZ / m_minorSpacing) * m_minorSpacing;
    if (m_cameraSnapX != snapX || m_cameraSnapZ != snapZ) {
        m_cameraSnapX = snapX;
        m_cameraSnapZ = snapZ;
        m_gridDirty = true;
    }
}

void GridManager::UpdateAndUpload(uint64_t fence, const DirectX::XMMATRIX &viewProj, const DirectX::XMFLOAT3 &cameraPos) {
    if (!m_initialized || !m_gridCB.IsValid())
        return;

    // 写入完整 CB（主线程，每帧更新）
    ID3D12Resource *cbRes = Resource::GpuResourceManager::GetInstance().GetResource(m_gridCB);
    if (!cbRes)
        return;

    struct GridCBData {
        DirectX::XMMATRIX viewProj;  // 64B — gViewProj
        float camX, camY, camZ, camW; // 16B — gCameraPos
        float minorSpacing, majorSpacing, lineWidth, fadeDist; // 16B — gGridParams
        float snapX, snapY, snapZ, gridHalfSize; // 16B — gSnapOffset
    };
    GridCBData cbData;
    cbData.viewProj = viewProj;
    cbData.camX = cameraPos.x;
    cbData.camY = cameraPos.y;
    cbData.camZ = cameraPos.z;
    cbData.camW = 0.0f;
    cbData.minorSpacing = m_minorSpacing;
    cbData.majorSpacing = m_majorSpacing;
    cbData.lineWidth = m_lineWidth;
    cbData.fadeDist = m_fadeDist;
    cbData.snapX = m_cameraSnapX;
    cbData.snapY = 0.0f;
    cbData.snapZ = m_cameraSnapZ;
    cbData.gridHalfSize = m_gridHalfSize;

    void *mapped = nullptr;
    HRESULT mapHr = cbRes->Map(0, nullptr, &mapped);
    // 2026-08-19 防御加固：Map 失败时绝不 Unmap（#310）/memcpy（写 0x0），跳过并记录
    if (FAILED(mapHr) || !mapped) {
        Logger::Logger::GetInstance()->Warn("[GridManager] GridCB Map failed: hr=0x{:08X}", (unsigned)mapHr);
        return;
    }
    memcpy(mapped, &cbData, sizeof(cbData));
    cbRes->Unmap(0, nullptr);

    m_gridDirty = false;
}

DirectX::XMFLOAT3 GridManager::SnapToGrid(const DirectX::XMFLOAT3 &pos) const {
    if (m_minorSpacing <= 0.0f)
        return pos;
    return DirectX::XMFLOAT3{std::roundf(pos.x / m_minorSpacing) * m_minorSpacing,
                             std::roundf(pos.y / m_minorSpacing) * m_minorSpacing,
                             std::roundf(pos.z / m_minorSpacing) * m_minorSpacing};
}

DirectX::XMFLOAT2 GridManager::SnapToGrid2D(const DirectX::XMFLOAT2 &pos) const {
    if (m_minorSpacing <= 0.0f)
        return pos;
    return DirectX::XMFLOAT2{std::roundf(pos.x / m_minorSpacing) * m_minorSpacing,
                             std::roundf(pos.y / m_minorSpacing) * m_minorSpacing};
}

} // namespace DX12Engine::Renderer