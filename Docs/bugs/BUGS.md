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
