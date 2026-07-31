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
//
// 所有 from_json / to_json 均为手动实现。
// to_json 只序列化非默认值字段，保持 JSON 简洁。
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

    // 材质内容 hash（FNV-1a 64-bit 十六进制字符串，可选）
    // 由工具链在导出时填充，加载时可为空
    std::string hash;
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
    if (j.contains("hash") && j["hash"].is_string())
        m.hash = j["hash"].get<std::string>();
}

inline void to_json(nlohmann::json &j, const MaterialParams &p) {
    j = nlohmann::json::object();
    j["baseColor"] = std::vector<float>{p.baseColor[0], p.baseColor[1], p.baseColor[2], p.baseColor[3]};
    if (p.metallic != 0.0f)
        j["metallic"] = p.metallic;
    if (p.roughness != 0.5f)
        j["roughness"] = p.roughness;
    if (p.ao != 1.0f)
        j["ao"] = p.ao;
    j["emissive"] = std::vector<float>{p.emissive[0], p.emissive[1], p.emissive[2], p.emissive[3]};
    if (p.alphaCutoff != 0.0f)
        j["alphaCutoff"] = p.alphaCutoff;
}

inline void to_json(nlohmann::json &j, const MaterialTextureSlots &s) {
    j = nlohmann::json::object();
    if (!s.baseColor.empty())
        j["baseColor"] = s.baseColor;
    if (!s.normal.empty())
        j["normal"] = s.normal;
    if (!s.metallicRoughness.empty())
        j["metallicRoughness"] = s.metallicRoughness;
    if (!s.ao.empty())
        j["ao"] = s.ao;
    if (!s.emissive.empty())
        j["emissive"] = s.emissive;
}

inline void to_json(nlohmann::json &j, const MaterialDesc &m) {
    j = nlohmann::json::object();
    j["shader"] = m.shader;
    j["params"] = m.params;
    if (!m.textures.baseColor.empty() || !m.textures.normal.empty() || !m.textures.metallicRoughness.empty() ||
        !m.textures.ao.empty() || !m.textures.emissive.empty())
        j["textures"] = m.textures;
    if (!m.hash.empty())
        j["hash"] = m.hash;
}

} // namespace DX12Engine::Resource
