# 渲染管线状态快照 (2026-08-05)

> 水渲染管线落地状态：AssetManager 虚拟资产管线已实现；水纹理输入问题已解决（2026-08-05 第四轮确认）
> 关联：`Docs/architecture/rendering/RenderPipelineSpecification.md`、
> `Docs/architecture/rendering/ProceduralGeometryPipeline.md`、
> `Docs/todos/archived/remaining_issues.md`、
> `.atomcode.md` 第 24/25 条

---

## 一、已完成

### 1.1 AssetManager 虚拟资产管线（procedural:// URI）

| 组件 | 状态 |
|:--|:--:|
| `AssetManager::LoadProceduralGeometry` | ✅ 解析 `procedural://grid/width/depth/segs` URI → 创建 `GeometryProceduralTask` |
| `AssetManager::Load` 识别 `procedural://` 前缀 | ✅ 路径以 `procedural://` 开头时走程序化加载 |
| `SceneConstructor::LoadScene` 收集 URI | ✅ 从 `dependencies.meshes` 收集程序化 URI 到 `LoadBatch` |
| `SceneConstructor::OnDependenciesLoaded` 提取缓存 | ✅ 从 `AssetManager::GetCache()` 提取 `GeometryHandle` 到 `geoMap` |
| `ConstructEntity` 标准组件组装 | ✅ `MeshComponent` + `RenderSlotComponent` + 子网格兜底 |

### 1.2 程序化网格子网格兜底

`GridGeometry` 没有 `SubMeshInfo` 表，`GetSubMeshInfo` 返回 `nullptr`。`ConstructEntity` 中已添加 fallback：

```
GetSubMeshInfo 返回 nullptr
  → 尝试 GetGeometry<GridGeometry>
  → 成功则手动构造 SubMeshInfo{0, indexCount}
  → 填入 subMeshRanges
```

### 1.3 WaterRenderer 对齐实体渲染器

| 改动 | 说明 |
|:--|:--|
| 移除 `SetWaterTextureSRV` / `m_waterTextureSRV` | 水纹理走材质系统，不再硬编码 |
| 根签名 slot 5 改为 `t0,space2` 纹理堆 | 匹配 `gTextureMaps[]` 无界数组声明 |
| `BeginFrame` 新增 `textureHeapStart` 参数 | 绑定纹理堆描述符表 |
| 根签名纹理堆改为 `UINT_MAX` 无界 | 匹配 `opaque.renderer.json` 的 `rangeCount: -1` |
| 深度缓冲屏障 COMMON → DEPTH_READ → COMMON | 修复 D3D12 #615 错误 |

### 1.4 水实体 JSON 修复

| 修复 | 说明 |
|:--|:--|
| `waterBlocks[]` → 标准实体 | 4 个水块实体（`WaterBlock_0`~`WaterBlock_3`） |
| 缺少 `mesh` 组件 | 补上 `mesh.geometry` 和 `mesh.materials` |
| `persistentId` 十六进制字符串 | 修复为数字 |

---

## 二、已解决：水纹理输入（2026-08-05 确认）

### 2.1 现象（历史背景）

- RenderDoc 中水块渲染无纹理输入
- 正常实体（mapChip/mountain 等）纹理正常
- 水块材质改用 PBR 材质（`mat_0075561b4840`，`road2` 纹理）后，实体渲染器（Opaque）仍无纹理
- 改用 `Sea.dxmesh`（非程序化网格）后，纹理可见但 UV 反

### 2.2 日志确认

```
WaterBlock_3: geometry found in geoMap ✅
WaterBlock_3: GetSubMeshInfo: 0 subMeshes → GridGeometry fallback subMeshRange(0,6144) ✅
WaterBlock_3: material='mat_0075561b4840' handleIdx=5 shaderType=0 ✅
WaterComponent added ✅
```

- 子网格兜底生效 ✅
- 材质注册正常（`handleIdx=5`）✅
- `shaderType=0`（Unknown → fallback opaque）⏸ 需确认

### 2.3 根因与修复（已闭环）

**根因确认**：`shaderType=0`（Unknown）是因为 `MaterialData::name` 被设置为材质 key（`"mat_0075561b4840"`）而非 shader 字符串（`"PBR/Standard"`）。`ParseShaderType` 无法识别 key 前缀，返回 `Unknown`，影响材质槽的纹理索引绑定路径。

**修复点**：
- `Engine/Background/MaterialLoadTask.h:78`：`out.data.name = shader.empty() ? filePath : shader;` —— `.mat` 的 `shader` 字段（`"PBR/Standard"`）正确写入 `MaterialData::name` ✅
- `Engine/Scene/SceneConstructor.cpp:664`：`ShaderType st = md ? ParseShaderType(md->name) : ShaderType::Unknown;` —— 路由键取自 name，水材质不再落到 `Unknown` ✅

### 2.4 待处理

| # | 问题 | 状态 |
|:--|:--|:--:|
| 1 | 水纹理未输入（仅水块，其他实体正常） | ✅ 已解决（2026-08-05） |
| 2 | `MaterialData::name` 应为 shader 字符串而非材质 key | ✅ 已解决（MaterialLoadTask L78 + SceneConstructor L664） |
| 3 | `Sea.dxmesh` UV 反向（旧格式） | P3 未处理 |
| 4 | Game 端透明队列无消费系统 | ⏸ 暂缓 |