#include "SceneLoader.h"
#include "Asset/Definitions/Scene/DxSceneFormat.h" // DxScene 二进制格式（.scene——DXSCENE 魔数 + Header + SOA）
#include "Common/Common.h"
#include "Resource/Utils/HashUtils.h"
#include <cstring> // std::memcmp（DxScene 魔数检查）
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

SceneDescription SceneLoader::LoadFromFileBinary(const std::filesystem::path &path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open())
        ErrorReporter::Fatal("SceneLoader: cannot open %s", path.string().c_str());

    SceneDescription desc;
    DxSceneHeader header{};
    ifs.read(reinterpret_cast<char *>(&header), sizeof(DxSceneHeader));
    if (std::memcmp(header.magic, DX_SCENE_MAGIC, 8) != 0) // "DXSCENE\0"
        ErrorReporter::Fatal("SceneLoader: bad binary magic in %s", path.string().c_str());
    const uint32_t count = header.entityCount;
    const uint32_t meshCount = header.meshCount;
    const uint32_t version = header.version;

    // mesh 名表（u16 len + 字节）
    std::vector<std::string> meshNames(meshCount);
    for (uint32_t i = 0; i < meshCount; ++i) {
        uint16_t len = 0;
        ifs.read(reinterpret_cast<char *>(&len), 2);
        meshNames[i].resize(len);
        ifs.read(&meshNames[i][0], len);
    }

    // 材质表（版本 2——材质名 u16 len + 字节——实体材质引用索引）
    std::vector<std::string> materialNames(header.materialCount);
    for (uint32_t i = 0; i < header.materialCount; ++i) {
        uint16_t len = 0;
        ifs.read(reinterpret_cast<char *>(&len), 2);
        materialNames[i].resize(len);
        ifs.read(&materialNames[i][0], len);
    }

    // SOA 字段数组（memcpy 直接读取）
    std::vector<uint64_t> pid(count);
    std::vector<uint32_t> meshIdx(count);
    std::vector<uint32_t> materialIdx(count); // 材质引用（版本 2——索引材质表，UINT32_MAX = 无）
    std::vector<float> pos(count * 3), rot(count * 4), scl(count * 3), cull(count);
    ifs.read(reinterpret_cast<char *>(pid.data()), count * 8);
    ifs.read(reinterpret_cast<char *>(meshIdx.data()), count * 4);
    ifs.read(reinterpret_cast<char *>(materialIdx.data()), count * 4); // 材质引用（版本 2）
    ifs.read(reinterpret_cast<char *>(pos.data()), count * 12);
    ifs.read(reinterpret_cast<char *>(rot.data()), count * 16);
    ifs.read(reinterpret_cast<char *>(scl.data()), count * 12);
    ifs.read(reinterpret_cast<char *>(cull.data()), count * 4);

    // 水块数组（版本 2——邻接 Sea 合并的水四边形 → desc.waterBlocks）
    for (uint32_t i = 0; i < header.waterBlockCount; ++i) {
        DxSceneWaterBlock wb{};
        ifs.read(reinterpret_cast<char *>(&wb), sizeof(DxSceneWaterBlock));
        Resource::WaterBlockDesc wd;
        wd.min = {wb.minX, wb.minZ};
        wd.max = {wb.maxX, wb.maxZ};
        // world = [posX, posY, posZ, rotX, rotY, rotZ, rotW, scaleX, scaleY, scaleZ]
        // 旋转恒等（水面 Y=0 平面，二进制省略旋转字段，此处补单位四元数）
        wd.world = {wb.posX, wb.posY, wb.posZ, 0.0f, 0.0f, 0.0f, 1.0f, wb.scaleX, wb.scaleY, wb.scaleZ};
        wd.tiling = {wb.tilingX, wb.tilingZ};
        desc.waterBlocks.push_back(std::move(wd));
    }
    // 环境段（版本 2——ambient RGBA + entityMotionPolicy + skybox 纹理名——不丢 JSON 架构内容）
    if (header.flags & DxSceneFlag_HasEnvironment) {
        float ambient[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        ifs.read(reinterpret_cast<char *>(ambient), 4 * sizeof(float));
        desc.sceneEnvironment.ambient.ambientLight = {ambient[0], ambient[1], ambient[2], ambient[3]};
        uint16_t policyLen = 0;
        ifs.read(reinterpret_cast<char *>(&policyLen), 2);
        if (policyLen > 0) {
            std::string policy(policyLen, '\0');
            ifs.read(&policy[0], policyLen);
            desc.sceneEnvironment.entityMotionPolicy = policy;
        }
        if (header.flags & DxSceneFlag_HasSkybox) {
            uint16_t skyLen = 0;
            ifs.read(reinterpret_cast<char *>(&skyLen), 2);
            if (skyLen > 0) {
                std::string sky(skyLen, '\0');
                ifs.read(&sky[0], skyLen);
                // skybox 纹理名（引擎 skybox 程序化驱动——纹理由 TextureManager 按名解析）
            }
        }
    }

    // 构造 SceneDescription（entities：transform + mesh + 材质引用 + cullDistance，按索引对齐）
    desc.version = static_cast<int>(version);
    desc.entities.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Resource::EntityDesc e;
        char pidBuf[17];
        snprintf(pidBuf, sizeof(pidBuf), "%016llx", static_cast<unsigned long long>(pid[i]));
        e.persistentId = pidBuf;
        e.name = meshNames[meshIdx[i]] + "_" + std::to_string(i);
        Resource::TransformDesc td;
        td.position = {pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]};
        td.rotation = {rot[i * 4], rot[i * 4 + 1], rot[i * 4 + 2], rot[i * 4 + 3]};
        td.scale = {scl[i * 3], scl[i * 3 + 1], scl[i * 3 + 2]};
        td.cullDistance = cull[i];
        e.transform = std::move(td);
        Resource::MeshDesc md;
        md.geometry = meshNames[meshIdx[i]];
        if (materialIdx[i] != UINT32_MAX && materialIdx[i] < materialNames.size())
            md.materials = {materialNames[materialIdx[i]]}; // 材质引用（版本 2——单槽 vector）
        e.mesh = std::move(md);
        desc.entities.push_back(std::move(e));
    }
    return desc;
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

    // blockConfig（块划分配置，可选；缺失 = 推导模式——加载时按地图范围自动推导，见 §0c）
    if (root.contains("blockConfig") && root["blockConfig"].is_object())
        desc.blockConfig = root["blockConfig"].get<Resource::BlockConfigDesc>();

    // worldConfig（Octree 空间索引世界范围，可选；缺失 = 推导模式——加载时按实体 worldBounds
    // 推导 worldSize = max(span)*1.2 + 中心，见 OctreeCullingAndRaycaster.md §7.5）
    if (root.contains("worldConfig") && root["worldConfig"].is_object())
        desc.worldConfig = root["worldConfig"].get<Resource::WorldConfigDesc>();

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

    // waterBlocks（水块数组——邻接 Sea 合并的程序化水面四边形，旧格式）
    if (root.contains("waterBlocks") && root["waterBlocks"].is_array()) {
        for (const auto &wb : root["waterBlocks"]) {
            Resource::WaterBlockDesc wd;
            if (wb.contains("min") && wb["min"].is_array())
                wd.min = wb["min"].get<std::vector<float>>();
            if (wb.contains("max") && wb["max"].is_array())
                wd.max = wb["max"].get<std::vector<float>>();
            if (wb.contains("world") && wb["world"].is_array())
                wd.world = wb["world"].get<std::vector<float>>();
            if (wb.contains("tiling") && wb["tiling"].is_array())
                wd.tiling = wb["tiling"].get<std::vector<float>>();
            desc.waterBlocks.push_back(std::move(wd));
        }
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

    // blockConfig（块划分配置，可选；缺省不写，保存时由 ExportToDescription 固化）
    // 注意：ordered_json 直接赋值自定义类型需 to_json(ordered_json&,...)，而 to_json 写在
    // nlohmann::json 上——与 metadata/sceneEnvironment 同模式：先转 nlohmann::json 再 move
    if (desc.blockConfig) {
        nlohmann::json bc = *desc.blockConfig;
        j["blockConfig"] = std::move(bc);
    }

    // worldConfig（Octree 世界范围，可选；缺省不写，保存时由 ExportToDescription 固化）
    if (desc.worldConfig) {
        nlohmann::json wc = *desc.worldConfig;
        j["worldConfig"] = std::move(wc);
    }

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

    // waterBlocks（水块数组——旧格式，与二进制 .scene 同构）
    if (!desc.waterBlocks.empty()) {
        nlohmann::json wbs = nlohmann::json::array();
        for (const auto &wd : desc.waterBlocks) {
            nlohmann::json wb = nlohmann::json::object();
            wb["min"] = wd.min;
            wb["max"] = wd.max;
            wb["world"] = wd.world;
            wb["tiling"] = wd.tiling;
            wbs.push_back(std::move(wb));
        }
        j["waterBlocks"] = std::move(wbs);
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

    // persistentId（静态烘焙 precomputed 按此匹配；四端一致性：EntityDesc 字段/to_json/ParseEntity，
    // 见 .atomcode.md #23——此前遗漏导致 eDesc.persistentId 为空、precomputed 永不命中、static=0）
    if (j.contains("persistentId") && j["persistentId"].is_string())
        entity.persistentId = j["persistentId"].get<std::string>();

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
    // 剔除距离（四端一致性：TransformDesc 字段/to_json/ParseTransform/SceneConstructor，规则 #23——
    // 此前遗漏导致 eDesc.transform.cullDistance 恒 0、距离剔除永不触发、路灯不剔除）
    if (j.contains("cullDistance") && j["cullDistance"].is_number())
        t.cullDistance = j["cullDistance"].get<float>();
    return t;
}

MeshDesc SceneLoader::ParseMesh(const nlohmann::json &j) {
    MeshDesc m;
    if (j.contains("geometry"))
        m.geometry = j["geometry"].get<std::string>();
    if (j.contains("materials") && j["materials"].is_array()) {
        m.materials = j["materials"].get<std::vector<std::string>>();
    }
    if (j.contains("receivesShadow"))
        m.receivesShadow = j["receivesShadow"].get<bool>();
    if (j.contains("castsShadow"))
        m.castsShadow = j["castsShadow"].get<bool>();
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
