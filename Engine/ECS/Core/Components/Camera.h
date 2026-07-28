#pragma once

namespace DX12Engine::ECS {

// 投影类型
enum class ProjectionType : uint8_t {
    Perspective = 0,
    Orthographic = 1
};

// 相机组件（编辑器可操作的数据，渲染时同步到 CameraManager）
// fov 单位为度，与 .scene.json 格式一致
struct CameraComponent {
    float fov = 60.0f;                    ///< 视野角度（度），Perspective 时使用
    float orthoSize = 10.0f;              ///< 正交视口高度，Orthographic 时使用
    float nearPlane = 0.1f;               ///< 近裁剪面
    float farPlane = 1000.0f;             ///< 远裁剪面
    ProjectionType projection = ProjectionType::Perspective;  ///< 投影类型
    bool isMain = false;                  ///< 是否为主相机
};

} // namespace DX12Engine::ECS
