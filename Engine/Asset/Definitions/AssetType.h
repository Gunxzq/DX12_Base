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
    Scene     ///< 场景描述 (.scene)
};

} // namespace DX12Engine::Resource