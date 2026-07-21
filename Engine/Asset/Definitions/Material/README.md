# Material 格式 — `.mat`

> 日期：2026-07-06
> 关联：`SceneDescription.h`、`DxMeshFormat.h`、`AssetFormatStrategy.md`

---

## 设计目标

| 目标 | 说明 |
|:-----|:------|
| 数据驱动 | 运行时加载 → 填充 Material 结构体，不依赖 C++ 硬编码 |
| 值/纹理 | 支持纯值类型材质（无纹理）和 PBR 全纹理材质 |
| 着色器模型 | 声明使用哪个着色器变体（Standard PBR / Unlit / ClearCoat / ...） |
| 扩展性 | 自定义参数可通过 `extraParams` 传递，着色器按需读取 |

---

## 格式定义

JSON 格式，匹配现有 `SceneDescription` 的依赖引用体系。

### 示例：PBR 全纹理材质

```json
{
    "shader": "PBR/Standard",
    "params": {
        "baseColor":      [0.8, 0.2, 0.2, 1.0],
        "metallic":       0.1,
        "roughness":      0.6,
        "ao":             1.0,
        "emissive":       [0.0, 0.0, 0.0, 1.0],
        "alphaCutoff":    0.0
    },
    "textures": {
        "baseColor":      "Textures/brick_diffuse.dds",
        "normal":         "Textures/brick_normal.dds",
        "metallicRoughness": "Textures/brick_metalRough.dds",
        "ao":             "Textures/brick_ao.dds",
        "emissive":       "Textures/brick_emissive.dds"
    }
}
```

### 示例：值类型材质（无纹理）

```json
{
    "shader": "PBR/Standard",
    "params": {
        "baseColor":      [0.2, 0.5, 0.8, 1.0],
        "metallic":       0.0,
        "roughness":      0.3,
        "ao":             1.0
    }
}
```

### 示例：Unlit 纯色材质

```json
{
    "shader": "Unlit/Color",
    "params": {
        "baseColor":      [1.0, 0.0, 0.0, 1.0]
    }
}
```

### 示例：带自定义参数的材质

```json
{
    "shader": "PBR/ClearCoat",
    "params": {
        "baseColor":        [0.9, 0.9, 0.9, 1.0],
        "metallic":         0.8,
        "roughness":        0.2,
        "clearCoat":        1.0,
        "clearCoatRough":   0.1
    },
    "textures": {
        "baseColor":        "Textures/car_paint.dds",
        "normal":           "Textures/car_normal.dds"
    },
    "extraParams": {
        "clearCoatNormal":  "Textures/car_clearcoat_normal.dds"
    }
}
```

---

## 着色器模型清单

| shader 值 | 说明 |
|:----------|:-----|
| `PBR/Standard` | 标准 PBR（metal/roughness 工作流） |
| `PBR/ClearCoat` | 带清漆层的 PBR |
| `PBR/Skin` | 皮肤着色器（次表面散射近似） |
| `Unlit/Color` | 纯色不受光照 |
| `Unlit/Texture` | 纯纹理不受光照 |
| `Sky/Procedural` | 程序化天空 |
| `Water/Sim` | 水面模拟 |
| `Vegetation/Standard` | 植被（风力动画） |

---

## 参数表

### PBR/Standard

| 参数名 | 类型 | 默认值 | 说明 |
|:-------|:-----|:-------|:-----|
| `baseColor` | float[4] | [1,1,1,1] | 基础色（RGB + Alpha） |
| `metallic` | float | 0.0 | 金属度 [0,1] |
| `roughness` | float | 0.5 | 粗糙度 [0,1] |
| `ao` | float | 1.0 | 环境光遮蔽 [0,1] |
| `emissive` | float[4] | [0,0,0,0] | 自发光色 |
| `alphaCutoff` | float | 0.0 | Alpha 裁剪阈值（0 表示不裁剪） |

### 纹理槽

| 槽名 | 格式 | 说明 |
|:-----|:-----|:------|
| `baseColor` | BC1/BC3/BC7 | 基础色（sRGB） |
| `normal` | BC5/BC7 | 法线贴图（线性） |
| `metallicRoughness` | BC7 | G 通道 roughness，B 通道 metallic |
| `ao` | BC4/BC7 | 环境光遮蔽（线性） |
| `emissive` | BC1/BC3/BC7 | 自发光（sRGB） |

---

## 文件扩展名

`.mat`，放在 `Content/Materials/` 目录下。

---

## 运行时加载流程

### 场景内联材质（推荐，运行时默认路径）

```
SceneLoader 解析场景 JSON
  └─ materials → key: { shader, params, textures }
      └─ SceneConstructor::OnDependenciesLoaded()
          ├─ 纹理加载完成 → 建立 textureKey → SRV 索引映射
          ├─ 遍历内联材质
          │     ├─ 材质纹理 key → 查 SRV 索引
          │     ├─ 构造 MaterialData → MaterialManager::RegisterMaterial
          │     └─ 返回 MaterialHandle
          └─ 材质 GPU buffer COPY 上传 → GeneratorTaskCompleteEvent
```

### 外部 `.mat` 引用（编辑器导出/共享场景时使用）

```
编辑器导出场景 JSON
  └─ materials.stone: { "$ref": "Content/Materials/SharedStone.mat" }
      └─ SceneLoader 解析时 resolve 为内联数据
```

> 运行时默认加载管线仅使用场景 JSON 内联材质定义。`.mat` 文件仅作为编辑器资产浏览和跨场景共享的序列化格式保留，不参与运行时加载路径。
