#include "Resource/AssetManager/AssetManager.h"
// TextureLoadTask.h 的内联实现使用 GpuResourceManager/CommandManager 完整类型，
// 但该头自身不包含它们——须先于 TextureLoadTask.h 引入（原 AssetManager.cpp 由 MeshLoadTask.h 间接提供）
#include "Background/TextureLoadTask.h"
#include "Common/Common.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/GpuResourceManager.h"

namespace DX12Engine::Resource {

// ========================================================================
// Texture Loader — .dds/.png/.jpg/.jpeg/.bmp/.tga → Texture
// 按资产类型拆文件（2026-08-02，参照属性卡 Register*Editor 模式）
// ========================================================================

void AssetManager::RegisterTextureLoader() {
    RegisterLoader(".dds", AssetType::Texture, [this](const std::string &p, AssetCallback cb, uint8_t pr) {
        return LoadTextureImpl(p, std::move(cb), pr);
    });
    RegisterLoader(".png", AssetType::Texture, [this](const std::string &p, AssetCallback cb, uint8_t pr) {
        return LoadTextureImpl(p, std::move(cb), pr);
    });
    RegisterLoader(".jpg", AssetType::Texture, [this](const std::string &p, AssetCallback cb, uint8_t pr) {
        return LoadTextureImpl(p, std::move(cb), pr);
    });
    RegisterLoader(".jpeg", AssetType::Texture, [this](const std::string &p, AssetCallback cb, uint8_t pr) {
        return LoadTextureImpl(p, std::move(cb), pr);
    });
    RegisterLoader(".bmp", AssetType::Texture, [this](const std::string &p, AssetCallback cb, uint8_t pr) {
        return LoadTextureImpl(p, std::move(cb), pr);
    });
    RegisterLoader(".tga", AssetType::Texture, [this](const std::string &p, AssetCallback cb, uint8_t pr) {
        return LoadTextureImpl(p, std::move(cb), pr);
    });
}

uint32_t AssetManager::LoadTextureImpl(const std::string &path, AssetCallback onComplete, uint8_t priority) {
    uint32_t id = m_nextRequestId++;
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
    return id;
}

} // namespace DX12Engine::Resource
