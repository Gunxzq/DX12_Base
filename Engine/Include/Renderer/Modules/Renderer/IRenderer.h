#pragma once

#include "Renderer/Core/Command/CommandList/CommandList.h"
#include "System/ECS/Registry.h"
#include <cstdint>

// 前置声明
class D3D12DeviceContext;

namespace DX12Engine::Renderer {

// IRenderer 接口
// 定义了所有渲染模块（如 SceneRenderer, UIRenderer）必须遵守的契约
class IRenderer {
public:
    // 虚析构函数：确保通过基类指针删除派生类对象时能正确释放资源
    virtual ~IRenderer() = default;

    // ----------------------------------------------------------------------
    // 1. 生命周期与依赖注入
    // ----------------------------------------------------------------------

    /**
     * @brief 注入 D3D12 设备上下文
     * @param context D3D12 核心上下文指针，用于访问 CommandManager、交换链等资源
     * @note 在 Initialize 之前调用
     */
    virtual void SetDeviceContext(D3D12DeviceContext *context) = 0;

    /**
     * @brief 初始化渲染资源
     * @note 在此处创建 PSO、RootSignature 以及静态几何体缓冲区等一次性资源
     */
    virtual void Initialize() = 0;

    /**
     * @brief 处理窗口大小改变
     * @param width 新宽度
     * @param height 新高度
     * @note 用于更新视口、投影矩阵及依赖屏幕尺寸的动态缓冲区
     */
    virtual void OnResize(uint32_t width, uint32_t height) = 0;

    // ----------------------------------------------------------------------
    // 2. 帧循环逻辑
    // ----------------------------------------------------------------------

    /**
     * @brief 逻辑更新
     * @param deltaTime 帧间隔时间（秒）
     * @note 用于更新摄像机、动画状态等非图形逻辑
     */
    virtual void Update(float deltaTime) = 0;

    virtual void RecordCommands(uint32_t backBufferIndex) = 0;

    /**
     * @brief 录制渲染命令
     * @param registry ECS 注册表，用于查询实体组件数据
     * @param cmdList 命令列表封装对象，用于录制 GPU 命令
     * @param backBufferIndex 当前后缓冲区索引，用于多缓冲资源选择
     * @note 实现类应在此处遍历 ECS 实体并录制绘制命令
     */
    virtual void RecordDrawCalls(DX12Engine::ECS::Registry &registry, DX12Engine::Renderer::CommandList &cmdList,
                                 uint32_t backBufferIndex) = 0;
};

} // namespace DX12Engine::Renderer