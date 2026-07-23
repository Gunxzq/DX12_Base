#pragma once

#include "ECS/SceneTagComponent.h"
#include "Scene/SceneManager.h"
#include "Resource/Core/GpuHandlePool.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Renderer/Material/MaterialHandle.h"
#include "ThirdParty/imgui/imgui.h"
#include <DirectXMath.h>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Engine::Scene {
struct SceneConstructData;
} // namespace DX12Engine::Scene

// ========================================================================
// EditorSceneManager — 编辑器场景管理器
//
// 组合包装 SceneManager，提供编辑器特有的场景管理能力：
//   - 实体描述缓存（EntityDesc 用于导出）
//   - 注册 EditorSceneConstructSystem（响应 GeneratorTaskCompleteEvent）
//   - 场景文件保存/导出（JSON 序列化）
//
// 注意：场景加载是 AssetManager 的编排职责，EditorSceneManager 不负责加载。
//       它只提供实体管理的接口（CreateEntity），
//       由 SceneConstructSystem 在异步加载完成后调用。
// ========================================================================

class EditorSceneManager {
public:
    EditorSceneManager() = default;
    ~EditorSceneManager() = default;

    EditorSceneManager(const EditorSceneManager&) = delete;
    EditorSceneManager& operator=(const EditorSceneManager&) = delete;

    // ====================================================================
    // 初始化/销毁
    // ====================================================================

    /// 初始化（绑定被包装的 SceneManager，注册 GPU 资源释放回调）
    /// @param sceneMgr 由 Bootstrap 创建的 SceneManager 实例
    /// @param context  GameContext 指针
    void Initialize(DX12Engine::Scene::SceneManager* sceneMgr,
                    DX12Engine::Boot::GameContext* context);

    /// 销毁，清理所有实体和子场景
    void Shutdown();

    // ====================================================================
    // 包装方法（委托给 SceneManager）
    // ====================================================================

    /// 获取被包装的 SceneManager 指针（供 OutlinerPanel/EditorViewportInput 等使用）
    DX12Engine::Scene::SceneManager* GetSceneManager() const { return m_sceneMgr; }

    /// 创建空实体
    uint64_t CreateEntity() { return m_sceneMgr ? m_sceneMgr->CreateEntity() : UINT64_MAX; }

    /// 获取内部 Registry 指针（供 Builder 系统获取 ECS 上下文）
    DX12Engine::ECS::Registry* GetRegistry() const { return m_sceneMgr ? m_sceneMgr->GetRegistry() : nullptr; }

    // ====================================================================
    // 场景构造系统注册
    // ====================================================================

    /// 注册 EditorSceneConstructSystem（响应 GeneratorTaskCompleteEvent）
    void RegisterSceneConstructSystem();

    // ====================================================================
    // 场景生命周期管理
    // ====================================================================

    /// 切换场景：释放旧场景资源 → 更新状态 → 加载新场景
    /// @param newSceneName 新场景名称
    /// @param sceneFilePath 场景文件路径（用于追踪活跃场景）
    /// @return true 表示需要继续加载场景，false 表示场景已存在 Tab 中，无需重复加载
    bool SwitchScene(const std::string& newSceneName, const std::filesystem::path& sceneFilePath);

    /// 获取当前活跃场景文件路径
    const std::filesystem::path& GetActiveScenePath() const { return m_activeScenePath; }

    /// 检查当前是否有活跃场景
    bool HasActiveScene() const { return !m_activeScenePath.empty(); }

    /// 保存当前活跃场景的快照到磁盘
    void SaveCurrentSnapshotToDisk();

    // ====================================================================
    // 场景文件管理（编辑器特有）
    // ====================================================================

    /// 新建空白场景
    void NewScene(const std::string& name);

    /// 保存场景（序列化为 SceneDescription → JSON）
    void SaveScene();
    void SaveSceneAs(const std::filesystem::path& filePath);

