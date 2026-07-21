#pragma once

#include "Resource/Core/GpuHandlePool.h"
#include <DirectXMath.h>
#include <d3d12.h>

namespace DX12Engine::Renderer {

// ========================================================================
// GridManager — 网格管理器（单例，Shader-Based 无限网格）
//
// 职责：
//   - 管理网格参数（间距、线宽、衰减距离、可见性）
//   - 管理 GPU 资源（网格 Quad VB/IB、常量缓冲 CB）的生命周期
//   - 每帧 SetCameraPos 后计算网格原点（snap 到 spacing 整数倍）
//   - SnapToGrid() 吸附计算（编辑器 + Game 共用）
//
// 网格模式：
//   - 大 Quad（2 个三角形）+ 像素着色器计算网格线
//   - 多级 LOD：次网格（minorSpacing）+ 主网格（majorSpacing）
//   - 三轴高亮线：X(红)、Y(绿)、Z(蓝)
//   - 距离衰减，反走样线
// ========================================================================

class GridManager {
public:
    static GridManager &GetInstance();

    GridManager(const GridManager &) = delete;
    GridManager &operator=(const GridManager &) = delete;

    GridManager() = default;
    ~GridManager() = default;

    void Initialize(ID3D12Device *device);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    // ── 参数控制 ──
    void SetMinorSpacing(float spacing);
    float GetMinorSpacing() const { return m_minorSpacing; }
    void SetMajorSpacing(float spacing);
    float GetMajorSpacing() const { return m_majorSpacing; }
    void SetVisible(bool visible) { m_visible = visible; }
    bool IsVisible() const { return m_visible; }

    /// 设置相机位置（每帧更新），网格中心会 snap 到 spacing 整数倍
    void SetCameraPosition(float camX, float camZ);

    // ── Immediate 回调（主线程） ──
    void UpdateAndUpload(uint64_t fence, const DirectX::XMMATRIX &viewProj, const DirectX::XMFLOAT3 &cameraPos);

    // ── GPU 资源访问（供 GridRenderer 使用） ──
    D3D12_GPU_VIRTUAL_ADDRESS GetGridCBAddress() const { return m_gridCBAddress; }
    Resource::GpuResourceHandle GetGridCBHandle() const { return m_gridCB; }
    Resource::GpuResourceHandle GetQuadVBHandle() const { return m_quadVB; }
    Resource::GpuResourceHandle GetQuadIBHandle() const { return m_quadIB; }
    uint32_t GetQuadVertexCount() const { return 4; }
    uint32_t GetQuadIndexCount() const { return 6; }
    float GetGridHalfSize() const { return m_gridHalfSize; }
    float GetLineWidth() const { return m_lineWidth; }
    float GetFadeDist() const { return m_fadeDist; }
    float GetCameraSnapX() const { return m_cameraSnapX; }
    float GetCameraSnapZ() const { return m_cameraSnapZ; }

    // ── 吸附计算 ──
    DirectX::XMFLOAT3 SnapToGrid(const DirectX::XMFLOAT3 &pos) const;
    DirectX::XMFLOAT2 SnapToGrid2D(const DirectX::XMFLOAT2 &pos) const;

private:
    Resource::GpuResourceHandle m_quadVB = Resource::GpuResourceHandle::Invalid(); // 4 顶点（单位 Quad）
    Resource::GpuResourceHandle m_quadIB = Resource::GpuResourceHandle::Invalid(); // 6 索引
    Resource::GpuResourceHandle m_gridCB = Resource::GpuResourceHandle::Invalid();

    float m_minorSpacing = 50.0f;   // 次网格间距
    float m_majorSpacing = 500.0f;  // 主网格间距
    float m_lineWidth = 2.0f;       // 网格线宽
    float m_fadeDist = 2000.0f;     // 衰减距离
    float m_gridHalfSize = 1000.0f; // 网格半边
    bool m_visible = true;
    bool m_gridDirty = true;

    float m_cameraSnapX = 0.0f;
    float m_cameraSnapZ = 0.0f;
    D3D12_GPU_VIRTUAL_ADDRESS m_gridCBAddress = 0;
    ID3D12Device *m_device = nullptr;
    bool m_initialized = false;
};

} // namespace DX12Engine::Renderer