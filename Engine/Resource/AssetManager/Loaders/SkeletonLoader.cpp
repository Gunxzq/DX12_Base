#include "Background/SkeletonLoadTask.h"
#include "Common/Common.h"
#include "Resource/AssetManager/AssetManager.h"

namespace DX12Engine::Resource {

// ========================================================================
// Skeleton Loader — .bone → Skeleton
// 按资产类型拆文件（2026-08-02，参照属性卡 Register*Editor 模式）
// ========================================================================

void AssetManager::RegisterSkeletonLoader() {
    RegisterLoader(".bone", AssetType::Skeleton, [this](const std::string &p, AssetCallback cb, uint8_t pr) {
        return LoadSkeletonImpl(p, std::move(cb), pr);
    });
}

uint32_t AssetManager::LoadSkeletonImpl(const std::string &path, AssetCallback onComplete, uint8_t priority) {
    uint32_t id = m_nextRequestId++;
    auto sharedPath = std::make_shared<std::string>(path);
    auto callback = std::move(onComplete);

    {
        char buf[256];
        sprintf_s(buf, "[AssetManager] Loading Skeleton: %s\n", path.c_str());
        OutputDebugStringA(buf);
    }

    // Delegate to SkeletonLoadTask（纯 CPU：解析 .bone JSON → 注册到 SkeletonManager）
    auto result = std::make_shared<Async::SkeletonLoadResult>();
    auto task = Async::SkeletonLoadTask::Create(path, m_skeletonMgr, result);
    auto origOnComplete = std::move(task.onComplete);
    task.onComplete = [this, sharedPath, callback, result,
                       origOnComplete = std::move(origOnComplete)](bool success) mutable {
        if (origOnComplete)
            origOnComplete(success);
        AssetResult res;
        res.type = AssetType::Skeleton;
        res.path = *sharedPath;
        res.success = result->handle.IsValid();
        if (result->handle.IsValid())
            res.skeletonHandle = result->handle;
        m_cache[*sharedPath] = res;
        if (callback)
            callback(res);
    };
    m_executor->SubmitLoadTask(std::move(task));
    return id;
}

} // namespace DX12Engine::Resource
