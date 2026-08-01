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
// 所有 from_json / to_json 均为手动实现：缺失的键保持默认值，不抛异常。
// to_json 只序列化非默认值字段，保持 JSON 简洁。
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

inline void to_json(nlohmann::json &j, const SceneMetadata &m) {
    j = nlohmann::json::object();
    if (!m.name.empty())
        j["name"] = m.name;
    if (!m.description.empty())
        j["description"] = m.description;
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

inline void to_json(nlohmann::json &j, const SceneDependencies &d) {
    j = nlohmann::json::object();
    if (!d.meshes.empty())
        j["meshes"] = d.meshes;
    if (!d.textures.empty())
        j["textures"] = d.textures;
    if (!d.terrains.empty())
        j["terrains"] = d.terrains;
}

// ========================================================================
// 组件描述
// ========================================================================

struct TransformDesc {
    std::vector<float> position = {0, 0, 0};
    std::vector<float> rotation = {0, 0, 0, 1}; // 四元数 [x, y, z, w]
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

inline void to_json(nlohmann::json &j, const TransformDesc &t) {
    j = nlohmann::json::object();
    j["position"] = t.position;
    j["rotation"] = t.rotation;
    j["scale"] = t.scale;
}

struct MeshDesc {
    std::string geometry;
    std::vector<std::string> materials; // [subMeshIndex] = material key, 向后兼容单材质
    bool receivesShadow = true;
};

inline void from_json(const nlohmann::json &j, MeshDesc &m) {
    if (j.contains("geometry"))
        m.geometry = j["geometry"].get<std::string>();
    if (j.contains("materials") && j["materials"].is_array()) {
        m.materials = j["materials"].get<std::vector<std::string>>();
    } else if (j.contains("material")) {
        // 向后兼容：旧格式 "material": "mat_body" → ["mat_body"]
        m.materials = {j["material"].get<std::string>()};
    }
    if (j.contains("receivesShadow"))
        m.receivesShadow = j["receivesShadow"].get<bool>();
}

inline void to_json(nlohmann::json &j, const MeshDesc &m) {
    j = nlohmann::json::object();
    j["geometry"] = m.geometry;
    if (!m.materials.empty())
        j["materials"] = m.materials;
    if (!m.receivesShadow)
        j["receivesShadow"] = false;
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

inline void to_json(nlohmann::json &j, const TerrainDesc &t) {
    j = nlohmann::json::object();
    j["heightmap"] = t.heightmap;
    if (t.heightScale != 20.0f)
        j["heightScale"] = t.heightScale;
    if (!t.albedo.empty())
        j["albedo"] = t.albedo;
    if (!t.normal.empty())
        j["normal"] = t.normal;
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

inline void to_json(nlohmann::json &j, const BillboardDesc &b) {
    j = nlohmann::json::object();
    j["texture"] = b.texture;
    if (b.width != 2.0f)
        j["width"] = b.width;
    if (b.height != 4.0f)
        j["height"] = b.height;
    if (b.mode != "axisY")
        j["mode"] = b.mode;
    if (!b.material.empty())
        j["material"] = b.material;
}

struct LightDesc {
    std::string type;
    std::vector<float> color = {1, 1, 1, 1};
    float intensity = 1.0f;
    std::optional<float> range;
    std::optional<float> falloffStart;
    std::optional<float> falloffEnd;
    std::optional<float> spotPower;
    float spotAngle = 0.0f;
    float castsShadow = 1.0f;
    std::optional<float> shadowBias;
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
    if (j.contains("falloffStart"))
        l.falloffStart = j["falloffStart"].get<float>();
    if (j.contains("falloffEnd"))
        l.falloffEnd = j["falloffEnd"].get<float>();
    if (j.contains("spotPower"))
        l.spotPower = j["spotPower"].get<float>();
    if (j.contains("spotAngle"))
        l.spotAngle = j["spotAngle"].get<float>();
    if (j.contains("castsShadow"))
        l.castsShadow = j["castsShadow"].get<float>();
    if (j.contains("shadowBias"))
        l.shadowBias = j["shadowBias"].get<float>();
}

inline void to_json(nlohmann::json &j, const LightDesc &l) {
    j = nlohmann::json::object();
    j["type"] = l.type;
    j["color"] = l.color;
    if (l.intensity != 1.0f)
        j["intensity"] = l.intensity;
    if (l.range.has_value())
        j["range"] = *l.range;
    if (l.falloffStart.has_value())
        j["falloffStart"] = *l.falloffStart;
    if (l.falloffEnd.has_value())
        j["falloffEnd"] = *l.falloffEnd;
    if (l.spotPower.has_value())
        j["spotPower"] = *l.spotPower;
    if (l.spotAngle != 0.0f)
        j["spotAngle"] = l.spotAngle;
    if (l.castsShadow != 1.0f)
        j["castsShadow"] = l.castsShadow;
    if (l.shadowBias.has_value())
        j["shadowBias"] = *l.shadowBias;
}

struct CameraDesc {
    float fov = 60.0f;
    float orthoSize = 10.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    std::string projection = "perspective"; // "perspective" | "orthographic"
    bool isMain = false;
};

inline void from_json(const nlohmann::json &j, CameraDesc &c) {
    if (j.contains("fov"))
        c.fov = j["fov"].get<float>();
    if (j.contains("orthoSize"))
        c.orthoSize = j["orthoSize"].get<float>();
    if (j.contains("nearPlane"))
        c.nearPlane = j["nearPlane"].get<float>();
    if (j.contains("farPlane"))
        c.farPlane = j["farPlane"].get<float>();
    if (j.contains("projection"))
        c.projection = j["projection"].get<std::string>();
    if (j.contains("isMain"))
        c.isMain = j["isMain"].get<bool>();
}

inline void to_json(nlohmann::json &j, const CameraDesc &c) {
    j = nlohmann::json::object();
    j["fov"] = c.fov;
    j["orthoSize"] = c.orthoSize;
    j["nearPlane"] = c.nearPlane;
    j["farPlane"] = c.farPlane;
    j["projection"] = c.projection;
    if (c.isMain)
        j["isMain"] = true;
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

inline void to_json(nlohmann::json &j, const SkinnedDesc &s) {
    j = nlohmann::json::object();
    if (!s.skeleton.empty())
        j["skeleton"] = s.skeleton;
    if (!s.animationClip.empty())
        j["animationClip"] = s.animationClip;
}

struct ReflectionProbeDesc {
    float range = 50.0f;
    uint32_t resolution = 256;
    uint8_t updatePriority = 1;
};

inline void from_json(const nlohmann::json &j, ReflectionProbeDesc &r) {
    if (j.contains("range"))
        r.range = j["range"].get<float>();
    if (j.contains("resolution"))
        r.resolution = j["resolution"].get<uint32_t>();
    if (j.contains("updatePriority"))
        r.updatePriority = j["updatePriority"].get<uint8_t>();
}

inline void to_json(nlohmann::json &j, const ReflectionProbeDesc &r) {
    j = nlohmann::json::object();
    if (r.range != 50.0f)
        j["range"] = r.range;
    if (r.resolution != 256)
        j["resolution"] = r.resolution;
    if (r.updatePriority != 1)
        j["updatePriority"] = r.updatePriority;
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

inline void to_json(nlohmann::json &j, const WaterDesc &w) {
    j = nlohmann::json::object();
    if (w.amplitude != 0.5f)
        j["amplitude"] = w.amplitude;
    if (w.frequency != 1.0f)
        j["frequency"] = w.frequency;
    if (w.speed != 0.5f)
        j["speed"] = w.speed;
    if (w.direction != 0.0f)
        j["direction"] = w.direction;
}

// ========================================================================
// 程序化几何体描述（用于天空盒等自生成形状）
// ========================================================================

struct ProceduralGeometryDesc {
    std::string type;       // "cube" | "sphere"；空 = 未使用
    float radius = 1.0f;    // 球体半径
    uint32_t rings = 16;    // 球体环数
    uint32_t segments = 16; // 球体分段数
};

inline void from_json(const nlohmann::json &j, ProceduralGeometryDesc &p) {
    p.type = j.value("type", std::string());
    p.radius = j.value("radius", 1.0f);
    p.rings = j.value("rings", 16u);
    p.segments = j.value("segments", 16u);
}

inline void to_json(nlohmann::json &j, const ProceduralGeometryDesc &p) {
    j = nlohmann::json::object();
    j["type"] = p.type;
    if (p.type == "sphere") {
        j["radius"] = p.radius;
        j["rings"] = p.rings;
        j["segments"] = p.segments;
    }
}

// ========================================================================
// 天空盒描述
// ========================================================================

struct SkyboxDesc {
    std::string texture;                     // dependencies.textures 中的 key
    std::string geometry;                    // dependencies.meshes 中的 key（可选；为空时走 procedural）
    ProceduralGeometryDesc procedural;       // 程序化几何体参数（geometry 为空时使用）
    std::vector<float> color = {1, 1, 1, 1}; // 兜底颜色（纹理加载失败时使用）
};

inline void from_json(const nlohmann::json &j, SkyboxDesc &s) {
    if (j.contains("texture"))
        s.texture = j["texture"].get<std::string>();
    if (j.contains("geometry")) {
        const auto &g = j["geometry"];
        if (g.is_string()) {
            s.geometry = g.get<std::string>();
        } else if (g.is_object()) {
            s.procedural = g.get<ProceduralGeometryDesc>();
        }
    }
    if (j.contains("color"))
        s.color = j["color"].get<std::vector<float>>();
}

inline void to_json(nlohmann::json &j, const SkyboxDesc &s) {
    j = nlohmann::json::object();
    j["texture"] = s.texture;
    if (!s.geometry.empty()) {
        j["geometry"] = s.geometry;
    } else if (!s.procedural.type.empty()) {
        j["geometry"] = s.procedural;
    }
    j["color"] = s.color;
}

// 关系描述
struct RelationshipDesc {
    std::string kind;       // "parent" | "socket" | "group" | "follow"
    uint64_t targetId = 0;  // 目标实体的 persistentId
    std::string socketName; // 仅 kind=="socket" 时使用
};

inline void from_json(const nlohmann::json &j, RelationshipDesc &r) {
    if (j.contains("kind"))
        r.kind = j["kind"].get<std::string>();
    if (j.contains("targetId"))
        r.targetId = j["targetId"].get<uint64_t>();
    if (j.contains("socketName"))
        r.socketName = j["socketName"].get<std::string>();
}

inline void to_json(nlohmann::json &j, const RelationshipDesc &r) {
    j = nlohmann::json::object();
    j["kind"] = r.kind;
    j["targetId"] = r.targetId;
    if (!r.socketName.empty())
        j["socketName"] = r.socketName;
}

// ========================================================================
// 实体描述（嵌套结构，加载时递归展平）
// ========================================================================

struct EntityDesc {
    std::string name;
    std::string persistentId; // fnv1a 64-bit 十六进制 hash 字符串

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

    std::vector<RelationshipDesc> relationships; // 实体关系列表
    std::vector<EntityDesc> children;            // 已废弃，向后兼容
};

inline void to_json(nlohmann::json &j, const EntityDesc &e) {
    j = nlohmann::json::object();
    if (!e.name.empty())
        j["name"] = e.name;
    if (!e.persistentId.empty())
        j["persistentId"] = e.persistentId;

    nlohmann::json comps = nlohmann::json::object();
    if (e.transform)
        comps["transform"] = *e.transform;
    if (e.mesh)
        comps["mesh"] = *e.mesh;
    if (e.terrain)
        comps["terrain"] = *e.terrain;
    if (e.billboard)
        comps["billboard"] = *e.billboard;
    if (e.light)
        comps["light"] = *e.light;
    if (e.camera)
        comps["camera"] = *e.camera;
    if (e.skinned)
        comps["skinned"] = *e.skinned;
    if (e.reflectionProbe)
        comps["reflection_probe"] = *e.reflectionProbe;
    if (e.water)
        comps["water"] = *e.water;
    if (e.opaque)
        comps["opaque"] = nullptr;
    if (e.transparent)
        comps["transparent"] = nullptr;
    j["components"] = std::move(comps);

    if (!e.relationships.empty())
        j["relationships"] = e.relationships;
    if (!e.children.empty())
        j["children"] = e.children;
}

// 环境描述
struct EnvironmentDesc {
    std::vector<float> ambientLight = {0.25f, 0.25f, 0.35f, 1.0f};
};

inline void from_json(const nlohmann::json &j, EnvironmentDesc &e) {
    if (j.contains("ambientLight"))
        e.ambientLight = j["ambientLight"].get<std::vector<float>>();
}

inline void to_json(nlohmann::json &j, const EnvironmentDesc &e) {
    j = nlohmann::json::object();
    j["ambientLight"] = e.ambientLight;
}

// ========================================================================
// SceneEnvironment — 管理器全局场景数据
//
// 语义：所有不进入 ECS Registry 的场景级全局参数（管理器特有数据）。
//       与 entities（ECS 实体数据）互斥——有 TransformComponent 的归 entities，
//       无实体身份的全局参数归 sceneEnvironment。
//
// 典型数据：环境光、天空盒。未来扩展：雾、后处理设置、时段等。
// ========================================================================
struct SceneEnvironment {
    EnvironmentDesc ambient;          // 环境光全局参数
    std::optional<SkyboxDesc> skybox; // 天空盒配置

    bool HasSkybox() const { return skybox.has_value() && !skybox->texture.empty(); }
    bool HasAmbient() const { return !ambient.ambientLight.empty(); }
};

inline void from_json(const nlohmann::json &j, SceneEnvironment &e) {
    if (j.contains("ambient"))
        e.ambient = j["ambient"].get<EnvironmentDesc>();
    if (j.contains("skybox"))
        e.skybox = j["skybox"].get<SkyboxDesc>();
}

inline void to_json(nlohmann::json &j, const SceneEnvironment &e) {
    j = nlohmann::json::object();
    j["ambient"] = e.ambient;
    if (e.skybox)
        j["skybox"] = *e.skybox;
}

// 顶层场景描述
struct SceneDescription {
    uint32_t version = 1;
    SceneMetadata metadata;
    std::string baseURL;               // 所有依赖路径的前缀（相对项目根），如 "Content/City"
    SceneEnvironment sceneEnvironment; // 管理器全局数据（环境光、天空盒等，不进入 ECS Registry）
    SceneDependencies dependencies;
    std::unordered_map<std::string, Resource::MaterialDesc> materials;
    std::vector<EntityDesc> entities;

    // 场景内容 hash（FNV-1a 64-bit 十六进制字符串，可选）
    // 加载时可为空，由工具链或编辑器在后续流程中填充
    std::string hash;
};

inline void to_json(nlohmann::json &j, const SceneDescription &d) {
    j = nlohmann::json::object();
    j["version"] = d.version;
    if (!d.metadata.name.empty() || !d.metadata.description.empty())
        j["metadata"] = d.metadata;
    if (!d.baseURL.empty())
        j["baseURL"] = d.baseURL;
    j["sceneEnvironment"] = d.sceneEnvironment;
    if (!d.dependencies.meshes.empty() || !d.dependencies.textures.empty() || !d.dependencies.terrains.empty())
        j["dependencies"] = d.dependencies;
    if (!d.materials.empty())
        j["materials"] = d.materials;
    j["entities"] = d.entities;
    if (!d.hash.empty())
        j["hash"] = d.hash;
}

} // namespace DX12Engine::Resource