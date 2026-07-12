#pragma once

#include "Asset/IO/Loader/SceneDescription.h"
#include "ECS/Core/Entity.h"
#include "Renderer/Material/MaterialHandle.h"
#include "Resource/AssetManager/AssetManager.h"
#include "Resource/GpuResourceManager.h"
#include <cstdint>
#include <functional>
#include <memory>
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
// SceneConstructor — 场景构造系统
//
// 接收 SceneDescription（已解析的场景 JSON），
// 异步加载所有依赖资产，完成后发送 SceneConstructReadyEvent，
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
    std::vector<Resource::EntityDesc> entities;
    std::unordered_map<std::string, Resource::GeometryHandle> geoMap;
    std::unordered_map<std::string, Resource::MaterialHandle> matMap;
};

class SceneConstructor {
public:
    SceneConstructor() = default;
    ~SceneConstructor() = default;

    SceneConstructor(const SceneConstructor &) = delete;
    SceneConstructor &operator=(const SceneConstructor &) = delete;

    using Callback = std::function<void(bool success)>;

    /// 加载场景（不直接构造实体，完成后发事件）
    void LoadScene(const Resource::SceneDescription &desc, Boot::GameContext *context, Callback onComplete = nullptr);

    /// 获取当前是否正在加载
    bool IsLoading() const { return m_loading; }

    /// 构造单个 entity（递归处理 children），供 SceneConstructSystem 调用
    static void ConstructEntity(ECS::Entity entity, const Resource::EntityDesc &eDesc,
                                const std::unordered_map<std::string, Resource::GeometryHandle> &geoMap,
                                const std::unordered_map<std::string, Resource::MaterialHandle> &matMap,
                                ECS::Registry *registry, Boot::GameContext *context);

private:
    // 依赖加载完成后的处理
    void OnDependenciesLoaded();

    /// 场景所有依赖（含材质 buffer）GPU 就绪后的回调
    void OnSceneReady(uint64_t sceneId);

    Resource::SceneDescription m_desc;
    Boot::GameContext *m_context = nullptr;
    Resource::AssetBatchPtr m_batch; // 当前加载批次的指针，用于追加子任务
    Callback m_onComplete;
    bool m_loading = false;
};

} // namespace Scene
} // namespace DX12Engine
