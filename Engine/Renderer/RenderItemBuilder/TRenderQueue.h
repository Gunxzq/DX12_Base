#pragma once

#include <algorithm>
#include <vector>

namespace DX12Engine::Renderer {

// ============================================================================
// 模板化渲染队列 - 支持任意渲染项类型
// ============================================================================
template <typename T> class TRenderQueue {
public:
    TRenderQueue() = default;
    ~TRenderQueue() = default;

    // 禁止拷贝，允许移动
    TRenderQueue(const TRenderQueue &) = delete;
    TRenderQueue &operator=(const TRenderQueue &) = delete;
    TRenderQueue(TRenderQueue &&) noexcept = default;
    TRenderQueue &operator=(TRenderQueue &&) noexcept = default;

    // ========================================================================
    // 队列操作
    // ========================================================================

    void Clear() { m_items.clear(); }
    void Add(const T &item) { m_items.push_back(item); }
    void Add(T &&item) { m_items.push_back(std::move(item)); }
    void Reserve(size_t capacity) { m_items.reserve(capacity); }

    // ========================================================================
    // 排序（需要 T 有 sortKey 成员，或使用自定义比较器）
    // ========================================================================

    void Sort() {
        std::sort(m_items.begin(), m_items.end(), [](const T &a, const T &b) { return a.sortKey < b.sortKey; });
    }

    template <typename Compare> void Sort(Compare comp) { std::sort(m_items.begin(), m_items.end(), comp); }

    // ========================================================================
    // 访问
    // ========================================================================

    const std::vector<T> &GetItems() const { return m_items; }
    std::vector<T> &GetItems() { return m_items; }
    size_t Size() const { return m_items.size(); }
    bool Empty() const { return m_items.empty(); }
    const T &operator[](size_t index) const { return m_items[index]; }
    T &operator[](size_t index) { return m_items[index]; }

    // ========================================================================
    // 迭代器支持
    // ========================================================================

    auto begin() const { return m_items.begin(); }
    auto end() const { return m_items.end(); }
    auto begin() { return m_items.begin(); }
    auto end() { return m_items.end(); }

private:
    std::vector<T> m_items;
};

} // namespace DX12Engine::Renderer