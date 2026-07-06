#pragma once
#include <cstdint>

namespace DX12Engine::ECS {

// 反射探针组件（编辑器可操作的数据，渲染时同步到 ReflectionProbeManager）
struct ReflectionProbeComponent {
    float captureRange = 50.0f;
    uint32_t resolution = 256;
    uint8_t updatePriority = 1;
};

// 静态绑定反射探针组件
struct ReflectionConsumerComponent {
    uint32_t probeIndex = UINT32_MAX;
    bool useDynamicFallback = true;
};

} // namespace DX12Engine::ECS
