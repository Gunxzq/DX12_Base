#pragma once

#include <cstdint>

namespace DX12Engine::Resource {

/**
 * @brief 资产类型枚举
 *
 * 定义引擎支持的所有资产类型，独立于 AssetManager 以便各模块引用。
 * 新增类型时需同步更新 AssetManager::Load() 中的 switch 分支。
 */
enum class AssetType : uint8_t {
    Mesh,     ///< 网格模型 (.dxmesh, .obj)
    Texture,  ///< 纹理 (.dds, .png, .jpg)
    Material, ///< 材质 (.material)
    Terrain,  ///< 地形数据
    Scene,    ///< 场景描述 (.scene)

    // 原子资产扩展（2026-07-31，CharacterAsset.md 设计定案）
    Skeleton, ///< 骨骼 (.bone) — 骨骼树 + rest pose（HOD 解析导出）
    Animation, ///< 动画剪辑 (.anim) — 骨骼动画剪辑（播放/调帧/循环）

    // 复合资产扩展
    Character, ///< 角色复合资产 (.character) — 骨架 + 网格 + 材质槽 + 动画剪辑打包

    // 预留
    Prefab,         ///< 预制体 (.prefab) — 实体模板（预留）
    ParticleSystem, ///< 粒子系统 (.particle) — 预留
    Audio           ///< 音频 (.wav / .ogg) — 预留
};

} // namespace DX12Engine::Resource