    /// 获取当前场景文件路径
    const std::filesystem::path& GetSceneFilePath() const { return m_sceneFilePath; }

    /// 检查场景是否有未保存的修改
    bool IsDirty() const { return m_dirty; }
    void MarkDirty() { m_dirty = true; }
    void ClearDirty() { m_dirty = false; }

    // ====================================================================
    // 多 Tab 管理
    // ====================================================================

    /// 单个场景 Tab 信息
    struct SceneTab {
        std::string name;               // 场景显示名称
        std::filesystem::path filePath; // 场景文件路径
        bool dirty = false;             // 是否有未保存修改
        uint64_t sceneId = 0;           // 场景标记 ID（用于 SceneTagComponent）
    };

    /// 单场景快照（聚合场景环境数据 + 实体缓存 + 编辑器 UX 状态）
    ///
    /// 设计原则：
    ///   - 可序列化字段可写入磁盘缓存，跨会话恢复
    ///   - 运行时字段（GPU 句柄）仅存于内存，跨会话需重新加载
    ///   - Tab 切换时整体切换此结构体，不逐个字段手动同步
    struct SceneSnapshot {
        // ===== 可序列化字段（可写入磁盘缓存） =====
        DX12Engine::Resource::SkyboxDesc skybox;
        DX12Engine::Resource::EnvironmentDesc environment;
        std::vector<DX12Engine::Resource::EntityDesc> entityDescs;

        // 相机状态
        DirectX::XMFLOAT3 cameraPosition = {0, 0, 0};
        DirectX::XMFLOAT3 cameraForward = {0, 0, 1};

        // 层级展开状态（entityId → parentId）
        std::unordered_map<std::string, std::string> parentMap;

        // 选中实体 persistentId
        uint64_t selectedEntity = 0;

        // ===== 运行时字段（仅内存，不可序列化） =====
        std::unordered_map<std::string, DX12Engine::Resource::GeometryHandle> geoMap;
        std::unordered_map<std::string, DX12Engine::Resource::MaterialHandle> matMap;
        DX12Engine::Resource::GpuResourceHandle skyboxTextureHandle;
        DX12Engine::Resource::GeometryHandle skyboxGeometryHandle;
        std::vector<uint64_t> entities;

        // ===== 序列化能力 =====
        /// 写入磁盘缓存（仅序列化可序列化字段）
        bool SaveTo(const std::filesystem::path& path) const;

        /// 从磁盘缓存读取（仅恢复可序列化字段）
        bool LoadFrom(const std::filesystem::path& path);

        // ===== 辅助方法 =====
        bool HasSkybox() const { return !skybox.texture.empty(); }
        bool HasEnvironment() const { return !environment.ambientLight.empty(); }
        bool HasCamera() const { return cameraPosition.x != 0 || cameraPosition.y != 0 || cameraPosition.z != 0; }
    };

    /// 获取所有已打开的 Tab 列表
    const std::vector<SceneTab>& GetOpenTabs() const { return m_openTabs; }

    /// 获取当前活跃 Tab 索引
    size_t GetActiveTabIndex() const { return m_activeTabIndex; }

    /// 获取当前活跃 Tab 的实体 Handle 列表（供 Outliner/Builder 使用）
    const std::vector<uint64_t>& GetActiveEntities() const;

    /// 获取当前活跃场景的标记 ID（用于 SceneTagComponent 过滤）
    uint64_t GetActiveSceneId() const;

    /// 绘制场景 Tab 栏（由 Editor 每帧调用，传入视口 SRV 用于在 Tab 内容区内渲染）
    /// @param viewportSRV 视口图像的 ImTextureID，在 BeginTabItem/EndTabItem 之间渲染
    /// @param outImageMin [out] 渲染图像的屏幕坐标左上角（供工具栏叠加定位）
    /// @param outImageMax [out] 渲染图像的屏幕坐标右下角
    void DrawTabBar(ImTextureID viewportSRV, ImVec2 *outImageMin = nullptr, ImVec2 *outImageMax = nullptr);

