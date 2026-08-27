# BugFix: 阴影贴图子网格割裂（阴影桶段静态字段错误使用主视口子网格区间）

## 日期

2026-08-15

## 症状

阴影贴图（方向光）中**多材质建筑物的子网格之间割裂分离**——用户观察"建筑物本体的阴影和建筑物顶部的阴影割裂分离了"。相机转动时部分子网格阴影消失（硬切）。

## 根因

**阴影渲染的 ExecuteIndirect 桶段静态字段（IndexCount/StartIndex）错误地来自主视口子网格区间**：

```
主视口 SetBucketDrawArgs（按【子网格区间】预置 IndexCount/StartIndex）→ bucketArgsUp
→ DispatchCulling 阴影分支 CopyBufferRegion(shadowArgsRes, 0, bucketArgsRes, 0, ...)
  （CullingRenderer.cpp 阴影分支 COPY 的是主视口 bucketArgsUp！）
→ shadowIndirectArgs 前部桶段区静态字段 = 主视口子网格区间
→ 阴影 ExecuteIndirect 读桶段静态字段（子网格区间）
→ 多材质建筑物每材质桶各画各的子网格区间 → 中间间隙 → 割裂
```

关键认识：

1. **ShadowRenderItem.subMeshRanges[0] = {0, base->indexCount}（整体区间）只影响 cmdCount（MaxCommandCount），
   不影响绘制区间**——ExecuteIndirect 的绘制参数 100% 来自 shadowIndirectArgs 桶段静态字段。
2. 阴影无材质语义（与主视口不同——主视口需要按材质分桶逐子网格绘制），应每桶整体绘制。
3. 此前单桶化尝试（只写主桶）无效——拆桶非根因，根因是**桶段静态字段的区间来源错误**。

## 修复

### 1. 阴影专用 args 缓冲（整体区间，独立于主视口子网格区间）

| 文件 | 改动 |
|------|------|
| `CullingResourceManager.h/.cpp` | 新增 `m_shadowBucketArgsUp` UPLOAD 缓冲（同 bucketArgsUp 尺寸）；`SetShadowBucketDrawArgs(bucketIndex, indexCount)` 每桶填充**整体区间** `{StartIndex=0, IndexCount=mesh 总索引}`；`GetShadowBucketArgsUp()` getter；ReleaseCullingResources 释放 |
| `CullingLayer.h/.cpp` | 门面转发 `SetShadowBucketDrawArgs`（对齐 SetBucketDrawArgs 先例） |
| `CullingRenderer.cpp` | DispatchCulling 阴影分支 COPY 源：`GetBucketArgsUp()` → **`GetShadowBucketArgsUp()`** |
| `Editor.cpp` FrameSync | ApplyBucketIndices 后遍历阴影渲染项，每桶 `SetShadowBucketDrawArgs(bucketIndex, subMeshRanges[0].indexCount)`（= mesh 总索引） |

### 2. 边界淡出（P0，ShadowSampling.hlsl SampleDirShadow）

UV 距 [0,1] 边界 < 2 texel 时平滑过渡到无阴影（1.0）——消除"相机角度导致某些子网格阴影硬切消失"（正交盒边缘）。`edgeDist < 0` 仍 return 1.0（真正越界），但 fadeRange 内已衰减到接近 1.0，跳变不可见。

## 验证（人工编译运行）

- 阴影贴图子网格不再割裂（多材质建筑物整体入影，本体/顶部阴影连续）
- 相机角度变化时阴影平滑过渡（边界淡出）
- 阴影整体正确性：InstanceData 布局统一（公共 InstanceData.hlsl）+ gBucketBase 每桶 CBV 偏移 + texel 对齐

## 经验教训

- **GPU 驱动间接绘制的绘制参数完全来自间接参数缓冲**（shadowIndirectArgs 桶段静态字段）——渲染项结构里的 subMeshRanges 只影响命令数，不影响绘制区间。修改绘制区间必须改间接参数缓冲的 COPY 源。
- **主视口与阴影的资源隔离**：主视口按材质分桶逐子网格绘制（材质槽语义），阴影无材质语义应整体绘制——两者桶段静态字段必须独立缓冲，不能共享 COPY 源。
- 相关链路：InstanceData 布局统一（公共 hlsl 抽离）→ texel 对齐（PreShadowTranslation）→ gBucketBase 每桶 CBV 偏移（对齐主视口 2026-08-09 方案 A 教训）→ 阴影专用 args 整体区间（本修复）→ 边界淡出。
