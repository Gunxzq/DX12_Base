#include "SkeletonData.h"
#include <algorithm>
#include <cfloat>

using namespace DirectX;

namespace DX12Engine::Resource {

// ============================================================================
// BoneAnimation
// ============================================================================

float BoneAnimation::GetStartTime() const { return Keyframes.empty() ? 0.0f : Keyframes.front().TimePos; }

float BoneAnimation::GetEndTime() const { return Keyframes.empty() ? 0.0f : Keyframes.back().TimePos; }

void BoneAnimation::Interpolate(float t, XMFLOAT4X4 &outMatrix) const {
    if (Keyframes.empty()) {
        XMStoreFloat4x4(&outMatrix, XMMatrixIdentity());
        return;
    }

    if (t <= Keyframes.front().TimePos) {
        XMVECTOR S = XMLoadFloat3(&Keyframes.front().Scale);
        XMVECTOR P = XMLoadFloat3(&Keyframes.front().Translation);
        XMVECTOR Q = XMLoadFloat4(&Keyframes.front().RotationQuat);
        XMVECTOR zero = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
        XMStoreFloat4x4(&outMatrix, XMMatrixAffineTransformation(S, zero, Q, P));
        return;
    }

    if (t >= Keyframes.back().TimePos) {
        XMVECTOR S = XMLoadFloat3(&Keyframes.back().Scale);
        XMVECTOR P = XMLoadFloat3(&Keyframes.back().Translation);
        XMVECTOR Q = XMLoadFloat4(&Keyframes.back().RotationQuat);
        XMVECTOR zero = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
        XMStoreFloat4x4(&outMatrix, XMMatrixAffineTransformation(S, zero, Q, P));
        return;
    }

    for (size_t i = 0; i < Keyframes.size() - 1; ++i) {
        if (t >= Keyframes[i].TimePos && t <= Keyframes[i + 1].TimePos) {
            float lerpPercent = (t - Keyframes[i].TimePos) / (Keyframes[i + 1].TimePos - Keyframes[i].TimePos);

            XMVECTOR s0 = XMLoadFloat3(&Keyframes[i].Scale);
            XMVECTOR s1 = XMLoadFloat3(&Keyframes[i + 1].Scale);
            XMVECTOR p0 = XMLoadFloat3(&Keyframes[i].Translation);
            XMVECTOR p1 = XMLoadFloat3(&Keyframes[i + 1].Translation);
            XMVECTOR q0 = XMLoadFloat4(&Keyframes[i].RotationQuat);
            XMVECTOR q1 = XMLoadFloat4(&Keyframes[i + 1].RotationQuat);

            XMVECTOR S = XMVectorLerp(s0, s1, lerpPercent);
            XMVECTOR P = XMVectorLerp(p0, p1, lerpPercent);
            XMVECTOR Q = XMQuaternionSlerp(q0, q1, lerpPercent);

            XMVECTOR zero = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
            XMStoreFloat4x4(&outMatrix, XMMatrixAffineTransformation(S, zero, Q, P));
            return;
        }
    }
}

// ============================================================================
// AnimationClip
// ============================================================================

float AnimationClip::GetClipStartTime() const {
    float t = FLT_MAX;
    for (const auto &anim : BoneAnimations) {
        t = std::min(t, anim.GetStartTime());
    }
    return t;
}

float AnimationClip::GetClipEndTime() const {
    float t = 0.0f;
    for (const auto &anim : BoneAnimations) {
        t = std::max(t, anim.GetEndTime());
    }
    return t;
}

void AnimationClip::Interpolate(float t, std::vector<XMFLOAT4X4> &outBoneTransforms) const {
    for (size_t i = 0; i < BoneAnimations.size(); ++i) {
        BoneAnimations[i].Interpolate(t, outBoneTransforms[i]);
    }
}

// ============================================================================
// SkeletonData
// ============================================================================

void SkeletonData::GetFinalTransforms(const std::string &clipName, float timePos,
                                      std::vector<XMFLOAT4X4> &outFinalTransforms) const {
    auto it = Animations.find(clipName);
    if (it == Animations.end()) {
        // 找不到动画片段，回退到绑定姿势
        for (uint32_t i = 0; i < BoneCount(); ++i) {
            XMStoreFloat4x4(&outFinalTransforms[i], XMMatrixTranspose(XMLoadFloat4x4(&BoneOffsets[i])));
        }
        return;
    }

    const AnimationClip &clip = it->second;
    uint32_t numBones = BoneCount();
    std::vector<XMFLOAT4X4> toParentTransforms(numBones);

    // 插值所有骨骼
    clip.Interpolate(timePos, toParentTransforms);

    // 遍历层次，计算到根空间的变换
    std::vector<XMFLOAT4X4> toRootTransforms(numBones);
    toRootTransforms[0] = toParentTransforms[0];

    for (uint32_t i = 1; i < numBones; ++i) {
        XMMATRIX toParent = XMLoadFloat4x4(&toParentTransforms[i]);
        int parentIndex = BoneHierarchy[i];
        XMMATRIX parentToRoot = XMLoadFloat4x4(&toRootTransforms[parentIndex]);
        XMMATRIX toRoot = XMMatrixMultiply(toParent, parentToRoot);
        XMStoreFloat4x4(&toRootTransforms[i], toRoot);
    }

    // 乘以偏移矩阵，输出最终变换（转置后供GPU使用）
    for (uint32_t i = 0; i < numBones; ++i) {
        XMMATRIX offset = XMLoadFloat4x4(&BoneOffsets[i]);
        XMMATRIX toRoot = XMLoadFloat4x4(&toRootTransforms[i]);
        XMMATRIX finalTransform = XMMatrixMultiply(offset, toRoot);
        XMStoreFloat4x4(&outFinalTransforms[i], XMMatrixTranspose(finalTransform));
    }
}

} // namespace DX12Engine::Resource
