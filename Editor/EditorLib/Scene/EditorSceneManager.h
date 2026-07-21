#pragma once

#include "Scene/SceneManager.h"
#include <filesystem>
#include <memory>
#include <unordered_map>

namespace DX12Engine::Scene {
struct SceneConstructData;
} // namespace DX12Engine::Scene

// ========================================================================
// EditorSceneManager — 编辑器场景管理器
//
// 组合包装 SceneManager，提供编辑器特有的场景管理能力：
//   - 实体描述缓存（EntityDesc 用于导出）
//   - 注册 EditorSceneConstructSystem（响应 GeneratorTaskCompleteEvent）
//   - 场景文件保存/导出（JSON 序列化）
//
// 注意：场景加载是 AssetManager 的编排职责，EditorSceneManager 不负责加载。
//       它只提供实体管理的接口（CreateEntity/RegisterEntity），
//       由 SceneConstructSystem 在异步加载完成后调用。
// ========================================================================

class EditorSceneManager {
public:
    EditorSceneManager() = default;
    ~EditorSceneManager() = default;

    EditorSceneManager(const EditorSceneManager&) = delete;
    EditorSceneManager& operator=(const EditorSceneManager&) = delete;

    // ====================================================================
    // 初始化/销毁
    // ====================================================================

    /// 初始化（绑定被包装的 SceneManager）
    /// @param sceneMgr 由 Bootstrap 创建的 SceneManager 实例
    /// @param context  GameContext 指针
    void Initialize(DX12Engine::Scene::SceneManager* sceneMgr,
                    DX12Engine::Boot::GameContext* context);

    /// 销毁，清理所有实体和子场景
    void Shutdown();

    // ====================================================================
    // 包装方法（委托给 SceneManager）
    // ====================================================================

    /// 获取被包装的 SceneManager 指针（供 OutlinerPanel/EditorViewportInput 等使用）
    DX12Engine::Scene::SceneManager* GetSceneManager() const { return m_sceneMgr; }

    /// 创建空实体
    uint64_t CreateEntity() { return m_sceneMgr ? m_sceneMgr->CreateEntity() : UINT64_MAX; }

    /// 注册实体
    void RegisterEntity(uint64_t entity) { if (m_sceneMgr) m_sceneMgr->RegisterEntity(entity); }

    /// 获取内部 Registry 指针（供 Builder 系统获取 ECS 上下文）
    DX12Engine::ECS::Registry* GetRegistry() const { return m_sceneMgr ? m_sceneMgr->GetRegistry() : nullptr; }

    // ====================================================================
    // 场景构造系统注册
    // ====================================================================

    /// 注册 EditorSceneConstructSystem（响应 GeneratorTaskCompleteEvent）
    void RegisterSceneConstructSystem();

    // ====================================================================
    // 场景文件管理（编辑器特有）
    // ====================================================================

    /// 新建空白场景
    void NewScene(const std::string& name);

    /// 保存场景（序列化为 SceneDescription → JSON）
    void SaveScene();
    void SaveSceneAs(const std::filesystem::path& filePath);

    /// 获取当前场景文件路径
    const std::filesystem::path& GetSceneFilePath() const { return m_sceneFilePath; }

    /// 检查场景是否有未保存的修改
    bool IsDirty() const { return m_dirty; }
    void MarkDirty() { m_dirty = true; }
    void ClearDirty() { m_dirty = false; }

    // ====================================================================
    // EntityDesc 编辑（编辑器特有）
    // ====================================================================

    /// 获取可编辑的 EntityDesc 列表（供 Outliner 编辑）
    DX12Engine::Resource::EntityDesc* GetMutableEntityDesc(uint64_t entity);

    /// 更新实体描述（Outliner 编辑后调用）
    void UpdateEntityDesc(uint64_t entity, const DX12Engine::Resource::EntityDesc& newDesc);

    /// 导出当前场景为 SceneDescription（用于序列化）
    DX12Engine::Resource::SceneDescription ExportToDescription() const;

    /// 获取默认场景描述（标准天空盒 + 空世界）
    static DX12Engine::Resource::SceneDescription GetDefaultSceneDescription();

private:
    // ====================================================================
    // 内部方法
    // ====================================================================

    /// 场景构造完成后的处理（从 SceneConstructData 创建实体）
    void OnSceneConstructReady(const DX12Engine::Scene::SceneConstructData& sceneData);

private:
    DX12Engine::Scene::SceneManager* m_sceneMgr = nullptr;
    DX12Engine::Boot::GameContext* m_context = nullptr;

    // 当前编辑的场景文件路径
    std::filesystem::path m_sceneFilePath;
    bool m_dirty = false;

    // 可编辑的 EntityDesc 列表（与 ECS 实体双向映射）
    std::unordered_map<uint64_t, DX12Engine::Resource::EntityDesc> m_entityDescs;

    bool m_initialized = false;
};