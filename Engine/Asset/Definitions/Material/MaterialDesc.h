#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

// ============================================================================
// MaterialDesc — 材质描述（对应 .mat JSON 格式）
//
// 数据驱动材质定义：
//   .mat  JSON 文件 → MaterialLoader 解析 → MaterialDesc
//   → MaterialManager 注册 → 返回 MaterialHandle
//
// 支持两种模式：
//   值类型材质：仅 params，无纹理引用
//   全纹理材质：params + textures 纹理槽引用
// ============================================================================

namespace DX12Engine::Resource {

// ── PBR 参数 ──
struct MaterialParams {
    float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ao = 1.0f;
    float emissive[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float alphaCutoff = 0.0f; // 0 = 不裁剪
};

inline void from_json(const nlohmann::json &j, MaterialParams &p) {
    auto readFloat4 = [&](const std::string &name, float *out) {
        if (j.contains(name) && j[name].is_array()) {
            auto &arr = j[name];
            for (int i = 0; i < 4 && i < (int)arr.size(); ++i)
                out[i] = arr[i].get<float>();
        }
    };
    readFloat4("baseColor", p.baseColor);
    readFloat4("emissive", p.emissive);

    if (j.contains("metallic"))
        p.metallic = j["metallic"].get<float>();
    if (j.contains("roughness"))
        p.roughness = j["roughness"].get<float>();
    if (j.contains("ao"))
        p.ao = j["ao"].get<float>();
    if (j.contains("alphaCutoff"))
        p.alphaCutoff = j["alphaCutoff"].get<float>();
}

// ── 纹理槽 ──
struct MaterialTextureSlots {
    std::string baseColor;         // BC1/BC3/BC7 (sRGB)
    std::string normal;            // BC5/BC7
    std::string metallicRoughness; // BC7 (G=Roughness, B=Metallic)
    std::string ao;                // BC4/BC7
    std::string emissive;          // BC1/BC3/BC7 (sRGB)
};

inline void from_json(const nlohmann::json &j, MaterialTextureSlots &s) {
    if (j.contains("baseColor"))
        s.baseColor = j["baseColor"].get<std::string>();
    if (j.contains("normal"))
        s.normal = j["normal"].get<std::string>();
    if (j.contains("metallicRoughness"))
        s.metallicRoughness = j["metallicRoughness"].get<std::string>();
    if (j.contains("ao"))
        s.ao = j["ao"].get<std::string>();
    if (j.contains("emissive"))
        s.emissive = j["emissive"].get<std::string>();
}

// ── 完整材质描述 ──
struct MaterialDesc {
    std::string shader;            // 着色器模型标识，如 "PBR/Standard"
    MaterialParams params;         // PBR 值类型参数
    MaterialTextureSlots textures; // 纹理槽引用（可选，空字符串表示无纹理）
    nlohmann::json extraParams;    // 自定义扩展参数，透传到着色器
};

inline void from_json(const nlohmann::json &j, MaterialDesc &m) {
    if (j.contains("shader"))
        m.shader = j["shader"].get<std::string>();
    if (j.contains("params"))
        m.params = j["params"].get<MaterialParams>();
    if (j.contains("textures"))
        m.textures = j["textures"].get<MaterialTextureSlots>();
    if (j.contains("extra"))
        m.extraParams = j["extra"];
}

} // namespace DX12Engine::Resource
