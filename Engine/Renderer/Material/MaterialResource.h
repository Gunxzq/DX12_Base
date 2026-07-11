#pragma once
#include "Common/d3dUtil.h"
#include "Math/HashTypes.h"
#include "Renderer/Material/MaterialHandle.h"
#include <cstdint>
#include <string>

namespace DX12Engine::Resource {

// 材质数据（狭义的材质参数，不包含 PSO、纹理等）
struct MaterialData {

    // ── 资产标识 ──
    TypeHash materialId = 0; // 材质资产 ID（必须唯一，使用 TYPE_HASH 生成）
    std::string name;        // 调试名称（可选）

    // ── PBR 参数 ──
    DirectX::XMFLOAT4 baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;  // 金属度 (0 = 非金属, 1 = 金属)
    float roughness = 0.5f; // 粗糙度 (0 = 光滑, 1 = 粗糙)
    float ambient = 0.0f;   // 环境光遮蔽强度
    float alpha = 1.0f;     // 透明度 (0 = 全透明, 1 = 不透明)

    // ── 自发光 ──
    DirectX::XMFLOAT4 emissive = {0.0f, 0.0f, 0.0f, 1.0f};
    float normalIntensity = 1.0f;

    // ── Alpha 测试 ──
    float alphaCutoff = 0.5f;

    // ── 纹理 ID（逻辑索引，非 GPU 描述符）──
    uint32_t baseColorTextureId = 0xFFFFFFFF;         // 基础颜色纹理 ID
    uint32_t normalTextureId = 0xFFFFFFFF;            // 法线纹理 ID
    uint32_t metallicRoughnessTextureId = 0xFFFFFFFF; // 金属度/粗糙度贴图
    uint32_t emissiveTextureId = 0xFFFFFFFF;          // 自发光贴图
    uint32_t occlusionTextureId = 0xFFFFFFFF;         // AO 贴图
    uint32_t heightTextureId = 0xFFFFFFFF;            // Displacement/Height 贴图
    uint32_t opacityTextureId = 0xFFFFFFFF;           // 透明度贴图（优先于 BaseColor.a）
    uint32_t maskTextureId = 0xFFFFFFFF;              // 遮罩贴图
    uint32_t subsurfaceTextureId = 0xFFFFFFFF;        // 次表面散射贴图
    uint32_t clearCoatTextureId = 0xFFFFFFFF;         // 清漆层贴图

    // ── 渲染器标识（绑定到具体的渲染管线）──
    uint64_t rendererTypeHash = 0;
};

} // namespace DX12Engine::Resource
