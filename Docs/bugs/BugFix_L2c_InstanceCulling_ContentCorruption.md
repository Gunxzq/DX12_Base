# BugFix：L2c 实例剔除绘制错乱排查链（2026-08-09）

> 状态：**排查中（未定案）**——CPU 侧全链路已验证正常，剩余疑点收窄到 GPU 内读取/帧间错位，待 PIX 或错乱帧日志捕获定案。
> 关联：`todos/InstanceCulling_L2c_Todo.md` §九、`snapshots/GPUDriven_Snapshot_20260806.md`

## 一、现象

- 偶发绘制错乱：**部分内容位置正确、部分内容位置错误，错误内容叠加到可见范围**（用户录屏逐帧确认）
- 错乱帧几何本身不变形，**世界位置画错地方**（用户澄清）
- 线框模式（实体渲染器切 Wireframe PSO）下错乱仍可见（一瞬闪烁/叠加）——非纯材质问题
- 低频偶发、无法稳定触发、与块粒度/预测相机/相机静止守卫无关

## 二、排查链（已排除项）

| # | 假设 | 核查方法 | 结论 |
|:--|:--|:--|:--|
| 1 | 块粒度（476.25→150 小块） | 场景文件 cellSize 对比实验 | ❌ 缩小块后仍偶发（非块粒度） |
| 2 | 块级命中（画不画） | 用户推理"顶多是画不画，非内容错乱" | ❌ 非画不画问题 |
| 3 | GPU 侧流程 | GBV 运行（无警告） | ❌ GPU 绑定/状态/描述符正常 |
| 4 | dispatch 相机静止守卫 | dispatchCalls=1/275 实证收益≈0 | ✅ 已移除（每帧 dispatch） |
| 5 | 预测相机差距 | predictionFactor 0.5→2.0 频率未加大 | ❌ 非根因（已恢复） |
| 6 | 并行 Builder 拼接 | 静态核查 entryIdx%chunkCount + MergeChunk | ❌ 分块确定、拼接修正正确 |
| 7 | subMeshRange 边界 | L266 有 `<kMaxSubMeshRanges` 保护 | ❌ 不越界写 |
| 8 | 桶偏移帧间错位 | [L2cOffset]/[CullOffset] 同帧号对比一致 | ❌ 偏移表同步正常 |
| 9 | 原子竞争索引丢失 | InterlockedAdd 硬件原子（slot 唯一） | ❌ 不丢计数 |
| 10 | StartIndex/IndexCount 溢出 | SetBucketDrawArgs 有 segs clamp + null 检查 | ❌ 有保护 |
| 11 | 两段 RingBuffer 错位 | [SegDiag] 两段地址相邻稳定（delta 千字节级） | ❌ 同帧分配 |
| 12 | flatInstances/allInstances 顺序 | L1293/L1294 相邻 push（同序同实体） | ❌ 严格同序 |
| 13 | 首遇快照取错批次 | [FirstSnap] 批次序 batch 171~176 变化但 mat 恒 27 | ❌ 首遇材质稳定 |
| 14 | 实例材质突变（19:23 日志 mat 27→20→2） | [EntDiag] 本次运行 mat 恒 27（19:28） | ❌ 旧运行状态，本次未触发 |
| 15 | PS 桶材质（gBucketMaterialIndex） | [BucketMat] 样本内桶材质稳定（bucket=128 恒 18） | ⚠️ 样本少，未覆盖错乱帧 |

## 三、关键日志矩阵（本轮新增）

| 日志 | 位置 | 观察层 |
|:--|:--|:--|
| `[EntDiag]` | Editor.cpp FrameSync 扁平化 | 实体 name/World/材质/桶段（固定目标每帧） |
| `[FirstSnap]` | Editor.cpp accum 首遇快照 | 多桶实体首遇批次材质（批次序 vs mat） |
| `[L2cOffset]` | Editor.cpp 提交循环 | VS 用 gBucketBase（桶偏移） |
| `[CullOffset]` | InstanceCullingBuffer.cpp dispatch | CS 用偏移表（同帧号对比） |
| `[SegDiag]` | Editor.cpp FrameSync | 两段 RingBuffer 地址（Instance vs InstanceCulling） |
| `[BucketMat]` | Editor.cpp CullParamsL2c | PS 实际用 gBucketMaterialIndex（桶材质） |
| `[OpaqueDraw]` | OpaqueRenderer.cpp | 每桶 VB/IB/实例段地址（渲染绑定） |

## 四、当前结论与剩余疑点

