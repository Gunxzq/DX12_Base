#pragma once
#include "Background/BackgroundExecutor.h"
#include "Core/SharedDataStore/SharedDataStore.h"
#include "Renderer/Material/MaterialHandle.h"
#include "Resource/Struct/GeometryHandle.h"
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

enum class AssetType : uint8_t { Mesh, Texture, Material, Terrain, Scene };

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
};

using AssetCallback = std::function<void(const AssetResult &result)>;

// 单个加载请求
struct AssetRequest {
    uint32_t id;
    std::string path;
    AssetType type;
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
                    Resource::TextureManager *texMgr = nullptr,
                    Resource::DescriptorHeapCollection *descHeaps = nullptr);
    void Shutdown();

    // 单资产加载
    uint32_t Load(const std::string &path, AssetType type, AssetCallback onComplete, uint8_t priority = 1);

    // 批量加载（全部完成后回调，返回 batch 指针用于追加子任务）
    AssetBatchPtr LoadBatch(const std::vector<std::pair<std::string, AssetType>> &assets,
                            AssetCallback perAssetComplete, std::function<void()> onAllComplete);

    // 每帧调用（驱动 BackgroundExecutor::Tick）
    void Update();

    /// 获取缓存引用（供 SceneConstructor 查询已加载结果）
    const std::unordered_map<std::string, AssetResult> &GetCache() const { return m_cache; }

private:
    uint32_t m_nextRequestId = 1;
    uint32_t m_nextBatchId = 1;

    Renderer::D3D12DeviceContext *m_deviceContext = nullptr;
    Async::BackgroundExecutor *m_executor = nullptr;
    Resource::GeometryResourceManager *m_geoMgr = nullptr;
    Resource::MaterialManager *m_matMgr = nullptr;
    Resource::TextureManager *m_texMgr = nullptr;
    Resource::DescriptorHeapCollection *m_descHeaps = nullptr;

    // 进行中的批量加载
    std::vector<AssetBatchPtr> m_activeBatches;

    // 加载器注册表（类型 → 创建 LoadTask 的函数）
    // TODO: 后续扩展为可注册的 Loader 接口

    // ========================================================================
    // 缓存：路径 → 加载结果
    // 同路径多次请求只加载一次，后续直接返回缓存结果
    // ========================================================================
    std::unordered_map<std::string, AssetResult> m_cache;

    /// 检查缓存，命中则直接回调并返回 true
    bool TryCache(const std::string &path, AssetCallback &callback);
};

} // namespace Resource
} // namespace DX12Engine
