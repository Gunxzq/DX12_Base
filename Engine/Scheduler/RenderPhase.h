#pragma once

#include <cstdint>
#include <stdint.h>
#include <stdio.h>

namespace DX12Engine {

namespace Scheduler {
enum class RenderPhase : uint8_t {
    // 用户阶段
    PrePass,      // 阴影、深度、遮挡剔除
    Opaque,       // 不透明物体 + 地形（资源状态一致，合并在同一阶段）
    Transparent,  // 透明物体
    Billboard,    // 公告牌渲染（独立阶段，避免与透明物体屏障冲突）
    PostProcess,  // 后处理
    FSR3_Upscale, // FSR3 超采样
    UI,           // 界面
    Count
};

}

} // namespace DX12Engine