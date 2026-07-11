#include "SceneLoader.h"
#include "Common/Common.h"
#include <fstream>

namespace DX12Engine::Resource {

SceneDescription SceneLoader::LoadFromFile(const std::filesystem::path &path) {
    std::ifstream ifs(path);
    if (!ifs.is_open())
        ErrorReporter::Fatal("SceneLoader: cannot open %s", path.string().c_str());

    nlohmann::json root;
    try {
        ifs >> root;
    } catch (const nlohmann::json::parse_error &e) {
        ErrorReporter::Fatal("SceneLoader: JSON parse error in %s: %s", path.string().c_str(), e.what());
    }

    return LoadFromJSON(root);
}

SceneDescription SceneLoader::LoadFromJSON(const nlohmann::json &root) {
    SceneDescription desc;

    // version
    if (root.contains("version"))
        desc.version = root["version"].get<uint32_t>();

    // metadata
    if (root.contains("metadata"))
        desc.metadata = root["metadata"].get<SceneMetadata>();

    // baseURL（所有依赖路径的前缀，如 "Content/City"）
    if (root.contains("baseURL") && root["baseURL"].is_string())
        desc.baseURL = root["baseURL"].get<std::string>();

    // environment
    if (root.contains("environment"))
        desc.environment = root["environment"].get<EnvironmentDesc>();

    // skybox（顶层全局属性，非 ECS 实体）
    if (root.contains("skybox") && root["skybox"].is_object()) {
        desc.skybox = root["skybox"].get<SkyboxDesc>();
    }

    // dependencies（from_json 内置可选字段处理）
    if (root.contains("dependencies"))
        desc.dependencies = root["dependencies"].get<SceneDependencies>();

    // materials（内联材质定义，key → MaterialDesc）
    if (root.contains("materials") && root["materials"].is_object()) {
        for (auto &[key, val] : root["materials"].items())
            desc.materials[key] = val.get<Resource::MaterialDesc>();
    }

    // entities（递归解析）
    if (root.contains("entities") && root["entities"].is_array()) {
        for (const auto &entityJson : root["entities"])
            desc.entities.push_back(ParseEntity(entityJson));
    }

    return desc;
}

EntityDesc SceneLoader::ParseEntity(const nlohmann::json &j) {
    EntityDesc entity;

    if (j.contains("name") && j["name"].is_string())
        entity.name = j["name"].get<std::string>();

    if (!j.contains("components"))
        return entity;

    const auto &c = j["components"];

    if (c.contains("transform"))
        entity.transform = ParseTransform(c["transform"]);
    if (c.contains("mesh"))
        entity.mesh = ParseMesh(c["mesh"]);
    if (c.contains("terrain"))
        entity.terrain = ParseTerrain(c["terrain"]);
    if (c.contains("billboard"))
        entity.billboard = ParseBillboard(c["billboard"]);
    if (c.contains("light"))
        entity.light = ParseLight(c["light"]);
    if (c.contains("camera"))
        entity.camera = ParseCamera(c["camera"]);
    if (c.contains("skinned"))
        entity.skinned = ParseSkinned(c["skinned"]);
    if (c.contains("reflection_probe"))
        entity.reflectionProbe = ParseReflectionProbe(c["reflection_probe"]);
    if (c.contains("water"))
        entity.water = ParseWater(c["water"]);

    // 标记组件：存在即 true
    entity.opaque = c.contains("opaque");
    entity.transparent = c.contains("transparent");

    // 子实体
    if (j.contains("children") && j["children"].is_array()) {
        for (const auto &childJson : j["children"])
            entity.children.push_back(ParseEntity(childJson));
    }

    return entity;
}

TransformDesc SceneLoader::ParseTransform(const nlohmann::json &j) {
    TransformDesc t;
    if (j.contains("position"))
        t.position = j["position"].get<std::vector<float>>();
    if (j.contains("rotation"))
        t.rotation = j["rotation"].get<std::vector<float>>();
    if (j.contains("scale"))
        t.scale = j["scale"].get<std::vector<float>>();
    return t;
}

MeshDesc SceneLoader::ParseMesh(const nlohmann::json &j) {
    MeshDesc m;
    if (j.contains("geometry"))
        m.geometry = j["geometry"].get<std::string>();
    if (j.contains("material"))
        m.material = j["material"].get<std::string>();
    if (j.contains("receivesShadow"))
        m.receivesShadow = j["receivesShadow"].get<bool>();
    return m;
}

TerrainDesc SceneLoader::ParseTerrain(const nlohmann::json &j) {
    TerrainDesc t;
    if (j.contains("heightmap"))
        t.heightmap = j["heightmap"].get<std::string>();
    if (j.contains("heightScale"))
        t.heightScale = j["heightScale"].get<float>();
    if (j.contains("albedo"))
        t.albedo = j["albedo"].get<std::string>();
    if (j.contains("normal"))
        t.normal = j["normal"].get<std::string>();
    return t;
}

BillboardDesc SceneLoader::ParseBillboard(const nlohmann::json &j) {
    BillboardDesc b;
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
    return b;
}

LightDesc SceneLoader::ParseLight(const nlohmann::json &j) {
    LightDesc l;
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
    return l;
}

CameraDesc SceneLoader::ParseCamera(const nlohmann::json &j) {
    CameraDesc c;
    if (j.contains("fov"))
        c.fov = j["fov"].get<float>();
    if (j.contains("nearPlane"))
        c.nearPlane = j["nearPlane"].get<float>();
    if (j.contains("farPlane"))
        c.farPlane = j["farPlane"].get<float>();
    if (j.contains("isMain"))
        c.isMain = j["isMain"].get<bool>();
    return c;
}

SkinnedDesc SceneLoader::ParseSkinned(const nlohmann::json &j) {
    SkinnedDesc s;
    if (j.contains("skeleton"))
        s.skeleton = j["skeleton"].get<std::string>();
    if (j.contains("animationClip"))
        s.animationClip = j["animationClip"].get<std::string>();
    return s;
}

ReflectionProbeDesc SceneLoader::ParseReflectionProbe(const nlohmann::json &j) {
    ReflectionProbeDesc r;
    if (j.contains("range"))
        r.range = j["range"].get<float>();
    if (j.contains("resolution"))
        r.resolution = j["resolution"].get<uint32_t>();
    return r;
}

WaterDesc SceneLoader::ParseWater(const nlohmann::json &j) {
    WaterDesc w;
    if (j.contains("amplitude"))
        w.amplitude = j["amplitude"].get<float>();
    if (j.contains("frequency"))
        w.frequency = j["frequency"].get<float>();
    if (j.contains("speed"))
        w.speed = j["speed"].get<float>();
    if (j.contains("direction"))
        w.direction = j["direction"].get<float>();
    return w;
}

} // namespace DX12Engine::Resource
