#pragma once
#include "Asset/Definitions/Material/MaterialDesc.h"
#include <DirectXMath.h>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Engine::Resource {

// ========================================================================
// 场景描述结构体（对应 Schemas/scene.schema.json）
//
// 所有 from_json 均为手动实现：缺失的键保持默认值，不抛异常。
// ========================================================================

struct SceneMetadata {
    std::string name;
    std::string description;
};

inline void from_json(const nlohmann::json &j, SceneMetadata &m) {
    if (j.contains("name"))
        j.at("name").get_to(m.name);
    if (j.contains("description"))
        j.at("description").get_to(m.description);
}

struct SceneDependencies {
    std::unordered_map<std::string, std::string> meshes;
    std::unordered_map<std::string, std::string> textures;
    std::unordered_map<std::string, std::string> terrains;
};

inline void from_json(const nlohmann::json &j, SceneDependencies &d) {
    if (j.contains("meshes"))
        j.at("meshes").get_to(d.meshes);
    if (j.contains("textures"))
        j.at("textures").get_to(d.textures);
    if (j.contains("terrains"))
        j.at("terrains").get_to(d.terrains);
}

// ========================================================================
// 组件描述
// ========================================================================

struct TransformDesc {
    std::vector<float> position = {0, 0, 0};
    std::vector<float> rotation = {0, 0, 0, 1};
    std::vector<float> scale = {1, 1, 1};
};

inline void from_json(const nlohmann::json &j, TransformDesc &t) {
    if (j.contains("position"))
        j.at("position").get_to(t.position);
    if (j.contains("rotation"))
        j.at("rotation").get_to(t.rotation);
    if (j.contains("scale"))
        j.at("scale").get_to(t.scale);
}

struct MeshDesc {
    std::string geometry;
    std::string material;
    bool receivesShadow = true;
};

inline void from_json(const nlohmann::json &j, MeshDesc &m) {
    if (j.contains("geometry"))
        m.geometry = j["geometry"].get<std::string>();
    if (j.contains("material"))
        m.material = j["material"].get<std::string>();
    if (j.contains("receivesShadow"))
        m.receivesShadow = j["receivesShadow"].get<bool>();
}

struct TerrainDesc {
    std::string heightmap;
    float heightScale = 20.0f;
    std::string albedo;
    std::string normal;
};

inline void from_json(const nlohmann::json &j, TerrainDesc &t) {
    if (j.contains("heightmap"))
        t.heightmap = j["heightmap"].get<std::string>();
    if (j.contains("heightScale"))
        t.heightScale = j["heightScale"].get<float>();
    if (j.contains("albedo"))
        t.albedo = j["albedo"].get<std::string>();
    if (j.contains("normal"))
        t.normal = j["normal"].get<std::string>();
}

struct BillboardDesc {
    std::string texture;
    float width = 2.0f;
    float height = 4.0f;
    std::string mode = "axisY";
    std::string material;
};

inline void from_json(const nlohmann::json &j, BillboardDesc &b) {
    if (j.contains("texture"))
        b.texture = j["texture"].get<std::string>();
    if (j.contains("width"))
        b.width = j["width"].get<float>();
    if (j.contains("height"))
        b.height = j["height"].get<float>();
    if (j.contains("mode"))
        b.mode = j["mode"].get<std::string>();
    if (j.contains("material"))
        b.material = j["material"].get<std::string>();
}

struct LightDesc {
    std::string type;
    std::vector<float> color = {1, 1, 1, 1};
    float intensity = 1.0f;
    float range = 0.0f;
    float spotAngle = 0.0f;
    float castsShadow = 1.0f;
};

inline void from_json(const nlohmann::json &j, LightDesc &l) {
    if (j.contains("type"))
        l.type = j["type"].get<std::string>();
    if (j.contains("color"))
        l.color = j["color"].get<std::vector<float>>();
    if (j.contains("intensity"))
        l.intensity = j["intensity"].get<float>();
    if (j.contains("range"))
        l.range = j["range"].get<float>();
    if (j.contains("spotAngle"))
        l.spotAngle = j["spotAngle"].get<float>();
    if (j.contains("castsShadow"))
        l.castsShadow = j["castsShadow"].get<float>();
}

