# 引擎 CORE 固定开销记录（Known Fixed Overhead）

> 2026-08-10 帧率排查结论存档——**下次优化/排查帧率时，先读本文档跳过已知固定开销，避免重复排查到引擎 CORE**。
>
> 排查背景：Editor 空场景稳定 60 帧、非空场景 40-43 FPS（引用化修复后 52-57 FPS）——逐层定位（Perf 细分 → TaskExecutor 三段 → 任务级 → DispatchSeg）确认以下引擎 CORE 固定开销。

---

## 一、固定开销清单（本次会话确认）

### 1. TaskFlow 调度固定开销（dispatch ~9.7-10.5ms/帧）

| 项 | 数值 | 位置 | 判定 |
|:--|:--|:--|:--|
| ExecutePhase 每 Phase 重建 TaskFlow 图 | 8 次 `clear()` + emplace + `run+wait` | TaskExecutor.cpp:87-150 | **已知固定开销**（与场景内容无关的调度成本） |
| PreRender（p=5）单 Phase | 9.0-9.8ms（maxP=5） | ExecutePhase run+wait | **非任务执行**（25 任务 <1ms）、非重建（sort/emplace <0.1ms）、非 Worker 等待（wait=0）——**TaskFlow 调度器开销** |
| SortByPhase 每 Phase 全量拓扑 | 8 次重复（Kahn O(V+E) 快，但重复） | TaskGraph.cpp:68-72 | 已知（非瓶颈——sort <0.1ms） |

- **已有日志**：`[TaskExecutor][Diag] ExecutePhase p={} sort/emplace/runWait`（每 Phase 独立节流 120 帧）+ `SlowestTask p={} '任务名' = {ms}`（任务级计时）
- **优化方向（未实施）**：TaskFlow 图缓存（方案 A）/ 单 TaskFlow 图 + Phase precede + 一次 run（方案 B）/ 弃 TaskFlow 自管线程池（方案 C）——详见 2026-08-10 讨论

### 2. TaskExecutor.cpp:83 另一处 run+wait

- **现象**：`SubmitGraph`（或类似函数，TaskExecutor.cpp:60-86 区域）也有 `m_executor.run(m_taskflow).wait()`——**每帧可能多次 TaskFlow 执行**（ExecutePhase × 8 + 该函数）
- **判定**：已知（未完全量化——若优化 TaskFlow 需一并排查）

### 3. EditorBuilderUpload 执行体（非空场景 CPU 开销）

| 段 | 修复前 | 修复后（2026-08-10 引用化） | 位置 |
|:--|:--|:--|:--|
| blockExpand（块展开拷贝 → 指针 push） | 5.4ms（9402 Entry 拷贝） | **1.05ms（23238 指针 push——City4）** | RenderSlotCache.cpp Dispatch 块展开段 |
| 执行体其他（SetLOD/Dispatch/水挂载） | ~2ms | ~8ms（dispatch 9ms 的剩余） | Editor.cpp EditorBuilderUpload |

- **已有日志**：`[EditorBuilderUpload][Diag] set/dispatch/water` + `[RenderSlotCache][Diag] DispatchSeg total/blockExpand/table/expandedMembers`
- **判定**：引用化后 blockExpand 已优化（指针引用——UE FMeshDrawCommand 模式）；剩余 ~8ms 在 Builder 消费/SetLOD 等——非空场景 CPU 组成

### 4. 空场景 60 帧 = DWM/Vsync 同步锁（非 system 开销）

- **现象**：空场景（无实体）稳定正好 60 帧——**非 system 计算开销**（空场景 system 快仍 60）
- **根因**：窗口化模式 **DWM 按 60Hz 合成**（即使 `enableVsync=false` → `Present(0,0)` 已生效，DWM 仍锁 60）——"正好 60"的 60Hz 同步特征
- **可控性**：`enableVsync` 已在 renderer.json 配置（Editor `false`、Game 未加默认 true——**Game 端如需不锁，Game/Config/renderer.json 加 `"enableVsync": false`**；全屏独占 Present 可绕过 DWM）
- **判定**：**已知同步锁，非 bug、非 CORE 计算开销**——下次排查帧率时跳过（编辑器 60 锁可接受，UE/Unity 编辑器同此）

