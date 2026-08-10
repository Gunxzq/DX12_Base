#pragma once
#include "Asset/Definitions/AssetType.h"
#include "Background/BackgroundExecutor.h"
#include "Core/SharedDataStore/SharedDataStore.h"
#include "Renderer/Material/MaterialHandle.h"
#include "Resource/Character/CharacterData.h"
#include "Resource/Core/DescriptorHeapCollection.h" // HeapTag 枚举（m_heapTag/SetHeapTag 依赖完整定义）
#include "Resource/Struct/ClipHandle.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/SkeletonHandle.h"
#include "Resource/Struct/TextureHandle.h"
#include "Scheduler/Task.h"
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Engine {

namespace Renderer {
class D3D12DeviceContext;
}

namespace Resource {

class GeometryResourceManager;  // 前向声明
class MaterialManager;          // 前向声明
class TextureManager;           // 前向声明
class SkeletonManager;          // 前向声明
class AnimationManager;         // 前向声明
class DescriptorHeapCollection; // 前向声明

// ========================================================================
// AssetManager — 通用异步资产加载管理器
//
// 职责：
//   接收加载请求 → 分派到对应 Loader → BackgroundExecutor 异步执行
//   → GPU 上传完成 → 回调通知。不关心加载后的数据怎么用。
//
// 数据流：
//   Load(path, type, priority)
//     → 创建 LoadTask（CPU 加载 + 创建 GPU 资源 + 录制命令）
//     → BackgroundExecutor::SubmitGraph()
//     → GpuWorkItem → BackgroundExecutor::Tick() → Submit COPY+DIRECT
//     → onComplete 回调（主线程）
//
// 典型用法：
//   AssetManager::GetInstance().LoadMesh("statue.ddsmesh", [](MeshResult result) {
//       manager->RegisterGeometry(result);
//   });
// ========================================================================

struct AssetResult {
    AssetType type;
    std::string path;
    bool success = false;

    // 纹理/单资源 GPU 句柄
    Resource::GpuResourceHandle gpuHandle;

    // 网格加载结果（VB + IB）
    Resource::GpuResourceHandle meshVBHandle;
    Resource::GpuResourceHandle meshIBHandle;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = 0;

    // 注册后的 GeometryHandle（Mesh 类型专用）
    GeometryHandle geometryHandle;

    // 纹理结果（Texture 类型专用）
    TextureHandle textureHandle;

    // 材质结果（Material 类型专用）
    MaterialHandle materialHandle;

    // 骨骼结果（Skeleton 类型专用）
    Resource::SkeletonHandle skeletonHandle;

    // 动画剪辑结果（Animation 类型专用）
    Resource::ClipHandle clipHandle;

    // 角色结果（Character 类型专用：复合资产，含 Handle 引用集）
    Resource::CharacterData characterData;
};

using AssetCallback = std::function<void(const AssetResult &result)>;

// ========================================================================
// Loader 注册表（2026-08-02 定案，见 Docs/architecture/assets/AssetLoaderImprovement.md）
//   以文件后缀分发加载请求，替代 switch(type)。类型由 Loader 声明（InferType 派生）。
//   后缀匹配规则：extension() 小写 → 查表；若为 ".json" 再对 stem() 的 extension() 查表
//   （自动覆盖 .scene.json 等双后缀）。".anim" 不注册——见 LoadAnimation 专用入口。
// ========================================================================

// 创建并提交 LoadTask 的闭包（捕获 managers/deviceContext），返回请求 id
using LoaderFunc = std::function<uint32_t(const std::string &path, AssetCallback onComplete, uint8_t priority)>;

struct LoaderEntry {
    LoaderFunc func; // 创建并提交 LoadTask 的闭包
    AssetType type;  // 该后缀产出的类型（供 InferType + AssetResult.type）
};

// 单个加载请求
struct AssetRequest {
    uint32_t id;
    std::string path;
    uint8_t priority = 1; // 0=最高, 255=最低
    AssetCallback onComplete;
};

// 批量加载请求（场景文件等需要多资产就绪的场景）
struct AssetBatch {
    uint32_t id;
    std::vector<AssetRequest> requests;
    std::function<void()> onAllComplete;
    std::atomic<uint32_t> completedCount{0};
    std::atomic<uint32_t> failedCount{0};

    // 子任务计数：资产加载完毕后可能追加后处理（如 SceneConstructor 的材质 buffer 上传）
    // OnSubTaskBegin 增加计数，OnSubTaskEnd 递减；
    // 只要 activeSubTasks > 0，CheckAllComplete 就不会触发 onAllComplete。
    // 当所有条件和满足时（初始资产 + 子任务均完成），onAllComplete 触发一次并通过 m_completed 守卫防重。
    std::atomic<uint32_t> activeSubTasks{0};
    std::atomic<bool> m_completed{false};

    AssetBatch(uint32_t id, std::vector<AssetRequest> reqs, std::function<void()> callback)
        : id(id), requests(std::move(reqs)), onAllComplete(std::move(callback)) {}

    /// 追加一个子任务（外部调用，如 SceneConstructor 在材质 buffer 上传前调用）
    void OnSubTaskBegin() { activeSubTasks.fetch_add(1, std::memory_order_release); }

    /// 子任务完成（递减计数）
    void OnSubTaskEnd() {
        if (activeSubTasks.fetch_sub(1, std::memory_order_acq_rel) == 1)
            CheckAllComplete();
    }

    /// 单个 asset 完成计数（由 LoadBatch 内部回调调用）
    void OnAssetComplete(bool success) {
        if (success)
            completedCount.fetch_add(1, std::memory_order_release);
        else
            failedCount.fetch_add(1, std::memory_order_release);

        CheckAllComplete();
    }

private:
    void CheckAllComplete() {
        uint32_t done = completedCount.load(std::memory_order_acquire) + failedCount.load(std::memory_order_acquire);
        uint32_t sub = activeSubTasks.load(std::memory_order_acquire);
        if (done < (uint32_t)requests.size()) {
            char buf[128];
            sprintf_s(buf, "[AssetBatch %u] asset %u/%zu completed (sub=%u)\n", id, done, requests.size(), sub);
            OutputDebugStringA(buf);
            return;
        }
        if (sub != 0) {
            char buf[128];
            sprintf_s(buf, "[AssetBatch %u] all assets done, waiting sub-tasks=%u\n", id, sub);
            OutputDebugStringA(buf);
            return;
        }

        bool expected = false;
        if (m_completed.compare_exchange_strong(expected, true)) {
            OutputDebugStringA(("[AssetBatch " + std::to_string(id) + "] FIRING onAllComplete\n").c_str());
            if (onAllComplete)
                onAllComplete();
        }
    }
};

using AssetBatchPtr = std::shared_ptr<AssetBatch>;

class AssetManager {
public:
    static AssetManager &GetInstance();

    void Initialize(Renderer::D3D12DeviceContext *deviceContext, Async::BackgroundExecutor *executor,
                    Resource::GeometryResourceManager *geoMgr = nullptr, Resource::MaterialManager *matMgr = nullptr,
                    Resource::TextureManager *texMgr = nullptr, Resource::SkeletonManager *skeletonMgr = nullptr,
                    Resource::AnimationManager *animMgr = nullptr,
                    Resource::DescriptorHeapCollection *descHeaps = nullptr);
    void Shutdown();

    // 单资产加载（只传路径，类型由后缀推断——见 InferType）
    uint32_t Load(const std::string &path, AssetCallback onComplete, uint8_t priority = 1);

    // 注册加载器：ext 统一小写带点（".dxmesh" / ".bone" / ".scene"）
    // 后注册的覆盖先注册的（允许模块覆盖默认加载器）
    void RegisterLoader(std::string ext, AssetType type, LoaderFunc func);

    // ── 各资产类型 Loader 自注册（定义在 Loaders/*.cpp，2026-08-02 按资产类型拆文件） ──
    // Initialize() 末尾调用完成默认注册；编辑器/Game 端也可在使用 AssetManager 之前自行调用
    // （后调用覆盖默认加载器，注册时机只需早于 Load()）。新增资产类型 = 新增 Loaders/xxx.cpp
    // + 此处一行声明，不改 AssetManager 主体（开放封闭）。
    void RegisterMeshLoader();
    void RegisterTextureLoader();
    void RegisterMaterialLoader();
    void RegisterSkeletonLoader();
    void RegisterCharacterLoader();

    /// 从路径推断资产类型：extension() 小写查表；若为 ".json" 再对 stem() 的 extension() 查表
    /// （自动覆盖 .scene.json 等双后缀）。未命中返回 AssetType::None。
    AssetType InferType(const std::string &path) const;

    // 动画剪辑专用加载：需要骨架 BoneNames 做通道匹配（.anim 通道按骨骼名对齐）
    // .anim 不注册注册表——anim 与 bone 以骨骼名紧密耦合，必须携带骨架上下文（见 AssetLoaderImprovement.md §3.4）
    // 调用方须先加载 .bone（SkeletonHandle 就绪）后传入其 BoneNames
    uint32_t LoadAnimation(const std::string &path, const std::vector<std::string> &boneNames, AssetCallback onComplete,
                           uint8_t priority = 1);

    // 批量加载（全部完成后回调，返回 batch 指针用于追加子任务；类型由各路径后缀推断）
    AssetBatchPtr LoadBatch(const std::vector<std::string> &paths, AssetCallback perAssetComplete,
                            std::function<void()> onAllComplete);

    // 每帧调用（驱动 BackgroundExecutor::Tick）
    void Update();

    /// 获取缓存引用（供 SceneConstructor 查询已加载结果）
    const std::unordered_map<std::string, AssetResult> &GetCache() const { return m_cache; }

    /// 设置纹理等资源的描述符堆域（编辑器多堆模式须传 HeapTag::EditorViewport，Game 用 Default）
    /// 场景构建器（SceneConstructor）在分发加载任务前调用
    void SetHeapTag(Resource::HeapTag tag) { m_heapTag = tag; }
    Resource::HeapTag GetHeapTag() const { return m_heapTag; }

private:
    uint32_t m_nextRequestId = 1;
    uint32_t m_nextBatchId = 1;

    Renderer::D3D12DeviceContext *m_deviceContext = nullptr;
    Async::BackgroundExecutor *m_executor = nullptr;
    Resource::GeometryResourceManager *m_geoMgr = nullptr;
    Resource::MaterialManager *m_matMgr = nullptr;
    Resource::TextureManager *m_texMgr = nullptr;
    Resource::SkeletonManager *m_skeletonMgr = nullptr;
    Resource::AnimationManager *m_animMgr = nullptr;
    Resource::DescriptorHeapCollection *m_descHeaps = nullptr;
    Resource::HeapTag m_heapTag = Resource::HeapTag::Default; // 资源加载目标堆域（SetHeapTag 设置）

    // 进行中的批量加载
    std::vector<AssetBatchPtr> m_activeBatches;

    // 加载器注册表：后缀（小写带点）→ Loader（2026-08-02 定案，替代 switch(type)）
    std::unordered_map<std::string, LoaderEntry> m_loaders;

    // ========================================================================
    // 缓存：路径 → 加载结果
    // 同路径多次请求只加载一次，后续直接返回缓存结果
    // ========================================================================
    std::unordered_map<std::string, AssetResult> m_cache;

    /// 检查缓存，命中则直接回调并返回 true
    bool TryCache(const std::string &path, AssetCallback &callback);

    // 按后缀查 Loader（小写 extension() 查表；".json" 再对 stem() 的 extension() 查表）
    // 未命中返回 nullptr
    const LoaderEntry *FindLoader(const std::string &path) const;

    // 各类型 Loader 实现（Initialize 末尾注册到 m_loaders）
    uint32_t LoadMeshImpl(const std::string &path, AssetCallback onComplete, uint8_t priority);
    uint32_t LoadTextureImpl(const std::string &path, AssetCallback onComplete, uint8_t priority);
    uint32_t LoadMaterialImpl(const std::string &path, AssetCallback onComplete, uint8_t priority);
    uint32_t LoadSkeletonImpl(const std::string &path, AssetCallback onComplete, uint8_t priority);
    uint32_t LoadCharacterImpl(const std::string &path, AssetCallback onComplete, uint8_t priority);

    // 程序化几何体加载器（procedural:// URI scheme，不走文件后缀注册表）
    uint32_t LoadProceduralGeometry(const std::string &uri, AssetCallback onComplete, uint8_t priority);
};

} // namespace Resource
} // namespace DX12Engine
