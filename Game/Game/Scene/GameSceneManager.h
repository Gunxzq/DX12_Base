#pragma once

#include "Scene/SceneManager.h"
#include "Scene/SceneConstructor.h"
#include <memory>
#include <string>

namespace DX12Engine::Scene {
struct SceneConstructData;
} // namespace DX12Engine::Scene

// ========================================================================
// GameSceneManager — 游戏场景管理器
//
// 组合包装 SceneManager，提供游戏运行时特有的场景管理能力：
//   - 注册 SceneConstructSystem（响应 GeneratorTaskCompleteEvent）
//   - 异步场景加载编排
//   - 关卡加载/切换（后续扩展）
//   - 流式加载调度（后续扩展）
//
// 设计原则：
//   - SceneManager 负责实体生命周期管理（CreateEntity）
//   - GameSceneManager 负责游戏运行时场景编排
//   - SceneConstructor 作为加载器，通过 CreateEntity 填充实体
// ========================================================================

class GameSceneManager {
public:
    GameSceneManager() = default;
    ~GameSceneManager() = default;

    GameSceneManager(const GameSceneManager&) = delete;
    GameSceneManager& operator=(const GameSceneManager&) = delete;

    // ====================================================================
    // 初始化/销毁
    // ====================================================================

    /// 初始化（绑定被包装的 SceneManager）
    /// @param sceneMgr 由 Bootstrap 创建的 SceneManager 实例
    /// @param context  GameContext 指针
    void Initialize(DX12Engine::Scene::SceneManager* sceneMgr,
                    DX12Engine::Boot::GameContext* context);

    /// 销毁
    void Shutdown();

    // ====================================================================
    // 包装方法（委托给 SceneManager）
    // ====================================================================

    /// 获取被包装的 SceneManager 指针
    DX12Engine::Scene::SceneManager* GetSceneManager() const { return m_sceneMgr; }

    /// 创建空实体
    uint64_t CreateEntity() { return m_sceneMgr ? m_sceneMgr->CreateEntity() : UINT64_MAX; }

    /// 获取内部 Registry 指针（供 Builder 系统获取 ECS 上下文）
    DX12Engine::ECS::Registry* GetRegistry() const { return m_sceneMgr ? m_sceneMgr->GetRegistry() : nullptr; }

    // ====================================================================
    // 场景构造系统注册
    // ====================================================================

    /// 注册 SceneConstructSystem（响应 GeneratorTaskCompleteEvent）
    void RegisterSceneConstructSystem();

    // ====================================================================
    // 异步场景加载
    // ====================================================================

    /// 异步加载场景文件
    /// @param filePath .scene.json 文件路径
    void LoadSceneAsync(const std::string& filePath);

    /// 获取当前是否正在加载场景
    bool IsLoading() const { return m_sceneCtor && m_sceneCtor->IsLoading(); }

private:
    // ====================================================================
    // 内部方法
    // ====================================================================

    /// 场景构造完成后的处理（从 SceneConstructData 创建实体）
    void OnSceneConstructReady(const DX12Engine::Scene::SceneConstructData& sceneData);

private:
    DX12Engine::Scene::SceneManager* m_sceneMgr = nullptr;
    DX12Engine::Boot::GameContext* m_context = nullptr;

    // 场景构造器（异步加载用）
    std::unique_ptr<DX12Engine::Scene::SceneConstructor> m_sceneCtor;

    bool m_initialized = false;
};