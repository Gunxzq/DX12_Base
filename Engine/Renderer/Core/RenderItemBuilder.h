// Renderer/Core/RenderItemBuilder.h
#pragma once
#include "ECS/Core/Registry.h"
#include "Renderer/Core/CullingSystem.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/Core/RenderItem.h"
#include "Renderer/Core/RenderQueue.h"
#include "Resource/Struct/GeometryHandle.h"
#include <unordered_map>

namespace DX12Engine::Renderer {

// 前向声明
class CameraManager;

// ============================================================================
// 渲染项构建器 - 将 ECS 数据转换为渲染项
// ============================================================================

class RenderItemBuilder {
public:
    RenderItemBuilder() = default;
    ~RenderItemBuilder() = default;

    // 禁止拷贝
    RenderItemBuilder(const RenderItemBuilder &) = delete;
    RenderItemBuilder &operator=(const RenderItemBuilder &) = delete;

    void SetCameraManager(CameraManager *mgr) { m_cameraManager = mgr; }

    void Execute(ECS::Registry &registry, const CullingResult &cullingResult, const LODResult &lodResult,
                 RenderQueue &outQueue);

private:
    float CalculateDepth(const DirectX::XMFLOAT3 &pos, const DirectX::XMFLOAT3 &cameraPos) const;
    uint64_t BuildSortKey(uint32_t materialId, float depth, bool isTransparent) const;

    CameraManager *m_cameraManager = nullptr;
};

} // namespace DX12Engine::Renderer