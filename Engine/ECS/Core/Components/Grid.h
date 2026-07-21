#pragma once

namespace DX12Engine::ECS {

// ========================================================================
// GridComponent — 网格组件
//
// 挂载到需要网格吸附/网格放置逻辑的实体上。
// 策略/建造类游戏通过此组件控制实体与网格的交互。
// 渲染数据由 GridManager 单例统一管理，不在此组件中持有 GPU 资源。
// ========================================================================

struct GridComponent {
    float spacing = 50.0f;
    bool visible = true;
};

} // namespace DX12Engine::ECS