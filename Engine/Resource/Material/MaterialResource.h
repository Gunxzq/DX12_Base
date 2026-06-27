#pragma once
#include "Common/d3dUtil.h"
#include "Math/HashTypes.h"
#include "Resource/Struct/MaterialHandle.h"
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
    uint32_t baseColorTextureId = 0;
    uint32_t normalTextureId = 0xFFFFFFFF;
    uint32_t metallicRoughnessTextureId = 0xFFFFFFFF;
    uint32_t emissiveTextureId = 0xFFFFFFFF;
    uint32_t occlusionTextureId = 0xFFFFFFFF;

    // ── 渲染器标识（绑定到具体的渲染管线）──
    uint64_t rendererTypeHash = 0;
};

} // namespace DX12Engine::Resource
