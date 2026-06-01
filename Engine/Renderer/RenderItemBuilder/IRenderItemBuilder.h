#pragma once

#include "ECS/Core/Registry.h"
#include <any>
#include <typeindex>

namespace DX12Engine::Renderer {

// ============================================================================
// 构建器基类（类型擦除）
// ============================================================================
// 基类不定义任何上下文参数
class IRenderItemBuilder {
public:
    virtual ~IRenderItemBuilder() = default;

    // 派生类自己决定需要什么参数
    virtual void Build(ECS::Registry &registry, std::any &outQueue) = 0;

    virtual const char *GetName() const { return "IRenderItemBuilder"; }
};

// ============================================================================
// 模板化构建器（类型安全）
// ============================================================================
template <typename TQueue> class TRenderItemBuilder : public IRenderItemBuilder {
public:
    using QueueType = TQueue;

    virtual void BuildTyped(ECS::Registry &registry, TQueue &outQueue) = 0;

    void Build(ECS::Registry &registry, std::any &outQueue) override {
        if (auto *queue = std::any_cast<TQueue>(&outQueue)) {
            BuildTyped(registry, *queue);
        }
    }
};
} // namespace DX12Engine::Renderer