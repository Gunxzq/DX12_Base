#pragma once

#include <cstdint>
#include <stdint.h>
#include <stdio.h>

namespace DX12Engine {

namespace Scheduler {
// FrameDriver.cpp 中 ExecuteRenderPhase 的调用序列决定。
// 早期 HZB = 上一帧 HZB（Opaque 后构建），本帧 PrePass 剔除直接消费；
// 新进入视野物体在其屏幕区域无深度记录 → HZB 采样 1.0（远）→ ObjNear < 1.0 不剔（天然安全）。
enum class RenderPhase : uint8_t {
    // 用户阶段
    Dispatch,          // GPU 剔除 dispatch
                       // 与 SSAO/SSR 等"显式串行阶段"惯例对齐，避免同阶段隐式依赖注册顺序）
    DerivedCollect,    // 派生渲染项收集+渲染显式阶段
    PrePass,           // 清屏
    Opaque,            // 不透明物体 + 地形（资源状态一致，合并在同一阶段；生成当前帧深度图 → 晚期 HZB 数据源）
    HZB_Build,         // 构建 HZB（消费本帧 Opaque 深度图 → mip 链；供本帧 SSR/接触阴影 + 下一帧遮挡剔除）
    DynamicAOcclusion, // 动态屏幕空间环境光遮蔽（Opaque 之后，Lighting 之前；直接采样深度，不依赖 HZB）
    Lighting,          // 延迟光照 Pass（读取 G-buffer + SSAO + 本帧 HZB，输出到交换链）
    SSR,               // 屏幕空间反射（Lighting 后、Transparent 前：G-buffer 法线/深度/粗糙度 + HZB 层级步进 →
                       // 半分辨率反射图，2026-08-12）
    Billboard,         // 公告牌渲染（独立阶段，避免与透明物体屏障冲突）
    Transparent,       // 透明物体
    PostProcess,       // 后处理 天空盒
    FSR3_Upscale,      // FSR3 超采样
    UI,                // 界面
    Count
};

} // namespace Scheduler

} // namespace DX12Engine
