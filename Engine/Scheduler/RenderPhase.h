#pragma once

#include <cstdint>
#include <stdint.h>
#include <stdio.h>

namespace DX12Engine::Scheduler {

enum class RenderPhase : uint8_t {
    // 帧开始屏障（引擎内部使用）
    BeginBarrier,

    // 用户阶段
    PrePass,     // 阴影、深度、遮挡剔除
    Opaque,      // 不透明物体
    Transparent, // 透明物体
    PostProcess, // 后处理
    UI,          // 界面

    // 帧结束屏障（引擎内部使用）
    EndBarrier,

    Count
};

} // namespace DX12Engine::Scheduler