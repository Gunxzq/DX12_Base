#pragma once

// ========================================================================
// 组件编辑器注册入口
//
// 各组件编辑器的注册函数在此声明，由 Editor::Initialize() 在启动时调用。
// 每个注册函数通过 ComponentEditorRegistry::Register<T>() 将组件的编辑方法
// 注册到全局注册表中，供 EditorLayout::DrawProperties() 遍历使用。
// ========================================================================

namespace DX12Engine::ECS {

/// 注册 TransformComponent 编辑方法
void RegisterTransformEditor();

/// 注册 LightComponent 编辑方法
void RegisterLightEditor();

} // namespace DX12Engine::ECS