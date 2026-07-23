#pragma once

#include "ECS/Core/Entity.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/TextureHandle.h"
#include <memory>

namespace DX12Engine {
namespace Boot {
class GameContext;
}
namespace ECS {
class Registry;
}
} // namespace DX12Engine

/// 游戏资源初始化 —— 引擎启动时所需的 GPU 资源
///
/// 职责：
///   - 创建 1x1 纯白纹理（反射测试立方体用）
///   - 预触 ECS 组件存储池（避免 Worker 线程竞态）
class GameResources {
public:
    GameResources() = default;
    ~GameResources() = default;

    GameResources(const GameResources &) = delete;
    GameResources &operator=(const GameResources &) = delete;

    void Initialize(DX12Engine::Boot::GameContext *context);

    /// 获取纯白纹理句柄
    DX12Engine::Resource::TextureHandle GetWhiteTextureHandle() const { return m_whiteTextureHandle; }

private:
    DX12Engine::Boot::GameContext *m_context = nullptr;

    DX12Engine::Resource::TextureHandle m_whiteTextureHandle;
    uint32_t m_whiteTextureSrvSlot = UINT32_MAX;
};