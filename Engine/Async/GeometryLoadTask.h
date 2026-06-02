#pragma once

#include "AsyncLoadTask.h"
#include "Resource/Manager/GeometryResourceManager.h"

namespace DX12Engine::Async {

// 定义事件哈希（需要在 EventRegistry.h 中注册）
constexpr uint32_t GEOMETRY_LOADED_EVENT_HASH = 0x00000200;

class GeometryLoadTask : public AsyncLoadTask<Resource::GeometryHandle, GEOMETRY_LOADED_EVENT_HASH> {
public:
    GeometryLoadTask(uint32_t requestId, const std::string &path, const std::string &name = "")
        : AsyncLoadTask(requestId, path, name.empty() ? "GeometryLoadTask" : name) {}

protected:
    Resource::GeometryHandle LoadInternal(const std::string &path) override {
        // 1. 加载几何体
        auto data = AssetLoader::LoadMesh(path);
        if (!data)
            return Resource::GeometryHandle::Invalid();

        // 2. 创建 GPU 资源
        Resource::GeometryHandle handle = GeometryResourceManager::GetInstance().CreateFromData(data);

        // 3. 获取对应的组合任务并设置句柄
        // 方式一：通过 requestId 查找（需要维护全局映射）
        auto combineTask = GetCombineTask(m_requestId);
        if (combineTask) {
            combineTask->SetGeometryHandle(handle);
        }

        return handle;
    }
};

} // namespace DX12Engine::Async