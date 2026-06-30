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

---

## 补充修复（Bug 3 & Bug 4）

### Bug 3：`AllocateConsecutive` FreeList 模式扩容路径同样存在双重分配

与 Bug 2 相同模式：FreeList 模式下 `AllocateConsecutive` 触发扩容后，`Expand` 将新索引推入 `m_freeIndices`，但随后走 `m_nextIndex` 路径分配，导致已分配的索引仍残留于 `m_freeIndices`。

**修复**：扩容后优先从 `m_freeIndices` 扫描连续块，找不到再回退到 `m_nextIndex`。

### Bug 4：LinearAlloc 模式下 `Expand` 错误地填充 `m_freeIndices`

LinearAlloc 模式下分配始终从 `m_nextIndex` 递增，不使用 `m_freeIndices`。但 `Expand` 无条件将新索引推入 `m_freeIndices`，虽然不直接导致冲突，但造成了内存浪费和逻辑不一致。

**修复**：`Expand` 仅在非 LinearAlloc 模式下填充 `m_freeIndices`。

### 其余模块扩容风险评估

| 模块 | 风险 | 状态 |
|------|------|------|
| `HandlePoolBase::ExpandCapacity` | 并发扩容时原子操作与指针替换非原子 | 低风险，有 mutex 保护 |
| `TextureManager::AllocateEntry` | SRV 索引依赖 DescriptorSlotAllocator，后者已修复 | 已消除 |
| `RingBuffer` (FrameResourceManager/TerrainManager) | 扩容时 `Shutdown()` 销毁旧 GPU 资源，若 GPU 仍在读取则 use-after-free | **中风险**，当前通过 fence 延迟回收规避，暂无实际触发 |
| `MaterialManager::GetGPUMaterialList` | 空洞填充可能导致数组膨胀 | 低风险 |

---

## 第二轮修复：Resource 层底层分配器安全审查

### Bug 5：RenderTargetPool/DepthStencilPool PurgeUnused 使用 vector::erase 导致索引前移

**文件**: `RenderTargetPool.cpp`, `DepthStencilPool.cpp`

`PurgeUnused` 释放资源后调用 `m_pool.erase(it)` 删除条目，导致被删条目之后所有条目的索引前移。如果外部持有指向后续条目的 `RenderTargetHandle`，`handle.poolIndex` 会指向错误条目。generation 检查只能防止访问已释放的条目，但无法防止访问到索引前移后的错误条目。

**修复**: 不再 erase，改为递增 `entry.generation` 使旧句柄失效。空闲条目留在 `m_pool` 中等待 `FindMatchingEntry` 复用。

### Bug 6：SamplerCache PurgeUnused 使用 vector::erase 导致索引前移

**文件**: `SamplerCache.cpp`

与 Bug 5 相同，且 `m_presetIndices` 中缓存的索引也会失效。此外 `GetCpuHandle`/`GetGpuHandle` 不验证 generation。

**修复**: 不再 erase，递增 generation 使旧句柄失效；`GetCpuHandle`/`GetGpuHandle` 添加 generation 验证。

### Bug 7：GeometryResourceManager generation 截断

**文件**: `GeometryResourceManager.cpp`, `GeometryHandle.h`

`GeometryHandle::generation` 是 10 位字段，但 `Entry::generation` 和 `m_nextGeneration` 是 32 位。当 `m_nextGeneration >= 1024` 时，`handle.generation = entry.generation` 会截断到低 10 位，`IsValid` 中 `entry.generation == handle.generation` 永远不匹配，所有句柄失效。

**修复**: 赋值时 `handle.generation = entry.generation & 0x3FF`，比较时取低 10 位比较。

### Bug 8：HandlePoolBase::m_capacity 非原子读取

**文件**: `HandlePoolBase.h`, `CpuHandlePool.cpp`, `GpuHandlePool.cpp`

TLS 缓存无锁路径中 `index >= m_capacity` 读取 `m_capacity`（普通 uint32_t），而扩容线程在锁内写入 `m_capacity`，形成 data race (UB)。

**修复**: `m_capacity` 改为 `std::atomic<uint32_t>`。

### 其余模块风险评估（更新）

| 模块 | 风险 | 状态 |
|------|------|------|
| `HandlePoolBase::ExpandCapacity` | 并发扩容时原子操作与指针替换非原子 | 低风险，有 mutex 保护 |
| `TextureManager::AllocateEntry` | SRV 索引依赖 DescriptorSlotAllocator，后者已修复 | 已消除 |
| `RingBuffer` (FrameResourceManager/TerrainManager) | 扩容时 `Shutdown()` 销毁旧 GPU 资源 | 中风险，fence 延迟回收，暂无实际触发 |
| `MaterialManager::GetGPUMaterialList` | 空洞填充可能导致数组膨胀 | 低风险 |
| `AssetDataManager::ReleaseRef` | refCount==0 立即释放不考虑 GPU 使用 | 中风险 |
| `AssetDataManager::Reclaim` | 路径收集和释放的非原子性 | 中风险 |
| `DataPool::Reset` | 旧指针悬垂（依赖调用方纪律） | 低风险 |
| `GpuResourceManager::Shutdown` | 强制释放可能 GPU 仍在使用 | 低风险 |
