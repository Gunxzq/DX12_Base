#pragma once

#include <DirectXMath.h>
#include <string>
#include <unordered_map>

// ========================================================================
// EditorStateFile — 编辑器状态持久化
//
// 职责：
//   将编辑器运行时状态（相机位置、节点层级关系等）独立于场景文件
//   持久化到 .editor/state.json 中，使场景文件保持"干净的世界数据"。
//
// 存储位置：<ProjectRoot>/.editor/state.json
//
// 当前状态：
//   - L1: 相机位置/朝向（按场景路径索引）
//   - 未来：节点父子结构映射、面板显隐、选择状态等
// ========================================================================

class EditorStateFile {
public:
    EditorStateFile() = default;
    ~EditorStateFile() { Shutdown(); }

    EditorStateFile(const EditorStateFile &) = delete;
    EditorStateFile &operator=(const EditorStateFile &) = delete;

    /// 初始化：指定项目根目录，加载已存在的状态文件
    void Initialize(const std::string &projectRoot);

    /// 关闭：写入磁盘
    void Shutdown();

    /// 立即保存到磁盘
    void Save();

    // ========================================================================
    // 相机状态
    // ========================================================================

    struct CameraState {
        DirectX::XMFLOAT3 position = {0, 0, 0};
        float pitch = 0; // 弧度，绕 Right 轴
        float yaw = 0;   // 弧度，绕世界 Y 轴
    };

    /// 获取某场景的相机状态（返回 false 表示无记录，使用默认值）
    bool LoadCameraState(const std::string &scenePath, CameraState &outState) const;

    /// 设置某场景的相机状态
    void SetCameraState(const std::string &scenePath, const CameraState &state);

    // ========================================================================
    // 最近场景
    // ========================================================================

    void SetLastScene(const std::string &scenePath) { m_lastScene = scenePath; }
    const std::string &GetLastScene() const { return m_lastScene; }

    // ========================================================================
    // 节点层级映射（编辑时用层级结构，运行时展平）
    // ========================================================================

    /// 设置实体的父节点（entityId → parentId）
    void SetParent(const std::string &entityId, const std::string &parentId);

    /// 获取实体的父节点（返回空字符串表示无父节点——根节点）
    std::string GetParent(const std::string &entityId) const;

    /// 获取某场景的完整父子关系映射
    const std::unordered_map<std::string, std::string> &GetParentMap(const std::string &scenePath) const;

    /// 设置某场景的完整父子关系映射
    void SetParentMap(const std::string &scenePath,
                      std::unordered_map<std::string, std::string> &&parentMap);

    // ========================================================================
    // 工具方法
    // ========================================================================

    /// 从相机 Forward 向量提取 Pitch/Yaw（弧度）
    static void ExtractPitchYaw(const DirectX::XMFLOAT3 &forward, float &outPitch, float &outYaw);

private:
    // ========================================================================
    // 内部数据结构（对应 JSON 布局）
    // ========================================================================

    struct PerSceneState {
        CameraState camera;
        std::unordered_map<std::string, std::string> parentMap; // entityId → parentId
    };

    // ========================================================================
    // 内部方法
    // ========================================================================

    void LoadFromDisk();
    void SaveToDisk() const;

    std::string m_filePath;
    std::unordered_map<std::string, PerSceneState> m_scenes;
    std::string m_lastScene;
    bool m_initialized = false;
};