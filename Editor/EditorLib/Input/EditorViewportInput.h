#pragma once

#include "Boot/GameContext.h"
#include "ECS/Core/Entity.h"
#include <DirectXMath.h>

// 前向声明
namespace DX12Engine::Scene { class SceneManager; }

// ========================================================================
// EditorViewportInput — 编辑器视口输入处理
//
// 功能：WASD 移动 + 鼠标右键拖拽旋转 + 鼠标滚轮调整速度
//       + F 键聚焦选中实体
// ========================================================================

class EditorViewportInput {
public:
    EditorViewportInput();
    ~EditorViewportInput();

    EditorViewportInput(const EditorViewportInput &) = delete;
    EditorViewportInput &operator=(const EditorViewportInput &) = delete;

    void Initialize(DX12Engine::Boot::GameContext *context);
    void Shutdown();

    /// 每帧更新（由 Editor ImmediateCallback 调用）
    void Update(float deltaTime);

    /// 设置视口尺寸（影响相机宽高比）
    void SetViewportSize(uint32_t width, uint32_t height);

    /// 设置视口悬停状态（由 Editor 每帧更新）
    void SetViewportHovered(bool hovered) { m_viewportHovered = hovered; }

    /// 聚焦到指定实体（保持当前视角方向，仅调整位置）
    /// @param entity 目标实体（需含有 TransformComponent）
    /// @param defaultDistance 无包围盒信息时的默认距离
    void FocusOnEntity(DX12Engine::ECS::Entity entity, float defaultDistance = 10.0f);

    /// 设置场景管理器引用（用于 FocusOnEntity 获取实体位置）
    void SetEditorSceneManager(DX12Engine::Scene::SceneManager *mgr) { m_editorSceneMgr = mgr; }

private:
    void HandleCameraInput(float deltaTime);

    // 龙书风格相机操作（复制自 GameInputHandler）
    void Pitch(float angle);    // 绕 Right 轴旋转（上下看）
    void RotateY(float angle);  // 绕世界 Y 轴旋转（左右看）
    void Strafe(float d);       // 沿 Right 轴平移（左右移动）
    void Walk(float d);         // 沿 Forward 轴平移（前后移动）

    DX12Engine::Boot::GameContext *m_context = nullptr;
    DX12Engine::Scene::SceneManager *m_editorSceneMgr = nullptr;

    // 相机移动参数
    float m_moveSpeed = 50.0f;
    float m_sprintMultiplier = 3.0f;
    float m_verticalSpeed = 30.0f;

    uint32_t m_viewportWidth = 1280;
    uint32_t m_viewportHeight = 720;
    bool m_viewportHovered = false;
    bool m_initialized = false;
};