    /// 处理待处理的 Tab 切换请求（在每帧 ImGui 渲染完成后调用）
    void ProcessPendingTabSwitch();

    /// 设置场景加载回调（由 Editor 注册，Tab 切换时触发 SceneConstructor 加载）
    /// @param callback 参数为场景名称和文件路径
    void SetOnLoadSceneCallback(std::function<void(const std::string&, const std::filesystem::path&)> callback);

    /// 关闭指定索引的 Tab（保存提示 + 资源释放）
    void CloseTab(size_t index);

    // ====================================================================
    // EntityDesc 编辑（编辑器特有）
    // ====================================================================

    /// 获取可编辑的 EntityDesc 列表（供 Outliner 编辑）
    DX12Engine::Resource::EntityDesc* GetMutableEntityDesc(uint64_t entity);

    /// 更新实体描述（Outliner 编辑后调用）
    void UpdateEntityDesc(uint64_t entity, const DX12Engine::Resource::EntityDesc& newDesc);

    /// 导出当前场景为 SceneDescription（用于序列化）
    DX12Engine::Resource::SceneDescription ExportToDescription() const;

    /// 获取默认场景描述（标准天空盒 + 空世界）
    static DX12Engine::Resource::SceneDescription GetDefaultSceneDescription();

    /// 初始化相机配置（裁剪面、远平面等），不设相机位置
    /// 场景无缓存相机状态时，配合 ResetCameraToDefault 使用
    static void InitCameraConfig(DX12Engine::Boot::GameContext *context);

    /// 重置相机到默认位置（仅无缓存场景使用）
    void ResetCameraToDefault();

private:
    // ====================================================================
    // 内部方法
    // ====================================================================

    /// 场景构造完成后的处理（从 SceneConstructData 创建实体）
    void OnSceneConstructReady(const DX12Engine::Scene::SceneConstructData& sceneData);

    /// 应用指定 Tab 的完整状态到全局管理器（Clear + Rebuild）
    void ApplyTabState(size_t index);

    /// 从快照恢复相机状态（无缓存时使用默认位置）
    void RestoreSnapshotCamera(size_t index);

private:
    DX12Engine::Scene::SceneManager* m_sceneMgr = nullptr;
    DX12Engine::Boot::GameContext* m_context = nullptr;

    // 缓存根目录（Content/Cache/Editor/）
    std::string m_cacheRoot;

    // 当前活跃场景文件路径（用于场景切换追踪）
    std::filesystem::path m_activeScenePath;

    // 当前编辑的场景文件路径
    std::filesystem::path m_sceneFilePath;
    bool m_dirty = false;

    // 可编辑的 EntityDesc 列表（与 ECS 实体双向映射）
    std::unordered_map<uint64_t, DX12Engine::Resource::EntityDesc> m_entityDescs;

    // 场景切换序列号（每次 SwitchScene 递增，用于检测过期异步回调）
    uint64_t m_sceneSwitchId = 0;

    // 多 Tab 追踪
    std::vector<SceneTab> m_openTabs;
    size_t m_activeTabIndex = 0;

    // 每 Tab 的完整快照（索引与 m_openTabs 对齐）
    std::vector<SceneSnapshot> m_snapshots;

    // 待处理的 Tab 切换（延迟执行，避免在 ImGui 渲染中切换场景）
    size_t m_pendingSwitchTab = SIZE_MAX;

    // 场景标记 ID 生成器（每新建一个 Tab 递增）
    uint64_t m_nextSceneId = 1;

    // 场景加载回调（由 Editor 注册，Tab 切换时触发 SceneConstructor 加载）
    std::function<void(const std::string&, const std::filesystem::path&)> m_onLoadScene;

    // 异步加载标志（用于显示加载指示器）
    bool m_isLoading = false;

    bool m_initialized = false;
};