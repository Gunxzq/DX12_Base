#include "Background/MaterialLoadTask.h"
#include "Resource/AssetManager/AssetManager.h"

namespace DX12Engine::Resource {

// ========================================================================
// Material Loader — .mat → Material
// 按资产类型拆文件（2026-08-02，参照属性卡 Register*Editor 模式）
// ========================================================================

void AssetManager::RegisterMaterialLoader() {
    RegisterLoader(".mat", AssetType::Material, [this](const std::string &p, AssetCallback cb, uint8_t pr) {
        return LoadMaterialImpl(p, std::move(cb), pr);
    });
}

uint32_t AssetManager::LoadMaterialImpl(const std::string &path, AssetCallback onComplete, uint8_t priority) {
    uint32_t id = m_nextRequestId++;
    auto sharedPath = std::make_shared<std::string>(path);
    auto callback = std::move(onComplete);

    // 传入外部 result，MaterialLoadTask 内部将 handle 写入此对象
    auto assetResult = std::make_shared<Async::MaterialLoadResult>();
    auto task = Async::MaterialLoadTask::Create(path, m_matMgr, assetResult);
    // task.onComplete 包含 RegisterMaterial 逻辑，在其基础上叠加缓存
    auto origOnComplete = std::move(task.onComplete);
    task.onComplete = [this, sharedPath, callback, assetResult,
                       origOnComplete = std::move(origOnComplete)](bool success) mutable {
        // 先执行 MaterialLoadTask 的注册（填写 assetResult->handle）
        if (origOnComplete)
            origOnComplete(success);

        // 再执行 AssetManager 的缓存
        AssetResult res;
        res.type = AssetType::Material;
        res.path = *sharedPath;
        res.success = assetResult->handle.IsValid();
        if (assetResult->handle.IsValid())
            res.materialHandle = assetResult->handle;
        m_cache[*sharedPath] = res;
        if (callback)
            callback(res);
    };
    m_executor->SubmitLoadTask(std::move(task));
    return id;
}

} // namespace DX12Engine::Resource
