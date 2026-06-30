# 渲染管线数据访问约束

## 总则

**主渲染资源（主 DSV、主 RTV）在对应的主渲染阶段之外，只允许读取，不允许写入。**

这一约束与 ECS 多线程安全原则一致：
- System 只能读取 ECS 组件，不能写入
- 修改通过临时结构进行，完成后一次性合并

## 类比

| ECS 多线程 | 渲染管线 |
|-----------|---------|
| System 只读 ECS Registry | 非主 Pass 只读主 DSV/RTV |
| 写入临时结构（CommandBuffer） | 写入私有 RT/DSV |
| 帧同步后合并 | 各 Pass 完成后将结果作为 SRV 供下游使用 |

## 适用范围

| 资源 | 可读 | 可写 | 写入者 |
|:----:|:----:|:----:|--------|
| **主深度缓冲** (Main DSV) | 所有 Pass（SSAO、后处理等） | ❌ 仅 Opaque 主渲染阶段 | 只允许场景主渲染写入 |
| **主颜色缓冲** (Main RTV / Back Buffer) | 所有 Pass | ❌ 仅 Opaque 主渲染阶段 | 只允许场景主渲染写入 |
| **私有深度缓冲** (Private DSV) | 创建者 Pass | ✅ 创建者 Pass | SSAO DrawNormals、ShadowMap 等 |
| **私有 RT** (Normal RT / AO RT) | 创建者 Pass + 下游 | ✅ 创建者 Pass | SSAO 等 |

## 具体影响

### SSAO DrawNormals（当前需修复）

❌ 当前问题：DrawNormals 直接写入主深度缓冲（`GetDepthStencilView()`）
✅ 正确做法：使用 SSAO 私有深度缓冲

```
SSAO DrawNormals:
  绑定私有 DSV + 私有 Normal RTV
  场景几何体 → 写入私有 DSV（深度）+ 私有 Normal RTV（法线）
  主 DSV → 作为 SRV 传给 ComputeAO 采样
                             ↑
                         只读，不写
```

### ShadowMap

✅ 已有正确做法：使用私有深度缓冲，不碰主 DSV

### 其他后处理 Pass

- Bloom、Tonemapping 等 → 使用私有 RT，只读主 RTV
- 最终在 UI 阶段或独立 Composite Pass 合并到主 RTV

## 优势

1. **可预测性** — 主渲染阶段开始前，主 DSV/RTV 状态已知（PrePass 清理后即为干净状态）
2. **无副作用** — 非主 Pass 不会意外污染主渲染数据
3. **RDG 友好** — 资源依赖关系清晰：各 Pass 声明"读主DSV"、"写私有DSV"即可自动排序
4. **多线程安全** — 类似 ECS 只读约束，降低数据竞争风险

## 状态

约束已确定，待实现 SSAO 私有深度缓冲。
