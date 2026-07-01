#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Engine::Resource {

// ============================================================================
// 关键帧定义
// ============================================================================
struct Keyframe {
    float TimePos = 0.0f;
    DirectX::XMFLOAT3 Translation = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 Scale = {1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT4 RotationQuat = {0.0f, 0.0f, 0.0f, 1.0f};
};

// ============================================================================
// 骨骼动画（一根骨头的所有关键帧）
// ============================================================================
struct BoneAnimation {
    std::vector<Keyframe> Keyframes;

    float GetStartTime() const;
    float GetEndTime() const;
    void Interpolate(float t, DirectX::XMFLOAT4X4 &outMatrix) const;
};

// ============================================================================
// 动画片段（如"Walk"、"Attack"，包含所有骨骼的动画）
// ============================================================================
struct AnimationClip {
    std::vector<BoneAnimation> BoneAnimations;

    float GetClipStartTime() const;
    float GetClipEndTime() const;
    void Interpolate(float t, std::vector<DirectX::XMFLOAT4X4> &outBoneTransforms) const;
};

// ============================================================================
// 骨骼数据（一个完整骨架的所有数据）
// ============================================================================
struct SkeletonData {
    std::vector<int> BoneHierarchy;                            // 父骨骼索引
    std::vector<DirectX::XMFLOAT4X4> BoneOffsets;              // 偏移矩阵（绑定姿势逆矩阵）
    std::vector<std::string> BoneNames;                        // 骨骼名称
    std::unordered_map<std::string, AnimationClip> Animations; // 动画片段列表

    uint32_t BoneCount() const { return static_cast<uint32_t>(BoneHierarchy.size()); }
    bool IsValid() const { return !BoneHierarchy.empty(); }

    // 计算最终变换矩阵（从骨骼局部空间到根空间，再乘以偏移矩阵）
    void GetFinalTransforms(const std::string &clipName, float timePos,
                            std::vector<DirectX::XMFLOAT4X4> &outFinalTransforms) const;
};

} // namespace DX12Engine::Resource
