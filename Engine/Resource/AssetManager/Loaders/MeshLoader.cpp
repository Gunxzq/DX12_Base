#include "Background/MeshLoadTask.h"
#include "Common/Common.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/AssetManager/AssetManager.h"

namespace DX12Engine::Resource {

// ========================================================================
// Mesh Loader — .dxmesh / .obj → Mesh
// 按资产类型拆文件（2026-08-02，参照属性卡 Register*Editor 模式）
// 注册时机：AssetManager::Initialize 末尾，或任何使用 AssetManager 之前
// ========================================================================

void AssetManager::RegisterMeshLoader() {
    RegisterLoader(".dxmesh", AssetType::Mesh, [this](const std::string &p, AssetCallback cb, uint8_t pr) {
        return LoadMeshImpl(p, std::move(cb), pr);
    });
    RegisterLoader(".obj", AssetType::Mesh, [this](const std::string &p, AssetCallback cb, uint8_t pr) {
        return LoadMeshImpl(p, std::move(cb), pr);
    });
}

uint32_t AssetManager::LoadMeshImpl(const std::string &path, AssetCallback onComplete, uint8_t priority) {
    uint32_t id = m_nextRequestId++;
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
    auto task = Async::MeshLoadTask::Create(path, m_deviceContext->GetDevice(), &m_deviceContext->GetCommandManager(),
                                            m_geoMgr, fence, result);
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
    return id;
}

} // namespace DX12Engine::Resource
