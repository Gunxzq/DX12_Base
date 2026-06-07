# BugFix: DescriptorSlotAllocator 槽位双重分配冲突

**日期**: 2026-06-07  
**文件**: `Engine/Resource/Core/DescriptorSlotAllocator.cpp`  
**影响**: 描述符堆槽位被两个不同资源占用，导致 GPU 句柄冲突，渲染异常。

---

## 问题现象

`LightManager` 的 `shadowDataSRV`（槽位 0）与地形高度图 SRV 占用了**同一个描述符堆槽位**，GPU 句柄完全一致：

```
shadowDataSRV.ptr  = 0x800001FDF2C1305D  (槽位 0)
heightMapSRV.ptr   = 0x800001FDF2C1305D  (槽位 0)  ← 冲突！
```

## 根因分析

### Bug 1：`Initialize` 中 `Reserve` 未生效

```cpp
// 修复前
void DescriptorSlotAllocator::Initialize(const DescriptorSlotAllocatorConfig &config) {
    // ...
    Reserve(m_config.initialCapacity); // ← 此时 m_initialized = false
    m_initialized = true;              // ← 在这里才设为 true
}
```

`Reserve` 的第一行是 `if (!m_initialized) return;`，由于 `Initialize` 调用它时 `m_initialized` 还是 `false`，`Reserve` 直接返回，导致：
- `m_capacity = 0`
- `m_freeIndices = []`（空）

### Bug 2：扩容后 `AllocateFromFreeList` 走了错误的分配路径

第一次 `Allocate()` 调用 `AllocateFromFreeList()`：
1. `m_freeIndices` 为空 → 跳过
2. `m_nextIndex(0) >= m_capacity(0)` → 触发 `Expand(65536)`
3. `Expand` 把 `0, 1, 2, ..., 65535` 全部推入 `m_freeIndices`
4. **Bug**: 扩容后走了 `if (m_nextIndex < m_capacity)` 分支，返回 `m_nextIndex=0`
5. 但 `m_freeIndices` 中的索引 0 **没有被移除**

**结果**: 槽位 0 同时存在于"已分配"（通过 `m_nextIndex` 路径）和"空闲列表"（`m_freeIndices` 仍包含 0）中。

后续 `AllocateConsecutive(2)` 对 `m_freeIndices` 排序后找到连续块 `[0, 1]`，再次分配了槽位 0 和 1，导致双重占用。

---

## 修复方案

### 修复 1：调整 `m_initialized` 设置顺序

```cpp
void DescriptorSlotAllocator::Initialize(const DescriptorSlotAllocatorConfig &config) {
    // ...
    m_initialized = true;              // ← 先设为 true
    Reserve(m_config.initialCapacity); // ← 再调用 Reserve
}
```

### 修复 2：扩容后从 `m_freeIndices` 分配

```cpp
// AllocateFromFreeList 扩容分支
if (HasFlag(m_config.flags, DescriptorSlotFlags::EnableExpand)) {
    uint32_t newCapacity = m_capacity == 0 ? m_config.initialCapacity : m_capacity * 2;
    Expand(newCapacity);

    // 扩容后 m_freeIndices 已被填充，必须从 m_freeIndices 分配
    if (!m_freeIndices.empty()) {
        uint32_t index = m_freeIndices.back();
        m_freeIndices.pop_back();
        ++m_allocatedCount;
        return index;
    }
}
```

---

## 影响范围

- `DescriptorSlotAllocator::Initialize` — 修复 Reserve 未生效
- `DescriptorSlotAllocator::AllocateFromFreeList` — 修复扩容后分配路径
