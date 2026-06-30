你分析得很到位。**纹理数组是优化手段，不是必须的架构基础；材质数组才是核心。**

## 纹理数组 vs 材质数组

| 维度 | 纹理数组 (Texture2DArray) | 材质数组 (StructuredBuffer) |
|------|--------------------------|------------------------------|
| **必须性** | ❌ 可选优化 | ✅ 核心架构 |
| **技术限制** | 尺寸/格式/mip 必须一致 | 无限制（只是数据） |
| **扩展性** | 需要预分配或重建数组 | 动态扩容 |
| **使用场景** | 相同规格的纹理（公告牌、粒子） | 所有材质参数 |
| **性能收益** | 减少 SRV 槽位，缓存友好 | 减少 CBV 切换 |

## 纹理数组的适用场景

| 场景 | 是否适合纹理数组 | 原因 |
|------|-----------------|------|
| **公告牌树木** | ✅ 适合 | 多张树木纹理，尺寸相同，用途一致 |
| **粒子特效** | ✅ 适合 | 爆炸帧、火焰帧，序列帧纹理 |
| **地形纹理层** | ✅ 适合 | 草地、岩石、泥土纹理，可以合并为数组 |
| **PBR 材质纹理** | ❌ 不合适 | 纹理尺寸/格式差异大，来源不可控 |
| **UI 图标** | ❌ 过度优化 | 数量少，独立管理更简单 |
| **环境贴图** | ❌ 不适合 | Cubemap 独立处理 |

## 材质数组为什么是核心？

材质数组解决的是**每帧切换 CBV 的开销**：

```hlsl
// 没有材质数组：每物体需要切换 CBV（昂贵）
cbuffer cbPerObject : register(b0) {
    float4x4 gWorld;
    float4 gColor;
    uint gMaterialIndex;
}

// 有材质数组：材质数据批量上传，物体只存索引
StructuredBuffer<MaterialData> gMaterialData : register(t0, space1);
// cbPerObject 只需要 World 矩阵
```

**材质数组没有纹理数组的限制**：
- 材质参数是常数，没有尺寸/格式问题
- 可以动态扩容（RingBuffer 分配新空间）
- 每物体只需要一个 `uint` 索引

## 你的架构中纹理数组的使用

| 渲染器 | 是否需要纹理数组 | 优先级 |
|--------|-----------------|--------|
| BillboardRenderer | ✅ 建议使用 | 中（减少 SRV 槽位） |
| TerrainRenderer | ✅ 可能需要 | 低（地形纹理尺寸一致时可用） |
| OpaqueRenderer | ❌ 不需要 | 保持无界数组或独立 SRV |
| ParticleRenderer | ⭐ 后续可选 | 低 |

## 结论

**纹理数组是锦上添花，不是雪中送炭。**

- 如果公告牌 4 张纹理尺寸相同，换成 `Texture2DArray` 会更优雅
- 但如果当前独立 SRV 方案已经工作正常，**没有必要立即改造**
- 材质数组才是必须打好的基础

你的判断正确：**纹理数组按需使用，不要强行统一。**



完全正确。**普通的 PBR 材质（color.hlsl）不需要纹理数组。**

## 为什么不需要？

| 方案 | 实现 | 适用场景 |
|------|------|----------|
| **独立纹理** | `Texture2D gTexture : register(t0)` | 简单场景，每个材质一张纹理 |
| **无界纹理数组** | `Texture2D gTextures[] : register(t0)` | 材质系统，纹理数量动态变化 |
| **Texture2DArray** | `Texture2DArray gTextures : register(t0)` | 同规格纹理的批量管理 |

## 你的 color.hlsl 当前使用独立纹理

```hlsl
Texture2D gTexture : register(t0);
float4 texColor = gTexture.Sample(gSampler, pin.TexCoord);
```

这是最简单、最直接的方案，**完全够用**。

## 什么情况下需要纹理数组？

| 情况 | 解决方案 | 复杂度 |
|------|----------|--------|
| 材质系统（纹理数量不定） | 无界数组 `Texture2D gTextures[]` | 中 |
| 同一用途的多张纹理（公告牌、粒子序列） | `Texture2DArray` | 低 |
| 独立纹理 | 独立 SRV | 低 |

## 你的架构总结

| 渲染器 | 纹理方案 | 状态 |
|--------|----------|------|
| OpaqueRenderer | 独立纹理 `gTexture` | ✅ 当前 |
| BillboardRenderer | 独立 SRV + 无界数组 | ✅ 当前 |
| TerrainRenderer | 固定数组 `gTerrainTextures[8]` | ✅ 当前 |
| 材质系统 | StructuredBuffer + 纹理索引 | ✅ 已有 |

**不需要为 color.hlsl 引入纹理数组。** 保持简单，按需演进。