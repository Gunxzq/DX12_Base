# Bug 索引

> BUG 目录下的独立文件使用命名规范 `BugFix_<简短描述>.md`，每条记录一个独立 bug。

---

## Bug 列表

| # | 文件 | 摘要 | 日期 |
|---|------|------|------|
| 1 | [BugFix_DescriptorSlotAllocator_DoubleAlloc](BugFix_DescriptorSlotAllocator_DoubleAlloc.md) | 描述符堆槽位双重分配冲突 | 2026-06-07 |
| 2 | [BugFix_RenderDoc_UnboundedTable_CaptureFail](BugFix_RenderDoc_UnboundedTable_CaptureFail.md) | RenderDoc 无法捕获帧（无界纹理表 + 条件绑定） | 2026-06-27 |
| 3 | [BugFix_ReflectionProbe_ResizeTDR](BugFix_ReflectionProbe_ResizeTDR.md) | 窗口缩放时反射探针系统引起 GPU TDR | 2026-07-01 |
| 4 | [BugFix_LightingPass_RootSigSampler](BugFix_LightingPass_RootSigSampler.md) | 光照 PASS 根签名缺少静态采样器导致 PSO 创建失败 | 2026-07-02 |
| 5 | [BugFix_SSAO_AmbientResourceStateMismatch](BugFix_SSAO_AmbientResourceStateMismatch.md) | SSAO ambient 资源状态错乱导致 ResourceBarrier 不匹配 | 2026-07-03 |
| 6 | [BugFix_Editor_DoubleProcessMessages](BugFix_Editor_DoubleProcessMessages.md) | Editor 主循环双重 ProcessMessages 导致鼠标 Delta 丢失（右键旋转失效） | 2026-07-15 |
| 7 | [BugFix_ImGuizmo_ImGuiAPIVersionMismatch](BugFix_ImGuizmo_ImGuiAPIVersionMismatch.md) | ImGuizmo 与 ImGui 1.92.9 WIP 的 AddPolyline/AddRect 参数顺序不匹配 | 2026-07-22 |
| 8 | [BugFix_BlankTextureProvider_InvalidDescriptorHandle](BugFix_BlankTextureProvider_InvalidDescriptorHandle.md) | 管理器空白纹理缺失导致 LightingRenderer envMapSrv 无效描述符崩溃（#646），引擎 CORE BlankTextureProvider 根治 | 2026-08-07 |
| 9 | [BugFix_ShadowMap_SubMeshSplit_StaticArgs](BugFix_ShadowMap_SubMeshSplit_StaticArgs.md) | 阴影贴图子网格割裂：阴影桶段静态字段错误 COPY 主视口子网格区间 → 阴影专用整体区间 args 缓冲 + 边界淡出 | 2026-08-15 |
