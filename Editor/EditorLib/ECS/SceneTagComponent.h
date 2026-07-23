#pragma once

#include <cstdint>

// ========================================================================
// SceneTagComponent — 编辑器端专用组件，标记实体所属场景
//
// 用途：
//   当多个场景 Tab 的实体共存于同一 ECS Registry 时，
//   通过 sceneId 区分实体属于哪个场景。
//   构建器根据当前活跃 sceneId 过滤需要处理的实体。
//
// 生命周期：
//   - 实体创建时由 OnSceneConstructReady 添加
//   - 场景切换时不销毁，仅更新 EditorSceneManager 的活跃 sceneId
//   - 关闭 Tab 时销毁对应 sceneId 的所有实体
// ========================================================================

struct SceneTagComponent {
    uint64_t sceneId = 0; // 所属场景 ID（0 表示未分配/默认场景）
};