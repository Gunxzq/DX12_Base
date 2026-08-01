#include "AssetManager.h"
#include "Background/AnimationLoadTask.h"
#include "Common/Common.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace DX12Engine::Resource {

AssetManager &AssetManager::GetInstance() {
    static AssetManager instance;
    return instance;
}

void AssetManager::Initialize(Renderer::D3D12DeviceContext *deviceContext, Async::BackgroundExecutor *executor,
                              Resource::GeometryResourceManager *geoMgr, Resource::MaterialManager *matMgr,
                              Resource::TextureManager *texMgr, Resource::SkeletonManager *skeletonMgr,
                              Resource::AnimationManager *animMgr, Resource::DescriptorHeapCollection *descHeaps) {
    m_deviceContext = deviceContext;
    m_executor = executor;
    m_geoMgr = geoMgr;
    m_matMgr = matMgr;
    m_texMgr = texMgr;
    m_skeletonMgr = skeletonMgr;
    m_animMgr = animMgr;
    m_descHeaps = descHeaps;

    // ── 注册默认加载器（2026-08-02 定案：后缀注册表替代 switch(type)） ──
    // 各资产类型 Loader 定义在 Loaders/*.cpp（按资产类型拆文件，参照属性卡 Register*Editor 模式）
    // 编辑器/Game 端也可在使用 AssetManager 之前自行调用 Register*Loader 覆盖默认注册
    RegisterMeshLoader();
    RegisterTextureLoader();
    RegisterMaterialLoader();
    RegisterSkeletonLoader();
    RegisterCharacterLoader();
    // .anim 不注册——anim 与 bone 以骨骼名紧密耦合，走 LoadAnimation 专用入口
}

void AssetManager::Shutdown() {
    m_activeBatches.clear();
    m_loaders.clear();
    m_deviceContext = nullptr;
    m_executor = nullptr;
}

// ========================================================================
// 注册表：RegisterLoader / InferType / FindLoader / Load
// ========================================================================

void AssetManager::RegisterLoader(std::string ext, AssetType type, LoaderFunc func) {
    // 统一小写带点（".dxmesh"），忽略大小写
    for (auto &c : ext)
        c = (char)tolower((unsigned char)c);
    if (ext.empty() || ext[0] != '.')
        ext = "." + ext;
    m_loaders[ext] = LoaderEntry{std::move(func), type};
}

AssetType AssetManager::InferType(const std::string &path) const {
    const LoaderEntry *entry = FindLoader(path);
    return entry ? entry->type : AssetType::None;
}

const LoaderEntry *AssetManager::FindLoader(const std::string &path) const {
    // 1. 取 extension() 小写查表
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    for (auto &c : ext)
        c = (char)tolower((unsigned char)c);
    auto it = m_loaders.find(ext);
    if (it != m_loaders.end())
        return &it->second;

    // 2. 双后缀：剥掉 .json 再查（kd-03.scene.json → stem=kd-03.scene → .scene）
    if (ext == ".json") {
        std::string ext2 = p.stem().extension().string();
        for (auto &c : ext2)
            c = (char)tolower((unsigned char)c);
        auto it2 = m_loaders.find(ext2);
        if (it2 != m_loaders.end())
            return &it2->second;
    }
    return nullptr;
}

uint32_t AssetManager::Load(const std::string &path, AssetCallback onComplete, uint8_t priority) {
    uint32_t id = m_nextRequestId++;

    // 检查缓存：同路径已加载过则直接回调解
    if (TryCache(path, onComplete)) {
        return id;
    }

    if (!m_deviceContext || !m_executor) {
        if (onComplete) {
            AssetResult result;
            result.type = InferType(path);
            result.path = path;
            result.success = false;
            onComplete(result);
        }
        return id;
    }

    // 按后缀查加载器（剥 .json 再试）；无匹配加载器则失败
    const LoaderEntry *entry = FindLoader(path);
    if (!entry) {
        if (onComplete) {
            AssetResult result;
            result.type = InferType(path);
            result.path = path;
            result.success = false;
            onComplete(result);
        }
        return id;
    }

    return entry->func(path, std::move(onComplete), priority);
}

// ========================================================================
// LoadAnimation（.anim 专用入口：anim 与 bone 以骨骼名紧密耦合，不注册注册表）
// ========================================================================

uint32_t AssetManager::LoadAnimation(const std::string &path, const std::vector<std::string> &boneNames,
                                     AssetCallback onComplete, uint8_t priority) {
    uint32_t id = m_nextRequestId++;

    // 检查缓存：同路径已加载过则直接回调解
    if (TryCache(path, onComplete)) {
        return id;
    }

    if (!m_executor || !m_animMgr) {
        if (onComplete) {
            AssetResult result;
            result.type = AssetType::Animation;
            result.path = path;
            result.success = false;
            onComplete(result);
        }
        return id;
    }

    auto sharedPath = std::make_shared<std::string>(path);
    auto callback = std::move(onComplete);

    {
        char buf[256];
        sprintf_s(buf, "[AssetManager] Loading Animation: %s\n", path.c_str());
        OutputDebugStringA(buf);
    }

    // Delegate to AnimationLoadTask（纯 CPU：解析 .anim JSON → 注册到 AnimationManager）
    auto result = std::make_shared<Async::AnimationLoadResult>();
    auto task = Async::AnimationLoadTask::Create(path, m_animMgr, boneNames, result);
    auto origOnComplete = std::move(task.onComplete);
    task.onComplete = [this, sharedPath, callback, result,
                       origOnComplete = std::move(origOnComplete)](bool success) mutable {
        if (origOnComplete)
            origOnComplete(success);
        AssetResult res;
        res.type = AssetType::Animation;
        res.path = *sharedPath;
        res.success = result->handle.IsValid();
        if (result->handle.IsValid())
            res.clipHandle = result->handle;
        m_cache[*sharedPath] = res;
        if (callback)
            callback(res);
    };
    m_executor->SubmitLoadTask(std::move(task));
    return id;
}

bool AssetManager::TryCache(const std::string &path, AssetCallback &callback) {
    auto it = m_cache.find(path);
    if (it != m_cache.end()) {
        if (callback)
            callback(it->second);
        return true;
    }
    return false;
}

AssetBatchPtr AssetManager::LoadBatch(const std::vector<std::string> &paths, AssetCallback perAssetComplete,
                                      std::function<void()> onAllComplete) {

    uint32_t batchId = m_nextBatchId++;
    std::vector<AssetRequest> requests;
    requests.reserve(paths.size());

    for (const auto &path : paths) {
        requests.push_back({m_nextRequestId++, path, 1, [this, perAssetComplete, batchId](const AssetResult &result) {
                                if (perAssetComplete)
                                    perAssetComplete(result);

                                auto it = std::find_if(m_activeBatches.begin(), m_activeBatches.end(),
                                                       [batchId](const AssetBatchPtr &b) { return b->id == batchId; });
                                if (it == m_activeBatches.end())
                                    return;

                                (*it)->OnAssetComplete(result.success);
                            }});
    }

    auto batch = std::make_shared<AssetBatch>(batchId, std::move(requests), std::move(onAllComplete));
    m_activeBatches.push_back(batch);

    for (auto &req : batch->requests) {
        Load(req.path, req.onComplete, req.priority);
    }

    return batch;
}

void AssetManager::Update() {
    // BackgroundExecutor::Tick 由 Editor/Game 主循环驱动（见项目规则 #15），
    // AssetManager 自身无需额外动作；保留入口以兼容既有调用。
}

} // namespace DX12Engine::Resource
