#include "EditorStateFile.h"
#include "Common/Common.h"
#include "Logger/Logger.h"
#include <DirectXMath.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace DirectX;
using namespace DX12Engine;

// ========================================================================
// 初始化 / 关闭
// ========================================================================

void EditorStateFile::Initialize(const std::string &projectRoot) {
    if (m_initialized)
        return;

    // 状态文件路径：<ProjectRoot>/.editor/state.json
    std::filesystem::path dir = std::filesystem::path(projectRoot) / ".editor";
    m_filePath = (dir / "state.json").string();

    // 确保目录存在
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        // 目录创建失败，降级为纯内存模式（不持久化）
        Logger::Logger::GetInstance()->Warn("[EditorStateFile] Cannot create directory '{}': {}", dir.string(), ec.message());
        m_initialized = true;
        return;
    }

    LoadFromDisk();
    m_initialized = true;
    Logger::Logger::GetInstance()->Info("[EditorStateFile] Initialized: {}", m_filePath);
}

void EditorStateFile::Shutdown() {
    if (!m_initialized)
        return;
    SaveToDisk();
    m_scenes.clear();
    m_lastScene.clear();
    m_initialized = false;
    Logger::Logger::GetInstance()->Info("[EditorStateFile] Shutdown");
}

// ========================================================================
// 保存 / 加载
// ========================================================================

void EditorStateFile::Save() {
    if (!m_initialized)
        return;
    SaveToDisk();
}

void EditorStateFile::LoadFromDisk() {
    std::ifstream ifs(m_filePath);
    if (!ifs.is_open()) {
        // 文件不存在是正常情况（首次使用）
        return;
    }

    nlohmann::json root;
    try {
        ifs >> root;
    } catch (const nlohmann::json::parse_error &) {
        Logger::Logger::GetInstance()->Warn("[EditorStateFile] Parse error in '{}', using defaults", m_filePath);
        return;
    }

    // lastScene
    if (root.contains("lastScene") && root["lastScene"].is_string())
        m_lastScene = root["lastScene"].get<std::string>();

    // scenes
    if (root.contains("scenes") && root["scenes"].is_object()) {
        for (auto &[sceneKey, sceneJson] : root["scenes"].items()) {
            PerSceneState pss;

            // camera
            if (sceneJson.contains("camera") && sceneJson["camera"].is_object()) {
                auto &cj = sceneJson["camera"];
                if (cj.contains("position") && cj["position"].is_array() && cj["position"].size() >= 3) {
                    pss.camera.position.x = cj["position"][0].get<float>();
                    pss.camera.position.y = cj["position"][1].get<float>();
                    pss.camera.position.z = cj["position"][2].get<float>();
                }
                if (cj.contains("pitch"))
                    pss.camera.pitch = cj["pitch"].get<float>();
                if (cj.contains("yaw"))
                    pss.camera.yaw = cj["yaw"].get<float>();
            }

            // hierarchy (parentMap)
            if (sceneJson.contains("hierarchy") && sceneJson["hierarchy"].is_object()) {
                auto &hj = sceneJson["hierarchy"];
                if (hj.contains("parentMap") && hj["parentMap"].is_object()) {
                    for (auto &[entityId, parentId] : hj["parentMap"].items()) {
                        if (parentId.is_string())
                            pss.parentMap[entityId] = parentId.get<std::string>();
                    }
                }
            }

            m_scenes[sceneKey] = std::move(pss);
        }
    }

    Logger::Logger::GetInstance()->Info("[EditorStateFile] Loaded state for {} scenes", m_scenes.size());
}

void EditorStateFile::SaveToDisk() const {
    if (m_filePath.empty())
        return;

    nlohmann::json root;

    // lastScene
    if (!m_lastScene.empty())
        root["lastScene"] = m_lastScene;

    // scenes
    nlohmann::json scenesJson;
    for (auto &[sceneKey, pss] : m_scenes) {
        nlohmann::json sceneJson;

        // camera
        nlohmann::json camJson;
        camJson["position"] = {pss.camera.position.x, pss.camera.position.y, pss.camera.position.z};
        camJson["pitch"] = pss.camera.pitch;
        camJson["yaw"] = pss.camera.yaw;
        sceneJson["camera"] = std::move(camJson);

        // hierarchy
        if (!pss.parentMap.empty()) {
            nlohmann::json parentJson;
            for (auto &[entityId, parentId] : pss.parentMap)
                parentJson[entityId] = parentId;
            nlohmann::json hierJson;
            hierJson["parentMap"] = std::move(parentJson);
            sceneJson["hierarchy"] = std::move(hierJson);
        }

        scenesJson[sceneKey] = std::move(sceneJson);
    }
    root["scenes"] = std::move(scenesJson);

    // 写入文件（原子替换：先写临时文件，再 rename）
    std::string tmpPath = m_filePath + ".tmp";
    {
        std::ofstream ofs(tmpPath);
        if (!ofs.is_open()) {
            Logger::Logger::GetInstance()->Warn("[EditorStateFile] Cannot write to '{}'", tmpPath);
            return;
        }
        ofs << root.dump(2);
        ofs.close();
    }

    std::error_code ec;
    std::filesystem::rename(tmpPath, m_filePath, ec);
    if (ec) {
        Logger::Logger::GetInstance()->Warn("[EditorStateFile] Failed to rename '{}' -> '{}': {}", tmpPath, m_filePath, ec.message());
    }
}

// ========================================================================
// 相机状态
// ========================================================================

void EditorStateFile::ExtractPitchYaw(const XMFLOAT3 &forward, float &outPitch, float &outYaw) {
    XMVECTOR f = XMLoadFloat3(&forward);
    f = XMVector3Normalize(f);
    float fx = XMVectorGetX(f);
    float fy = XMVectorGetY(f);
    float fz = XMVectorGetZ(f);

    // Pitch: 从水平面上下角度，Forward.y = -sin(pitch)
    // Yaw: 绕 Y 轴旋转角度，Forward.xz 方向
    outPitch = asinf(std::clamp(-fy, -1.0f, 1.0f));
    outYaw = atan2f(fx, fz);
}

bool EditorStateFile::LoadCameraState(const std::string &scenePath, CameraState &outState) const {
    auto it = m_scenes.find(scenePath);
    if (it == m_scenes.end())
        return false;
    outState = it->second.camera;
    return true;
}

void EditorStateFile::SetCameraState(const std::string &scenePath, const CameraState &state) {
    m_scenes[scenePath].camera = state;
}

// ========================================================================
// 节点层级映射
// ========================================================================

void EditorStateFile::SetParent(const std::string &entityId, const std::string &parentId) {
    // 需要在场景路径下操作，暂时不实现单次 setter
    // 请使用 SetParentMap 批量设置
}

std::string EditorStateFile::GetParent(const std::string &entityId) const {
    // 需要在场景路径下操作，暂时不实现单次 getter
    // 请使用 GetParentMap 查询
    return {};
}

const std::unordered_map<std::string, std::string> &
EditorStateFile::GetParentMap(const std::string &scenePath) const {
    static std::unordered_map<std::string, std::string> s_empty;
    auto it = m_scenes.find(scenePath);
    if (it == m_scenes.end())
        return s_empty;
    return it->second.parentMap;
}

void EditorStateFile::SetParentMap(const std::string &scenePath,
                                   std::unordered_map<std::string, std::string> &&parentMap) {
    m_scenes[scenePath].parentMap = std::move(parentMap);
}