```
CPU 侧全链路已验证正常：
  实体数据（[EntDiag] name/World 恒定）/ 首遇快照 / 桶偏移 / 两段地址
  / 渲染绑定 / 桶材质——全部静态正确

剩余疑点（错乱 = 几何正确 + 世界位置错误）：
  A. 样本盲区：错乱帧未在抽样（节流 60~120 帧 + 抽样桶未覆盖）
  B. GPU 内读取：VS flatInstanceIndex / gAppend 内容（静态正确但需运行时证据）
  C. 桶归属变化瞬间的帧间错位（低频偶发，需错乱帧捕获）

下一步：
  1. 扩大 [BucketMat]/[EntDiag] 采样到错乱帧（观察到错乱时立即看日志）
  2. 或 PIX GPU 捕获（抓 VS 实际读取的 flatInstanceIndex/World）——决定性手段
```

## 五、修复落地（本轮已确认）

| 修复 | 说明 |
|:--|:--|
| dispatch 相机静止守卫移除 | 每帧 dispatch（收益≈0，消除三态过期） |
| cameraMoved Position/Forward 检测 | 消除 ViewProj 矩阵浮点误差误判（静止误判移动） |
| Octree 查询相机静止守卫恢复 | CPU 粗筛静止缓存（大型引擎标准） |
| AppendBuffer 预分配 ×2 | 避免运行时扩容 GPU 同步阻塞（帧率骤降） |
| OnSceneConstructReady 简化 | 移除动态预算 Initialize，仅 MarkDirty（AddEntity 自动中心化/扩容） |
| 线框诊断模式 | OpaqueRenderer SetWireframeMode + PSOFactory 线框变体（验证实体/材质） |

## 六、数据全链路验证（2026-08-09 晚，静态全部正常）

> 用户导出 RenderDoc 捕获数据逐层验证——**CPU/GPU 输入/CS 输出/命令序列全部正常**，错乱根因未能在数据层捕获。

### 6.1 RenderDoc 导出验证

| 数据 | 导出文件 | 验证结果 |
|:--|:--|:--|
| gPlanes（CS 剔除视锥） | 实例剔除剔除参数.csv | ✅ 6 平面法线全部 \|n\|=1.0000、d 合理、相机在视锥内 |
| gIndirectArgs（CS 输出） | indirestArgs 错误/正常.csv | ✅ InstanceCount 无垃圾值、空桶数量接近（40352 vs 40565） |
| gAppend（存活索引表） | InstanceCulling_Append 异常/正常.csv | ✅ 索引最大 552、无越界大值、两帧几乎一致 |
| gInstances（CS 输入） | 描述符1的剔除数据.csv | ✅ radius 全部正常（21.2 左右）；"114 个 radius=0"为解析伪影（每实例 6 行非 5 行） |
| 实例数据（VS 用） | 实例数据 错误/正常.csv | ✅ World 位置正常（City 坐标）、元素数 6961 vs 6101 |
| 命令录制 | 错误帧命令录制2/下一帧命令录制.txt | ✅ 两帧命令序列结构一致（Dispatch 1/Copy 5/ExecuteIndirect 175~181/Draw 24） |

### 6.2 静态核查（RingBuffer/命令同步）

| 检查点 | 结论 |
|:--|:--|
| RingBuffer 段 fence 保护 | ✅ m_currentFence + m_pending 等待（段在 GPU 完成前不被复用） |
| 命令同步 barrier | ✅ DispatchCulling UAV→SRV 对称（规则 10） |
| 运行时扩容 | ✅ 用户确认未发生 |
| 帧资源生命周期 | ✅ Allocate 用 m_currentFence（跨帧安全） |

### 6.3 结论与推测

```
静态验证已达极限（数据/时序/生命周期/同步全部正常）——"计算内容正常但错乱仍存在"。

用户推测（未确认）：可能是【旧状态导致的异常】——
  gIndirectArgs/gAppend/gInstanceData 中的某些段在特定帧保留了
  上一场景/上一帧的旧状态（未清零/未覆盖），ExecuteIndirect 在
  帧间同步窗口读到旧数据 → 部分实例画到错误位置/叠加。
  （示例：gIndirectArgs 的 InstanceCount 残留、gAppend 尾部旧索引、
   RingBuffer 段旧内容在 fence 边界窗口被读）

处理计划：bindless 推进时统一重构实例数据布局（GPUInstanceData 段
  重设计 + 显式清零/版本戳），届时可验证/消除旧状态异常。
```

## 七、防御性代码下放（2026-08-09 定案）

> 排查中推断的防御性修复落地（防止潜在异常扩大），供 bindless 推进时保留。

| 防御 | 位置 | 说明 |
|:--|:--|:--|
| Builder radius 下限保护 | OpaqueRenderItemBuilder.cpp | radius = max(radius, 1e-3)（防 localBounds 异常 → CS expandedRadius=0 → 误剔） |
| gAppend 越界防御 | InstanceCulling.cs.hlsl | gAppend 写入段范围保护（防 CS 写越界） |
| gBucketBase 越界防御 | InstanceCullingBuffer.cpp | GetBucketOffset 越界返回 0（已有） |