### 5. 偶发 82-105ms 尖峰（资源 Purge 周期性）

- **现象**：每 ~60 帧一次 82-105ms（10-12 FPS）尖峰
- **根因**：`RenderTargetPool/DepthStencilPool::PurgeUnused`（Editor.cpp 每 60 帧触发）——资源回收同步
- **判定**：**已知周期性固定开销**——非帧率瓶颈（单帧尖峰，用户确认正常）

### 6. getComp 结论修正（Builder 侧重新解析）

- **现象**：`Opaque/SkinnedRenderItemBuilder` 消费桶时用 `registry.TryGetComponent<MeshComponent/TransformComponent>(entry.entity)` **重新解析**（OpaqueRenderItemBuilder.cpp:62-63 等）——**未用 Entry 缓存的组件指针**（Water 用 `entry.meshComp` 缓存指针——零 getComp）
- **修正**："TryGetComponent ~2.7μs/次 ×4 = getComp 占 BuildTyped 70%"结论**源于 Builder 重新解析**（非 Dispatch——Dispatch 已缓存指针到 Entry）
- **优化方向（未实施）**：Builder 统一改用 `entry.meshComp/entry.transformComp` 缓存指针（消除 Builder 侧 getComp）

### 7. RenderSlotCache 引用化已知项（2026-08-10）

- 桶存 `const Entry*`（指向 `m_blockExpanded` 缓存——**Rebuild 唯一修改路径**，IsDirty 驱动，指针稳定）；表分发段/DispatchAll 存 `&m_dispatchEntries.back()`（**已 reserve 预留——扩容失效已修**，2026-08-10 断言崩溃根因）
- **下次注意**：桶指针生命周期 = 本帧 Dispatch → Builder 消费（帧内安全）；m_blockExpanded 指针跨帧稳定（Rebuild 前）

---

## 二、下次排查帧率跳过指南

1. **先看 Perf 每帧**（`frame/imm/render/fsync/end/graph/update/phase/dispatch/main/wait/maxP` + `gpuBehind` + `tasks`）——`gpuBehind=0` = CPU 瓶颈、`dispatch` 大 = TaskFlow 固定开销（§一.1）、`maxP=5` = PreRender/EditorBuilderUpload（§一.3）；
2. **空场景 60 帧** → DWM/Vsync 同步锁（§一.4）——**不再排查 system**；
3. **偶发 ~100ms 尖峰** → 资源 Purge（§一.5）——**不再排查**；
4. **dispatch ~10ms** → TaskFlow 调度固定开销（§一.1/一.2）——**不再深入 CORE**（优化方向已记录）；
5. **blockExpand**（`DispatchSeg`）→ 已引用化（§一.3/一.7）——若仍大查 Builder 消费（§一.6 getComp）而非 Dispatch。

---

## 三、相关日志标记速查

> **2026-08-10 清理**：以下排查期诊断日志已移除（CORE 开销已确认并记录于 §一——TaskExecutor 三段
> `[TaskExecutor][Diag] ExecutePhase sort/emplace/runWait`、任务级 `SlowestTask`、执行体 `[EditorBuilderUpload][Diag] set/dispatch/water`、
> `[RenderSlotCache][Diag] DispatchSeg/Dispatch 汇总/IsDirty`）——**不再输出，避免不必要的帧率分析**；
> 仅保留每帧核心观察日志：

| 日志 | 含义 |
|:--|:--|
| `[Perf][Diag] frame=... gpuBehind=... dispatch=... maxP=...` | 帧时间/瓶颈/阶段细分（Editor.cpp 每帧，不节流） |

> 若需复现 §一 各项固定开销（TaskFlow 调度/EditorBuilderUpload/块展开），按 §一 记录的位置与数值参考
> （dispatch ~10ms = TaskFlow 固定开销、blockExpand 已引用化、空场景 60 = DWM 锁等），无需再加诊断日志。
