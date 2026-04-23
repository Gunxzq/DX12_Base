#pragma once

#include <cstdint>
#include <d3d12.h>

// 前置声明：避免循环依赖
// GameTimer 用于获取时间步长
class GameTimer;
// D3D12DeviceContext 是我们之前讨论的“上下文”，负责提供底层资源
class D3D12DeviceContext;

// IRenderer 接口
// 这是一个纯粹的虚类（接口），定义了所有渲染模块（如 SceneRenderer, UIRenderer）必须遵守的契约
class IRenderer {
public:
    // 虚析构函数：确保通过基类指针删除派生类对象时能正确释放资源
    virtual ~IRenderer() = default;

    // ----------------------------------------------------------------------
    // 1. 生命周期与依赖注入
    // ----------------------------------------------------------------------

    // 注入上下文
    // 在引擎初始化阶段，由 D3D12DeviceContext (或 RenderCore) 调用此函数
    // 将设备、命令队列、交换链等底层资源的访问权交给渲染器
    virtual void SetSystem(D3D12DeviceContext *system) = 0;

    // 初始化资源
    // 在这里创建 PSO (管线状态对象), RootSignature (根签名), 以及静态资源 (如静态几何体缓冲区)
    // 注意：这里不应该包含具体的绘制逻辑，只做一次性初始化
    virtual void Initialize() = 0;

    // 资源重建
    // 当窗口大小改变 (OnResize) 时调用
    // 用于更新视口 (Viewport), 投影矩阵, 以及依赖屏幕尺寸的动态缓冲区
    virtual void OnResize(uint32_t width, uint32_t height) = 0;

    // ----------------------------------------------------------------------
    // 2. 帧循环逻辑
    // ----------------------------------------------------------------------

    // 逻辑更新
    // 每一帧调用一次，用于更新非图形逻辑
    // 例如：更新摄像机位置、UI 动画状态、粒子系统的 CPU 模拟等
    virtual void Update(const GameTimer &timer) = 0;

    // 录制命令
    // 这是最核心的函数。
    // cmdList: 当前帧的命令列表，渲染器负责往里面写入指令 (Draw, SetPipeline...)
    // backBufferIndex: 当前正在绘制的后缓冲区索引 (0, 1, 2...)，用于获取对应的 RTV/Descriptor
    //
    // 注意：这里只负责 Record (录制)，不负责 Execute (执行)。
    // 执行的工作由持有 SwapChain 的 D3D12DeviceContext 统一管理。
    virtual void RecordCommands(ID3D12GraphicsCommandList *cmdList, uint32_t backBufferIndex) = 0;
};