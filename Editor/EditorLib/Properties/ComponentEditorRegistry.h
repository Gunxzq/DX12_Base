#pragma once

#include "ECS/Core/Entity.h"
#include <functional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace DX12Engine::ECS {
class Registry;
}

// ========================================================================
// ComponentEditorRegistry — ECS 组件编辑器注册器
//
// 属性卡的组件编辑方法由各组件模块独立注册，EditorLayout 遍历注册表绘制。
// 控制逻辑分散到各组件，而非集中在 EditorLayout 中。
// ========================================================================

// 组件编辑回调
using ComponentEditorFn = std::function<void(DX12Engine::ECS::Registry *, DX12Engine::ECS::Entity)>;

// 组件存在性检查
using ComponentHasFn = std::function<bool(DX12Engine::ECS::Registry *, DX12Engine::ECS::Entity)>;

// 组件创建
using ComponentAddFn = std::function<void(DX12Engine::ECS::Registry *, DX12Engine::ECS::Entity)>;

// 组件移除
using ComponentRemoveFn = std::function<void(DX12Engine::ECS::Registry *, DX12Engine::ECS::Entity)>;

/// 已注册的组件编辑器信息
struct ComponentEditorInfo {
    std::string typeName;       ///< 组件显示名称（如 "Transform", "Light"）
    std::string category;       ///< 分组类别（用于折叠）
    ComponentEditorFn drawFn;   ///< 绘制回调
    ComponentHasFn hasFn;       ///< 检查组件是否存在于实体上
    ComponentAddFn addFn;       ///< 在实体上创建组件
    ComponentRemoveFn removeFn; ///< 从实体上移除组件
};

class ComponentEditorRegistry {
public:
    template <typename T>
    static void Register(const char *typeName, const char *category, ComponentEditorFn drawFn, bool removable = true) {
        auto &instance = GetInstance();
        std::type_index idx(typeid(T));
        instance.m_editors[idx] = {
            typeName, category, std::move(drawFn),
            /* hasFn  */ [](DX12Engine::ECS::Registry *r, DX12Engine::ECS::Entity e) { return r->HasComponent<T>(e); },
            /* addFn  */ [](DX12Engine::ECS::Registry *r, DX12Engine::ECS::Entity e) { r->AddComponent<T>(e); },
            /* removeFn */
            removable ? ComponentRemoveFn(
                            [](DX12Engine::ECS::Registry *r, DX12Engine::ECS::Entity e) { r->RemoveComponent<T>(e); })
                      : ComponentRemoveFn{}};
    }

    static const ComponentEditorInfo *Get(std::type_index typeIndex) {
        auto &instance = GetInstance();
        auto it = instance.m_editors.find(typeIndex);
        if (it != instance.m_editors.end()) {
            return &it->second;
        }
        return nullptr;
    }

    template <typename T> static bool Has() {
        auto &instance = GetInstance();
        return instance.m_editors.find(std::type_index(typeid(T))) != instance.m_editors.end();
    }

    static const std::unordered_map<std::type_index, ComponentEditorInfo> &GetAll() { return GetInstance().m_editors; }

    static void Clear() { GetInstance().m_editors.clear(); }

private:
    ComponentEditorRegistry() = default;
    ~ComponentEditorRegistry() = default;

    ComponentEditorRegistry(const ComponentEditorRegistry &) = delete;
    ComponentEditorRegistry &operator=(const ComponentEditorRegistry &) = delete;

    static ComponentEditorRegistry &GetInstance() {
        static ComponentEditorRegistry s_instance;
        return s_instance;
    }

    std::unordered_map<std::type_index, ComponentEditorInfo> m_editors;
};