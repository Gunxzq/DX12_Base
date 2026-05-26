#pragma once
#include "RenderItem.h"
#include <algorithm>
#include <vector>

namespace DX12Engine::Renderer {

// ============================================================================
// 渲染队列 - 负责存储和排序渲染项
// ============================================================================
// 职责：
//   1. 存储本帧需要渲染的所有 RenderItem
//   2. 按 sortKey 排序（优化状态切换）
//   3. 不执行绘制，绘制由 System 负责
// ============================================================================

class RenderQueue {
public:
    RenderQueue() = default;
    ~RenderQueue() = default;

    // 禁止拷贝，允许移动
    RenderQueue(const RenderQueue &) = delete;
    RenderQueue &operator=(const RenderQueue &) = delete;
    RenderQueue(RenderQueue &&) noexcept = default;
    RenderQueue &operator=(RenderQueue &&) noexcept = default;

    // ========================================================================
    // 队列操作
    // ========================================================================

    void Clear() { m_items.clear(); }
    void Add(const RenderItem &item) { m_items.push_back(item); }
    void Add(RenderItem &&item) { m_items.push_back(std::move(item)); }
    void Reserve(size_t capacity) { m_items.reserve(capacity); }

    // ========================================================================
    // 排序
    // ========================================================================

    void Sort() {
        std::sort(m_items.begin(), m_items.end(),
                  [](const RenderItem &a, const RenderItem &b) { return a.sortKey < b.sortKey; });
    }
    template <typename Compare> void Sort(Compare comp) { std::sort(m_items.begin(), m_items.end(), comp); }

    // ========================================================================
    // 访问
    // ========================================================================
    const std::vector<RenderItem> &GetItems() const { return m_items; }
    size_t Size() const { return m_items.size(); }
    bool Empty() const { return m_items.empty(); }
    const RenderItem &operator[](size_t index) const { return m_items[index]; }

    // ========================================================================
    // 迭代器支持
    // ========================================================================

    auto begin() const { return m_items.begin(); }
    auto end() const { return m_items.end(); }
    auto begin() { return m_items.begin(); }
    auto end() { return m_items.end(); }

private:
    std::vector<RenderItem> m_items;
};

} // namespace DX12Engine::Renderer