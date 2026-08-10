#pragma once

#include "Asset/IO/Loader/SceneDescription.h"
#include "ECS/Core/Entity.h"
#include "Renderer/Material/MaterialHandle.h"
#include "Resource/AssetManager/AssetManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/GpuResourceManager.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Engine {

namespace Boot {
class GameContext;
}

namespace Resource {
struct GeometryHandle;
} // namespace Resource

namespace ECS {
class Registry;
}

namespace Scene {

// ========================================================================
// 生成器类型标识（用于 GeneratorTaskCompleteEvent 的 payload 高位编码）
// 各生成器在各自头文件中定义自己的类型 ID，引擎 CORE 不感知具体类型
// ========================================================================

/// SceneConstructor 的生成器类型 ID（payload 高位 32 bits）
constexpr uint32_t GENERATOR_TYPE_SCENE_CONSTRUCTOR = 0;

// ========================================================================
// SceneConstructor — 场景构造系统
//
// 接收 SceneDescription（已解析的场景 JSON），
// 异步加载所有依赖资产，完成后发送 GeneratorTaskCompleteEvent，
// 由 SceneConstructSystem 响应并构造 ECS 实体。
//
// 职责边界：
//   AssetManager: 文件 IO → GPU 资源句柄
//   SceneConstructor: 资源句柄 → 场景构造数据 + 材质 buffer 上传
//   SceneConstructSystem: 场景构造数据 → ECS 实体
// ========================================================================

// 场景构造数据（存入 SharedDataStore，由 SceneConstructSystem 消费）
struct SceneConstructData {
    std::string sceneName;
    std::string sceneFilePath; // 原始场景文件路径（用于 Tab 匹配）
    std::vector<Resource::EntityDesc> entities;
    std::unordered_map<std::string, Resource::GeometryHandle> geoMap;
    std::unordered_map<std::string, Resource::MaterialHandle> matMap;

    // 场景环境数据（由 OnSceneConstructReady 设置到 SceneManager）
    std::optional<Resource::SkyboxDesc> skybox;
    std::optional<Resource::EnvironmentDesc> environment;

    // 水块（邻接 Sea 合并——程序化水面四边形——.scene 二进制/JSON 同构；OnSceneConstructReady 消费）
    std::vector<Resource::WaterBlockDesc> waterBlocks;

    // 完整场景环境（precomputed/entityMotionPolicy；静态函数无法访问 SceneConstructor::m_desc，
    // ConstructEntity 需显式传入此引用）
    Resource::SceneEnvironment sceneEnvironment;

    // 块划分配置（UE World Partition 模式：JSON 显式配置或加载时推导——缺失则按实体世界范围
    // 推导 cellSize = clamp(mapExtent / blocksPerAxis)；Phase C 区块划分消费，见 GPU-Drive.md §4.1）
    std::optional<Resource::BlockConfigDesc> blockConfig;

    // 空间索引世界范围（OctreeSystem：JSON 显式配置或加载时推导——缺失则按实体 worldBounds
    // 推导 worldSize = max(span)*1.2 + 中心；OctreeCullingSystem 初始化消费，见
    // OctreeCullingAndRaycaster.md §7.5）
    std::optional<Resource::WorldConfigDesc> worldConfig;

    // 天空盒 GPU 资源句柄（Tab 切换时重建天空盒用）
    Resource::GpuResourceHandle skyboxTextureHandle;
    Resource::GeometryHandle skyboxGeometryHandle;

    // 原始场景资产依赖（保留 key → 相对路径映射，用于编辑器重新导出时正确序列化）
    Resource::SceneDependencies originalDependencies;

    // 原始内联材质定义（用于编辑器重新导出时正确序列化）
    std::unordered_map<std::string, Resource::MaterialDesc> originalMaterials;

    // 原始 baseURL（用于编辑器重新导出）
    std::string baseURL;
};

class SceneConstructor {
public:
    SceneConstructor() = default;
    ~SceneConstructor() = default;

    SceneConstructor(const SceneConstructor &) = delete;
    SceneConstructor &operator=(const SceneConstructor &) = delete;

    using Callback = std::function<void(bool success)>;

    /// 加载场景（不直接构造实体，完成后发事件）
    void LoadScene(const Resource::SceneDescription &desc, Boot::GameContext *context,
                   Resource::HeapTag heapTag = Resource::HeapTag::Default, Callback onComplete = nullptr,
                   const std::string &sceneFilePath = "");

    /// 获取当前是否正在加载
    bool IsLoading() const { return m_loading; }

    /// 构造单个 entity（递归处理 children），供 SceneConstructSystem 调用
    /// @param sceneEnv 场景环境（precomputed/entityMotionPolicy；静态函数无法访问 m_desc，须显式传入）
    static void ConstructEntity(ECS::Entity entity, const Resource::EntityDesc &eDesc,
                                const std::unordered_map<std::string, Resource::GeometryHandle> &geoMap,
                                const std::unordered_map<std::string, Resource::MaterialHandle> &matMap,
                                const Resource::SceneEnvironment &sceneEnv, ECS::Registry *registry,
                                Boot::GameContext *context);

private:
    // 依赖加载完成后的处理
    void OnDependenciesLoaded();

    /// 场景所有依赖（含材质 buffer）GPU 就绪后的回调
    void OnSceneReady(uint64_t sceneId);

    Resource::SceneDescription m_desc;
    std::string m_sceneFilePath; // 原始场景文件路径
    Boot::GameContext *m_context = nullptr;
    Resource::HeapTag m_heapTag = Resource::HeapTag::Default;
    Resource::AssetBatchPtr m_batch; // 当前加载批次的指针，用于追加子任务
    Callback m_onComplete;
    bool m_loading = false;

    // 原始场景数据快照（路径解析前的依赖/materials/baseURL，供编辑器导出用）
    Resource::SceneDependencies m_originalDependencies;
    std::unordered_map<std::string, Resource::MaterialDesc> m_originalMaterials;
    std::string m_originalBaseURL;

    // 程序化生成的天空盒几何体句柄（Tab 切换时保持存活）
    std::vector<Resource::GeometryHandle> m_skyboxProceduralHandles;
};

} // namespace Scene
} // namespace DX12Engine
