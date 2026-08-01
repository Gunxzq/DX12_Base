#include "SceneLoader.h"
#include "Common/Common.h"
#include "Resource/Utils/HashUtils.h"
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

    // 基本校验：必需字段（对应 schema 的 "required": ["version", "entities"]）
    if (!root.contains("version"))
        ErrorReporter::Fatal("SceneLoader: missing required field 'version'");
    if (!root.contains("entities") || !root["entities"].is_array())
        ErrorReporter::Fatal("SceneLoader: missing required field 'entities' (must be an array)");

    // version
    if (root.contains("version"))
        desc.version = root["version"].get<uint32_t>();

    // metadata
    if (root.contains("metadata"))
        desc.metadata = root["metadata"].get<SceneMetadata>();

    // baseURL（所有依赖路径的前缀，如 "Content/City"）
    if (root.contains("baseURL") && root["baseURL"].is_string())
        desc.baseURL = root["baseURL"].get<std::string>();

    // sceneEnvironment（管理器全局数据：环境光、天空盒等，不进入 ECS Registry）
    if (root.contains("sceneEnvironment"))
        desc.sceneEnvironment = root["sceneEnvironment"].get<SceneEnvironment>();

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

    // hash（场景内容 FNV-1a 校验，可选）
    if (root.contains("hash") && root["hash"].is_string())
        desc.hash = root["hash"].get<std::string>();

    return desc;
}

nlohmann::ordered_json SceneLoader::SaveToJSON(const SceneDescription &desc) {
    // 使用 ordered_json 保持键顺序与 schema 一致
    nlohmann::ordered_json j;

    // 按 schema 字段顺序写入（$schema 惯例第一，hash 最后）
    j["$schema"] = "https://raw.githubusercontent.com/Gunxzq/DX12_Base/Refactor/Schemas/scene.schema.json";
    j["version"] = desc.version;

    if (!desc.baseURL.empty())
        j["baseURL"] = desc.baseURL;

    if (!desc.metadata.name.empty() || !desc.metadata.description.empty()) {
        nlohmann::json meta = desc.metadata;
        j["metadata"] = std::move(meta);
    }

    {
        nlohmann::json sceneEnv = desc.sceneEnvironment;
        j["sceneEnvironment"] = std::move(sceneEnv);
    }

    if (!desc.dependencies.meshes.empty() || !desc.dependencies.textures.empty() ||
        !desc.dependencies.terrains.empty()) {
        nlohmann::json deps = desc.dependencies;
        j["dependencies"] = std::move(deps);
    }

    if (!desc.materials.empty()) {
        nlohmann::json mats = desc.materials;
        j["materials"] = std::move(mats);
    }

    {
        nlohmann::json ents = desc.entities;
        j["entities"] = std::move(ents);
    }

    if (!desc.hash.empty())
        j["hash"] = desc.hash;

    return j;
}

bool SceneLoader::SaveToFile(const SceneDescription &desc, const std::filesystem::path &path) {
    nlohmann::ordered_json j = SaveToJSON(desc);

    // 自动计算 hash（排除 $schema 和 hash 自身字段）
    if (!j.contains("hash") || j["hash"].empty() || j["hash"].get<std::string>().empty()) {
        // 临时移除 $schema 和 hash（如果有）用于 hash 计算
        bool hadSchema = j.contains("$schema");
        std::string schemaVal;
        if (hadSchema) {
            schemaVal = j["$schema"].get<std::string>();
            j.erase("$schema");
        }
        j.erase("hash");

        std::string jsonStr = j.dump();
        uint64_t hashVal = HashUtils::CalculateMemoryHash(jsonStr.data(), jsonStr.size());
        char hashBuf[17];
        snprintf(hashBuf, sizeof(hashBuf), "%016llx", hashVal);

        // 恢复 $schema（如果有），再添加 hash
        if (hadSchema)
            j["$schema"] = schemaVal;
        j["hash"] = std::string(hashBuf);
    }

    std::ofstream ofs(path);
    if (!ofs.is_open())
        return false;

    ofs << j.dump(4); // 4 空格缩进，可读性好
    return true;
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

    // 关系（实体层级，不在 components 中）
    if (j.contains("relationships") && j["relationships"].is_array()) {
        for (const auto &relJson : j["relationships"])
            entity.relationships.push_back(relJson.get<RelationshipDesc>());
    }

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
    if (j.contains("materials") && j["materials"].is_array()) {
        m.materials = j["materials"].get<std::vector<std::string>>();
    } else if (j.contains("material")) {
        // 向后兼容
        m.materials = {j["material"].get<std::string>()};
    }
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