struct CameraDesc {
    float fov = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    bool isMain = false;
};

inline void from_json(const nlohmann::json &j, CameraDesc &c) {
    if (j.contains("fov"))
        c.fov = j["fov"].get<float>();
    if (j.contains("nearPlane"))
        c.nearPlane = j["nearPlane"].get<float>();
    if (j.contains("farPlane"))
        c.farPlane = j["farPlane"].get<float>();
    if (j.contains("isMain"))
        c.isMain = j["isMain"].get<bool>();
}

struct SkinnedDesc {
    std::string skeleton;
    std::string animationClip;
};

inline void from_json(const nlohmann::json &j, SkinnedDesc &s) {
    if (j.contains("skeleton"))
        s.skeleton = j["skeleton"].get<std::string>();
    if (j.contains("animationClip"))
        s.animationClip = j["animationClip"].get<std::string>();
}

struct ReflectionProbeDesc {
    float range = 50.0f;
    uint32_t resolution = 256;
};

inline void from_json(const nlohmann::json &j, ReflectionProbeDesc &r) {
    if (j.contains("range"))
        r.range = j["range"].get<float>();
    if (j.contains("resolution"))
        r.resolution = j["resolution"].get<uint32_t>();
}

// ========================================================================
// 水描述
// ========================================================================

struct WaterDesc {
    float amplitude = 0.5f;
    float frequency = 1.0f;
    float speed = 0.5f;
    float direction = 0.0f;
};

inline void from_json(const nlohmann::json &j, WaterDesc &w) {
    if (j.contains("amplitude"))
        w.amplitude = j["amplitude"].get<float>();
    if (j.contains("frequency"))
        w.frequency = j["frequency"].get<float>();
    if (j.contains("speed"))
        w.speed = j["speed"].get<float>();
    if (j.contains("direction"))
        w.direction = j["direction"].get<float>();
}

// ========================================================================
// 天空盒描述
// ========================================================================

struct SkyboxDesc {
    std::string texture;                     // dependencies.textures 中的 key
    std::string geometry;                    // dependencies.meshes 中的 key（可选，默认单位盒）
    std::vector<float> color = {1, 1, 1, 1}; // 兜底颜色（纹理加载失败时使用）
};

inline void from_json(const nlohmann::json &j, SkyboxDesc &s) {
    if (j.contains("texture"))
        s.texture = j["texture"].get<std::string>();
    if (j.contains("geometry"))
        s.geometry = j["geometry"].get<std::string>();
    if (j.contains("color"))
        s.color = j["color"].get<std::vector<float>>();
}

// ========================================================================
// 实体描述（嵌套结构，加载时递归展平）
// ========================================================================

struct EntityDesc {
    std::string name;

    std::optional<TransformDesc> transform;
    std::optional<MeshDesc> mesh;
    std::optional<TerrainDesc> terrain;
    std::optional<BillboardDesc> billboard;
    std::optional<LightDesc> light;
    std::optional<CameraDesc> camera;
    std::optional<SkinnedDesc> skinned;
    std::optional<ReflectionProbeDesc> reflectionProbe;
    std::optional<WaterDesc> water;

    bool opaque = false;
    bool transparent = false;

    std::vector<EntityDesc> children;
};

// 环境描述
struct EnvironmentDesc {
    std::vector<float> ambientLight = {0.25f, 0.25f, 0.35f, 1.0f};
};

inline void from_json(const nlohmann::json &j, EnvironmentDesc &e) {
    if (j.contains("ambientLight"))
        e.ambientLight = j["ambientLight"].get<std::vector<float>>();
}

// 顶层场景描述
struct SceneDescription {
    uint32_t version = 1;
    SceneMetadata metadata;
    std::string baseURL; // 所有依赖路径的前缀（相对项目根），如 "Content/City"
    EnvironmentDesc environment;
    std::optional<SkyboxDesc> skybox; // 场景天空盒（非 ECS 组件，全局属性）
    SceneDependencies dependencies;
    std::unordered_map<std::string, Resource::MaterialDesc> materials;
    std::vector<EntityDesc> entities;
};

} // namespace DX12Engine::Resource
