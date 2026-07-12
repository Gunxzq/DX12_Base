#include "AssetManager.h"
#include "Asset/IO/AssetLoader.h"
#include "Asset/IO/Loader/DDSLoader.h"
#include "Background/MaterialLoadTask.h"
#include "Background/MeshLoadTask.h"
#include "Background/TextureLoadTask.h"
#include "Common/Common.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Texture/TextureManager.h"
#include <algorithm>
#include <fstream>

namespace DX12Engine::Resource {

AssetManager &AssetManager::GetInstance() {
    static AssetManager instance;
    return instance;
}

void AssetManager::Initialize(Renderer::D3D12DeviceContext *deviceContext, Async::BackgroundExecutor *executor,
                              Resource::GeometryResourceManager *geoMgr, Resource::MaterialManager *matMgr,
                              Resource::TextureManager *texMgr, Resource::DescriptorHeapCollection *descHeaps) {
    m_deviceContext = deviceContext;
    m_executor = executor;
    m_geoMgr = geoMgr;
    m_matMgr = matMgr;
    m_texMgr = texMgr;
    m_descHeaps = descHeaps;
}

void AssetManager::Shutdown() {
    m_activeBatches.clear();
    m_deviceContext = nullptr;
    m_executor = nullptr;
}

uint32_t AssetManager::Load(const std::string &path, AssetType type, AssetCallback onComplete, uint8_t priority) {
    uint32_t id = m_nextRequestId++;

    // 检查缓存：同路径已加载过则直接回调解
    if (TryCache(path, onComplete)) {
        return id;
    }

    if (!m_deviceContext || !m_executor) {
        if (onComplete) {
            AssetResult result;
            result.type = type;
            result.path = path;
            result.success = false;
            onComplete(result);
        }
        return id;
    }

    switch (type) {
    case AssetType::Mesh: {
        auto sharedPath = std::make_shared<std::string>(path);
        auto callback = std::move(onComplete);

        {
            char buf[256];
            sprintf_s(buf, "[AssetManager] Loading Mesh: %s\n", path.c_str());
            OutputDebugStringA(buf);
        }

        // Delegate to MeshLoadTask
        auto result = std::make_shared<Async::MeshLoadOutput>();
        uint64_t fence = m_deviceContext->GetCommandManager().GetNextSequence();
        auto task = Async::MeshLoadTask::Create(path, m_deviceContext->GetDevice(),
                                                &m_deviceContext->GetCommandManager(), m_geoMgr, fence, result);
        auto origOnComplete = std::move(task.onComplete);
        task.onComplete = [this, sharedPath, callback, result,
                           origOnComplete = std::move(origOnComplete)](bool success) mutable {
            if (origOnComplete)
                origOnComplete(success);
            AssetResult res;
            res.type = AssetType::Mesh;
            res.path = *sharedPath;
            res.success = result->success;
            if (result->success)
                res.geometryHandle = result->geometryHandle;
            m_cache[*sharedPath] = res;
            if (callback)
                callback(res);
        };
        m_executor->SubmitLoadTask(std::move(task));
        break;
    }
    case AssetType::Texture: {
        auto sharedPath = std::make_shared<std::string>(path);
        auto callback = std::move(onComplete);

        {
            char buf[256];
            sprintf_s(buf, "[AssetManager] Loading Texture: %s\n", path.c_str());
            OutputDebugStringA(buf);
        }

        // Delegate to TextureLoadTask — standardised async texture pipeline
        auto result = std::make_shared<Async::TextureLoadOutput>();
        uint64_t fence = m_deviceContext->GetCommandManager().GetNextSequence();
        auto task =
            Async::TextureLoadTask::Create(path, m_deviceContext->GetDevice(), &m_deviceContext->GetCommandManager(),
                                           m_texMgr, m_descHeaps, fence, result);
        auto origOnComplete = std::move(task.onComplete);
        task.onComplete = [this, sharedPath, callback, result,
                           origOnComplete = std::move(origOnComplete)](bool success) mutable {
            if (origOnComplete)
                origOnComplete(success);
            AssetResult res;
            res.type = AssetType::Texture;
            res.path = *sharedPath;
            res.success = result->success;
            if (result->success) {
                res.gpuHandle = result->texHandle; // 原始 GPU 资源句柄（供 Cubemap SRV 等二次使用）
                res.textureHandle = result->texRegHandle;
            }
            m_cache[*sharedPath] = res;
            if (callback)
                callback(res);
        };
        m_executor->SubmitLoadTask(std::move(task));
        break;
    }
    case AssetType::Material: {
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
        break;
    }
    case AssetType::Terrain:
    case AssetType::Scene:
    default:
        if (onComplete) {
            AssetResult result;
            result.type = type;
            result.path = path;
            result.success = false;
            onComplete(result);
        }
        break;
    }

    return id;
}

bool AssetManager::TryCache(const std::string &path, AssetCallback &callback) {
    auto it = m_cache.find(path);
    if (it == m_cache.end())
        return false;
    if (callback)
        callback(it->second);
    return true;
}

AssetBatchPtr AssetManager::LoadBatch(const std::vector<std::pair<std::string, AssetType>> &assets,
                                      AssetCallback perAssetComplete, std::function<void()> onAllComplete) {

    uint32_t batchId = m_nextBatchId++;
    std::vector<AssetRequest> requests;
    requests.reserve(assets.size());

    for (const auto &[path, type] : assets) {
        requests.push_back(
            {m_nextRequestId++, path, type, 1, [this, perAssetComplete, batchId](const AssetResult &result) {
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
        Load(req.path, req.type, req.onComplete, req.priority);
    }

    return batch;
}

void AssetManager::Update() {
    if (m_executor)
        m_executor->Tick();
}

} // namespace DX12Engine::Resource
