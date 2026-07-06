#pragma once
#include "Resource/Struct/SkeletonHandle.h"
#include <d3d12.h>
#include <string>

namespace DX12Engine::ECS {

// 骨骼动画组件
struct SkinnedComponent {
    Resource::SkeletonHandle skeletonHandle;
    std::string currentClip;
    float timePos = 0.0f;
    D3D12_GPU_VIRTUAL_ADDRESS boneBufferAddress = 0;

    bool IsValid() const { return skeletonHandle.IsValid(); }
};

} // namespace DX12Engine::ECS
