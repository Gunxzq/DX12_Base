#pragma once
#include <DirectXMath.h>

namespace DX12Engine::ECS {

// 光源组件（编辑器可操作的数据，渲染时同步到 LightManager）
// 方向光使用 transform.rotation 表达朝向，点/聚光灯使用 transform.position 表达位置
struct LightComponent {
    float type = 0.0f;                                    // 0=Directional, 1=Point, 2=Spot
    DirectX::XMFLOAT4 strength = {1.0f, 1.0f, 1.0f, 100.0f};
    float range = 10.0f;
    float falloffStart = 1.0f;
    float falloffEnd = 30.0f;
    float spotPower = 8.0f;
    float castShadow = 1.0f;
    float shadowBias = 0.005f;
};

} // namespace DX12Engine::ECS
