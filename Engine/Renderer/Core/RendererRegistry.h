#pragma once

#include <cstdint>
#include <unordered_map>

namespace DX12Engine::Renderer {

// 前向声明
class IRenderer;

// ============================================================================
// 渲染器注册表 - 管理材质类型到渲染器的映射
// ============================================================================
// 职责：
//   1. 通过类型 Hash 注册渲染器
//   2. 提供根据 Hash 查找渲染器的能力
//   3. 用于材质系统与渲染器的解耦
// ============================================================================

class RendererRegistry {
public:
    static RendererRegistry &GetInstance() {
        static RendererRegistry instance;
        return instance;
    }

    // 注册渲染器
    void Register(uint64_t typeHash, IRenderer *renderer) { m_renderers[typeHash] = renderer; }

    // 注销渲染器
    void Unregister(uint64_t typeHash) { m_renderers.erase(typeHash); }

    // 根据类型 Hash 获取渲染器
    IRenderer *Get(uint64_t typeHash) {
        auto it = m_renderers.find(typeHash);
        return (it != m_renderers.end()) ? it->second : nullptr;
    }

    // 检查是否存在
    bool Has(uint64_t typeHash) const { return m_renderers.find(typeHash) != m_renderers.end(); }

    // 清空所有注册
    void Clear() { m_renderers.clear(); }

private:
    RendererRegistry() = default;
    ~RendererRegistry() = default;

    std::unordered_map<uint64_t, IRenderer *> m_renderers;
};

} // namespace DX12Engine::Renderer