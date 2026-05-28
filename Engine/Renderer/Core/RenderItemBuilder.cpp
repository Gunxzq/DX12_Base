#include "RenderItemBuilder.h"
#include "ECS/Core/Components.h"
#include "Renderer/Scene/CameraManager.h"
#include "Resource/Manager/MaterialManager.h"
#include <iostream>

#include <cmath>

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

// ============================================================================
// 执行
// ============================================================================

void RenderItemBuilder::Execute(ECS::Registry &registry, const CullingResult &cullingResult, const LODResult &lodResult,
                                RenderQueue &outQueue) {

    // 清空上一帧的队列
    outQueue.Clear();

    // 获取相机位置（用于深度计算）
    DirectX::XMFLOAT3 cameraPos = {0.0f, 0.0f, 0.0f};
    if (m_cameraManager) {
        const auto &camera = m_cameraManager->GetMainCamera();
        cameraPos = camera.Position;
    }

    for (auto entity : cullingResult.visibleEntities) {

        auto &meshComp = registry.GetComponent<ECS::MeshComponent>(entity);

        Resource::GeometryHandle handle = lodResult.GetHandle(entity);
        if (!handle.IsValid()) {
            continue;
        }
        Resource::MaterialHandle materialHandle = meshComp.materialHandle;
         Resource::TextureHandle textureHandle = meshComp.textureHandle;
         Resource::MaterialData *material = m_materialManager->GetMaterial(materialHandle);
        

        if (!materialHandle.IsValid()) {
            continue;
        }

        if (!textureHandle.IsValid()) {
            continue;
        }

        // 3. 获取 Transform
        auto &transform = registry.GetComponent<ECS::TransformComponent>(entity);

        // 4. 计算深度
        float depth = CalculateDepth(transform.position, cameraPos);

        // 5. 构建排序键（当前阶段透明物体统一按深度排序）
        //    TODO: 后续支持材质系统时，排序键应包含材质 ID
        bool isTransparent = false; // 当前阶段无透明物体
        uint64_t sortKey = BuildSortKey(0, depth, isTransparent);

        // 构建 ObjectConstants
        XMMATRIX world = transform.GetMatrix();
        XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

        ObjectConstants objCB;
        XMStoreFloat4x4(&objCB.World, world);
        XMStoreFloat4x4(&objCB.WorldInvTranspose, worldInvTranspose);

        // 直接分配 GPU 地址
        D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress =
            m_frameResourceManager->AllocateObjectCB(&objCB, sizeof(ObjectConstants));

        MaterialConstants matCB = m_materialManager->GetGPUConstants(materialHandle);

        D3D12_GPU_VIRTUAL_ADDRESS materialCBAddress =
            m_frameResourceManager->AllocateMaterialCB(&matCB, sizeof(MaterialConstants));

        // 6. 构建 RenderItem
        RenderItem item;
        item.geometryHandle = handle;
        item.materialHandle = materialHandle;
        item.worldMatrix = transform.GetMatrix();
        item.objectCBAddress = objectCBAddress;
        item.materialCBAddress = materialCBAddress;
        item.depth = depth;
        item.sortKey = sortKey;
        item.textureSRV = m_textureManager->GetSRV(textureHandle);

        outQueue.Add(item);
    }
}

// ============================================================================
// 辅助方法
// ============================================================================

float RenderItemBuilder::CalculateDepth(const DirectX::XMFLOAT3 &pos, const DirectX::XMFLOAT3 &cameraPos) const {
    float dx = pos.x - cameraPos.x;
    float dy = pos.y - cameraPos.y;
    float dz = pos.z - cameraPos.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

uint64_t RenderItemBuilder::BuildSortKey(uint32_t materialId, float depth, bool isTransparent) const {
    // 位布局：
    //   Bits 63:      透明标志（1 = 透明）
    //   Bits 62-48:   保留
    //   Bits 47-32:   材质 ID
    //   Bits 31-0:    深度值
    uint64_t transparentBit = isTransparent ? (1ULL << 63) : 0;
    uint64_t materialPart = (static_cast<uint64_t>(materialId) & 0xFFFF) << 32;

    // 深度值转换为 32 位整数（精度 0.001 米）
    uint32_t depthInt = static_cast<uint32_t>(depth * 1000.0f);

    if (isTransparent) {
        // 透明物体：远到近（深度值大的排前面）
        depthInt = 0xFFFFFFFF - depthInt;
    }

    return transparentBit | materialPart | depthInt;
}

} // namespace DX12Engine::Renderer