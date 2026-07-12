#pragma once

#include "Asset/Definitions/Material/MaterialDesc.h"
#include "BackgroundExecutor.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/Material/MaterialResource.h"
#include <string>

namespace DX12Engine::Async {

// ========================================================================
// 解析结果 — 包含 MaterialData 和原始纹理路径
// ========================================================================
struct ParsedMaterial {
    Resource::MaterialData data;
    // 原始纹理路径（用于注册前解析为 SRV 索引）
    std::string baseColorPath;
    std::string normalPath;
    std::string metallicRoughnessPath;
    std::string aoPath;
    std::string emissivePath;
};

// ========================================================================
// ParseMaterialFile — 解析 .mat JSON 文件，返回 ParsedMaterial
// 纯 CPU 函数，线程安全，可在任意线程调用。
// ========================================================================
inline bool ParseMaterialFile(const std::string &filePath, ParsedMaterial &out,
                              TypeHash defaultRendererHash = TYPE_HASH("OpaquePBR")) {
    std::ifstream file(filePath);
    if (!file.is_open())
        return false;
    nlohmann::json root;
    try {
        file >> root;
    } catch (...) {
        return false;
    }

    // 解析 params
    if (root.contains("params")) {
        auto &p = root["params"];
        if (p.contains("baseColor")) {
            auto &c = p["baseColor"];
            float bc[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            for (int i = 0; i < 4 && i < (int)c.size(); ++i)
                bc[i] = c[i].get<float>();
            memcpy(&out.data.baseColor, bc, sizeof(float) * 4);
        }
        if (p.contains("metallic"))
            out.data.metallic = p["metallic"].get<float>();
        if (p.contains("roughness"))
            out.data.roughness = p["roughness"].get<float>();
        if (p.contains("ao"))
            out.data.ambient = p["ao"].get<float>();
        if (p.contains("alphaCutoff"))
            out.data.alphaCutoff = p["alphaCutoff"].get<float>();
    }

    // 解析纹理路径
    if (root.contains("textures")) {
        auto &t = root["textures"];
        if (t.contains("baseColor"))
            out.baseColorPath = t["baseColor"].get<std::string>();
        if (t.contains("normal"))
            out.normalPath = t["normal"].get<std::string>();
        if (t.contains("metallicRoughness"))
            out.metallicRoughnessPath = t["metallicRoughness"].get<std::string>();
        if (t.contains("ao"))
            out.aoPath = t["ao"].get<std::string>();
        if (t.contains("emissive"))
            out.emissivePath = t["emissive"].get<std::string>();
    }

    // 填充 metadata
    std::string shader = root.contains("shader") ? root["shader"].get<std::string>() : "";
    out.data.materialId = TYPE_HASH(shader.empty() ? filePath : shader);
    out.data.name = shader.empty() ? filePath : shader;
    out.data.rendererTypeHash = defaultRendererHash;

    return true;
}

// ========================================================================
// MaterialLoadTask — 从 .mat JSON 文件异步加载材质
//
// 数据流：
//   cpuWork (后台线程): 读取文件 → 解析 JSON → 填充 MaterialData
//   onComplete (主线程): 注册到 MaterialManager
// 无需 gpuWork（纯 CPU 任务，onComplete 通过 DeferToMainThread 延后到主线程）
// ========================================================================

struct MaterialLoadResult {
    Resource::MaterialHandle handle;
};

class MaterialLoadTask {
public:
    static LoadTask Create(const std::string &filePath, Resource::MaterialManager *matMgr,
                           std::shared_ptr<MaterialLoadResult> outResult = nullptr,
                           TypeHash defaultRendererHash = TYPE_HASH("OpaquePBR")) {
        LoadTask task;
        task.name = "MatLoad:" + filePath;

        auto parsed = std::make_shared<ParsedMaterial>();
        auto result = outResult ? outResult : std::make_shared<MaterialLoadResult>();
        auto path = std::make_shared<std::string>(filePath);

        // cpuWork（后台线程）：只做文件读取 + JSON 解析（不调非线程安全的 Manager）
        task.cpuWork = [parsed, path, defaultRendererHash]() {
            if (!ParseMaterialFile(*path, *parsed, defaultRendererHash)) {
                OutputDebugStringA(("[MaterialLoadTask] FAILED to parse: " + *path + "\n").c_str());
            }
        };

        // onComplete（主线程，通过 BackgroundExecutor::DeferToMainThread 延后执行）
        // 注意：SceneConstructor 场景加载模式下，材质注册由 OnDependenciesLoaded 统一处理，
        // 此处不调 RegisterMaterial，只做数据解析。
        task.onComplete = [parsed, matMgr, result](bool success) {
            if (!success || !matMgr)
                return;

            result->handle = matMgr->RegisterMaterial(parsed->data);

            char buf[256];
            sprintf_s(buf, "[MaterialLoadTask] RegisterMaterial done: handle=%s\n",
                      result->handle.IsValid() ? "valid" : "INVALID");
            OutputDebugStringA(buf);
        };

        return task;
    }
};

} // namespace DX12Engine::Async
