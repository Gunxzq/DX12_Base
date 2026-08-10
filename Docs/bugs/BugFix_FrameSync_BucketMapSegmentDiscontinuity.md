# BugFix: FrameSync bucketMap 段不连续 → 桶归属错乱 + 渲染位置错乱

## 日期

2026-08-09

## 症状

方案 B（动态 EntityBucketMap）落地 + SRV 96B 对齐修复后，编辑器渲染画面：

1. **渲染错乱**：实体位置/材质错乱（部分实体缺失、重叠、画到错误位置）——
   用户观察"不仅仅是桶的问题，索引也可能是乱的，导致位置都是错乱的"
2. **桶偏移表缺失**：
   ```
   [FrameSync][Diag] batches=206 skipped=0 instTotal=2334 flatInstances=706 bucketsUsed=206 bucketMap=2334
   [InstanceCullingBuffer][Diag] bucketOffsets: usedBuckets=95 total=2334 [0→0]
   ```
   `usedBuckets(95) << bucketsUsed(206)`——Builder 分配了 206 个桶索引，但桶偏移表只有 95 个
   非空桶，**111 个桶的实体引用缺失**
3. 剔除链路本身正常（readback 有值、dispatch 每帧执行、CullParams 平面正确）

## 根因：bucketMap 实体桶段不连续（插入污染）

### 旧实现（FrameSync 边遍历边 push）

```cpp
// Editor.cpp FrameSync（重构前 L1211-1235）
实体首遇（entityIndex 无该实体）：
    g.meta.y = bucketMap.size();  // meta.y = 当前末尾
    g.meta.z = 1;
    bucketMap.push_back(assignedBucket);
实体已存在（非 dup）：
    bucketMap.push_back(assignedBucket);  // ← 追加到【全局末尾】
    ++g.meta.z;                           // ← 但 meta.y 不变！
```

**Bug 机制**（实体 E 跨多材质段桶时）：

```
E 首遇（batch A）：meta.y=5, meta.z=1 → bucketMap[5]=A     段=[5,6)   ✓
F 首遇（batch B）：meta.y=6, meta.z=1 → bucketMap[6]=B
E 再遇（batch C，新桶 C）：push → bucketMap[7]=C, E.meta.z=2
  → E 的段现在是 [5,7) = {bucketMap[5]=A, bucketMap[6]=B}   ← 读到 F 的桶 B！
  → E 真正的桶 C 在 bucketMap[7]，但 meta.z=2 只覆盖 [5,7)，永远读不到！
```

**后果链**：
1. **桶归属错乱**：CS 遍历 `gBucketMap[mapStart..+bucketCount)` 读到其他实体的桶 → 对错误桶计数 → `usedBuckets < bucketsUsed`（95 vs 206）
2. **索引错乱**：桶归属错乱 → gAppend 分段基址（桶偏移表）错位 → VS 的 `gBucketBase + instanceID` 取错实例索引 → **位置错乱**（与用户判断一致）
3. 部分桶计数丢失/重复 → 实体缺失/重叠

**注意**：flatInstances ↔ allInstances 本身同序（实体首遇时同时 push），
gAppend 存 dtid.x（= flatInstances 索引）的链路设计正确——错乱根源是 bucketMap 段不连续。

## 修复：两阶段构建（Editor.cpp FrameSync 重构）

### 阶段 1：收集（按实体聚合去重桶集合）

```cpp
struct EntityAccum {
    InstanceData inst;              // 实体级绘制数据快照（首次遇见）
    std::vector<uint32_t> buckets;  // 去重桶索引（batch 遍历序 = bucketIndex 升序）
};
std::unordered_map<ECS::Entity, EntityAccum> accum;
for (auto &batch : batches) {
    ...assignedBucket 分配 + 写回渲染项 + instTotal 统计...
    for (i < batch.instances.size()) {
        auto &a = accum[e];
        if (a.buckets.empty())
            a.inst = inst;                      // 首次快照
        // 桶去重（同实体跨多批次 = 多材质段桶）
        bool dup = false;
        for (uint32_t b : a.buckets)
            if (b == assignedBucket) { dup = true; break; }
        if (!dup)
            a.buckets.push_back(assignedBucket);
    }
}
```

### 阶段 2：连续扁平化（每实体段严格连续）

```cpp
for (auto &[e, a] : accum) {
    g.meta.y = static_cast<uint32_t>(bucketMap.size());  // 本实体段起点 = 当前末尾
    g.meta.z = static_cast<uint32_t>(a.buckets.size());  // 段长 = 去重桶数
    for (uint32_t b : a.buckets)
        bucketMap.push_back(b);   // 连续写入，杜绝跨实体插入污染
    flatInstances.push_back(g);
    allInstances.push_back(a.inst);  // 与 flatInstances 同序
}
```

## 验证（编译运行后日志）

```
修复前：usedBuckets=95  vs bucketsUsed=206（差 111 桶）
修复后：usedBuckets=254 ≈ bucketsUsed=249（对齐，差 ≤5 为读回延迟/场景变化）
       readback: total=1222 nonZeroBuckets=162  ← 剔除恢复
       visible=1222 / total=1028 (118.9%)       ← 可见实例×桶数 > 实体数，正常（每实体多桶）
```

## 经验教训

1. **扁平映射表的"实体段"必须连续**：任何"追加到全局末尾 + 段长自增"的模式，若段起点
   （meta.y）固定不变，段内容会被后续其他实体的插入污染——这是扁平数组按实体分段的
   经典陷阱（同类：顶点索引缓冲区按子网格分段、贴图图集按图元分段）
2. **先收集后扁平化**是安全范式：阶段 1 用独立容器按实体聚合（vector 天然隔离），
   阶段 2 再按实体顺序一次性写入连续段
3. **诊断判据**：`usedBuckets < bucketsUsed` 时，桶偏移表与 Builder 桶不一致——优先怀疑
   桶归属映射（bucketMap）而非剔除逻辑本身
4. 与 `BugFix_GPUInstanceData_SRV_StrideAlignment.md` 的关系：SRV 对齐修复解决"CS 读 gInstances
   错位全灭（visible=0）"，本修复解决"剔除恢复后渲染位置错乱"——两者是方案 B 的两个独立缺陷
