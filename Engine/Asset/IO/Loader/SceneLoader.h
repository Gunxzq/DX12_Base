#pragma once
#include "SceneDescription.h"
#include <filesystem>
#include <nlohmann/json.hpp>

namespace DX12Engine::Resource {

// ========================================================================
// SceneLoader — 场景文件加载器
//
// 开发版：读取原始 JSON 场景文件 → SceneDescription
// 发布版：接口不变，从打包数据构造 nlohmann::json → SceneDescription
// ========================================================================

class SceneLoader {
public:
    /**
     * @brief 从 JSON 文件加载场景描述
     */
    static SceneDescription LoadFromFile(const std::filesystem::path &path);
    /// 二进制 SOA 场景加载（.scene.bin——SCNB 格式：magic/version/count/meshCount + mesh 名表 + SOA 字段数组，
    /// 体积 ~9% of JSON，memcpy 直接解析字段数组）
    static SceneDescription LoadFromFileBinary(const std::filesystem::path &path);

    /**
     * @brief 从内存 JSON 加载场景描述（发布版走此路径）
     */
    static SceneDescription LoadFromJSON(const nlohmann::json &root);

    /**
     * @brief 将 SceneDescription 序列化为 ordered JSON（保持键顺序与 schema 一致）
     */
    static nlohmann::ordered_json SaveToJSON(const SceneDescription &desc);

    /**
     * @brief 将 SceneDescription 写入 JSON 文件
     */
    static bool SaveToFile(const SceneDescription &desc, const std::filesystem::path &path);

private:
    static EntityDesc ParseEntity(const nlohmann::json &j);
    static TransformDesc ParseTransform(const nlohmann::json &j);
    static MeshDesc ParseMesh(const nlohmann::json &j);
    static TerrainDesc ParseTerrain(const nlohmann::json &j);
    static BillboardDesc ParseBillboard(const nlohmann::json &j);
    static LightDesc ParseLight(const nlohmann::json &j);
    static CameraDesc ParseCamera(const nlohmann::json &j);
    static SkinnedDesc ParseSkinned(const nlohmann::json &j);
    static ReflectionProbeDesc ParseReflectionProbe(const nlohmann::json &j);
    static WaterDesc ParseWater(const nlohmann::json &j);
};

} // namespace DX12Engine::Resource
