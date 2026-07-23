#pragma once

#include "ECS/Core/Entity.h"
#include <functional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace DX12Engine::ECS { class Registry; }

// ========================================================================
// ComponentEditorRegistry — ECS 组件编辑器注册器
//
// 属性卡的组件编辑方法由各组件模块独立注册，EditorLayout 遍历注册表绘制。
// 控制逻辑分散到各组件，而非集中在 EditorLayout 中。
// ========================================================================

/// 组件编辑回调签名
/// @param registry  ECS Registry（用于读写组件数据）
/// @param entity    当前选中的实体 ID
using ComponentEditorFn = std::function<void(DX12Engine::ECS::Registry*, DX12Engine::ECS::Entity)>;

/// 已注册的组件编辑器信息
struct ComponentEditorInfo {
    std::string typeName;       ///< 组件显示名称（如 "Transform", "Light"）
    std::string category;       ///< 分组类别（用于折叠）
    ComponentEditorFn drawFn;   ///< 绘制回调
};

class ComponentEditorRegistry {
public:
    /// 注册组件类型的编辑方法
    /// @tparam T       组件类型
    /// @param typeName 组件显示名称（如 "Transform", "Light"）
    /// @param category 分组类别（用于折叠，如 "Transform", "Lighting"）
    /// @param drawFn   绘制回调
    template<typename T>
    static void Register(const char* typeName, const char* category, ComponentEditorFn drawFn) {
        auto& instance = GetInstance();
        std::type_index idx(typeid(T));
        instance.m_editors[idx] = {typeName, category, std::move(drawFn)};
    }

    /// 获取组件的编辑方法（不存在返回 nullptr）
    static const ComponentEditorInfo* Get(std::type_index typeIndex) {
        auto& instance = GetInstance();
        auto it = instance.m_editors.find(typeIndex);
        if (it != instance.m_editors.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /// 检查组件是否已注册编辑器
    template<typename T>
    static bool Has() {
        auto& instance = GetInstance();
        return instance.m_editors.find(std::type_index(typeid(T))) != instance.m_editors.end();
    }

    /// 获取所有已注册的编辑器列表（用于遍历绘制）
    static const std::unordered_map<std::type_index, ComponentEditorInfo>& GetAll() {
        return GetInstance().m_editors;
    }

    /// 清空所有注册（用于 Shutdown/重初始化）
    static void Clear() {
        GetInstance().m_editors.clear();
    }

private:
    ComponentEditorRegistry() = default;
    ~ComponentEditorRegistry() = default;

    ComponentEditorRegistry(const ComponentEditorRegistry&) = delete;
    ComponentEditorRegistry& operator=(const ComponentEditorRegistry&) = delete;

    static ComponentEditorRegistry& GetInstance() {
        static ComponentEditorRegistry s_instance;
        return s_instance;
    }

    std::unordered_map<std::type_index, ComponentEditorInfo> m_editors;
};