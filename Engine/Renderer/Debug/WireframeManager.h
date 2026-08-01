#pragma once

#include "Renderer/Scene/Camera.h"
#include "Renderer/Scene/Struct/Frustum.h"
#include "Resource/Core/GpuHandlePool.h"
#include <DirectXMath.h>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine::Math {
struct BoundingAABB;
}

namespace DX12Engine::Renderer {

// ========================================================================
// WireframeManager — 引擎级调试线框渲染管理器（单例）
//
// 职责：
//   - 声明式收集：AddLine / AddAABB / AddFrustum 只写 CPU 侧线列表（无 GPU、顺序无关）
//   - 固定录制点：UpdateAndUpload 上传线列表到动态 VB，Draw 统一录制
//   - GS 展开：D3D12 LINELIST 无可靠线宽，GS 按屏幕空间宽度展开为四边形（2 三角形）
//   - PSO 具备深度测试（DepthEnable=TRUE，不写深度，调试叠加层）
//
// 设计依据：Docs/architecture/WireframeDebugDraw.md
//   - 管理器模式（非 ECS 组件）：调试线是瞬态命令，非场景"物体"，无属性卡需求
//   - 收集与录制分离：Add* 任意调用者安全；录制在固定时机发生，顺序确定
//   - 资源管理收敛在 Manager 内部，调用者零资源责任（规则 11 协作模式）
// ========================================================================

class WireframeManager {
public:
    static WireframeManager &GetInstance();

    WireframeManager(const WireframeManager &) = delete;
    WireframeManager &operator=(const WireframeManager &) = delete;
    WireframeManager() = default;
    ~WireframeManager() = default;

    void Initialize(ID3D12Device *device, DXGI_FORMAT depthFormat);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    // ── 声明式收集（CPU 侧，帧内累积，EndFrame 清空） ──

    /// 添加一条线段
    void AddLine(const DirectX::XMFLOAT3 &a, const DirectX::XMFLOAT3 &b, DirectX::XMFLOAT4 color);

    /// 添加世界空间 AABB 线框（12 条边）
    void AddAABB(const DX12Engine::Math::BoundingAABB &aabb, const DirectX::XMMATRIX &world, DirectX::XMFLOAT4 color);

    /// 添加视锥体线框（Blender 风格：从相机位置向远平面 4 角点画汇聚线，近/远裁剪面矩形保留）
    /// @param frustum 视锥体（GetCorners 提供近/远平面角点）
    /// @param cameraPosition 相机位置（汇聚线起点，视觉上定位相机）
    /// @param nearColor 近裁剪面矩形颜色
    /// @param farColor 远裁剪面矩形颜色
    /// @param connectColor 相机→远平面汇聚线颜色
    void AddFrustum(const Frustum &frustum, const DirectX::XMFLOAT3 &cameraPosition, DirectX::XMFLOAT4 nearColor,
                    DirectX::XMFLOAT4 farColor, DirectX::XMFLOAT4 connectColor);

    /// 清空本帧线列表（帧首调用）
    void Clear();

    /// 当前待绘制的线数（UpdateAndUpload 后 = 已上传线数；Editor 用于判断是否创建渲染命令）
    size_t GetLineCount() const { return m_uploadedLineCount > 0 ? m_uploadedLineCount : m_lines.size(); }

    // ── 固定录制点（帧循环固定位置调用） ──

    /// 上传 CPU 线列表到 GPU 动态 VB（UPLOAD 堆，主线程）
    /// @param fence 未来 fence（资源释放用，当前固定容量无需动态重建）
    /// @param camera 相机（用于写 viewProj 到 CB）
    void UpdateAndUpload(uint64_t fence, const Camera &camera);

    /// 录制线框绘制（需在当前渲染命令列表中调用，PSO 已含 GS 展开 + 深度）
    void Draw(ID3D12GraphicsCommandList *cmdList);

    // ── 参数控制 ──
    void SetVisible(bool visible) { m_visible = visible; }
    bool IsVisible() const { return m_visible; }
    void SetViewportSize(float width, float height) {
        m_screenWidth = width;
        m_screenHeight = height;
    }
    void SetLineWidth(float width) { m_lineWidth = width; }
    float GetLineWidth() const { return m_lineWidth; }

private:
    // ── 线框顶点 ──
    struct LineVertex {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT4 color;
    };

    // ── CB 布局（256 字节对齐） ──
    struct LineCBData {
        DirectX::XMMATRIX viewProj; // 64B — gViewProj
        float screenWidth;          // gParams.x
        float screenHeight;         // gParams.y
        float lineWidth;            // gParams.z
        float referenceDistance; // gParams.w — 距离自适应线宽的参考距离（此距离处线宽=基础宽）
    };

    void CreateRootSignature();
    void CreatePSO(ID3D12Device *device, DXGI_FORMAT depthFormat);
    void LoadShaders(ID3D12Device *device);

    // ── 收集状态 ──
    std::vector<LineVertex> m_lines;
    size_t m_uploadedLineCount = 0; // 最近一次 UpdateAndUpload 上传的线数（Draw 依据，与 m_lines 解耦）
    bool m_visible = true;

    // ── GPU 资源 ──
    Resource::GpuResourceHandle m_lineVB = Resource::GpuResourceHandle::Invalid(); // 动态线列表 VB（UPLOAD）
    Resource::GpuResourceHandle m_lineCB = Resource::GpuResourceHandle::Invalid(); // viewProj + 参数 CB（UPLOAD）
    D3D12_GPU_VIRTUAL_ADDRESS m_lineCBAddress = 0;
    uint32_t m_vbCapacityBytes = 0; // 已分配 VB 容量

    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_gsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

    // ── 绘制参数 ──
    float m_screenWidth = 1920.0f;
    float m_screenHeight = 1080.0f;
    float m_lineWidth = 3.0f; // 基础线宽（像素）——2.0f 太细，3.0f 配合距离自适应远处加粗
    float m_referenceDistance = 100.0f; // 距离自适应参考距离：此距离内保持基础宽，越远越粗（上限见 shader distScale）

    ID3D12Device *m_device = nullptr;
    bool m_initialized = false;
};

} // namespace DX12Engine::Renderer
