# AssetTool — 资产转换与生成工具（规划）

> 日期：2026-07-06
> 关联：`DxMeshFormat.h`、`MaterialDesc.h`、`GeometryGenerator.h`、`M3dLoader.h`

---

## 定位

一个命令行 exe 工具，两个职责：

```
AssetTool gen    程序化几何体导出
AssetTool import 外部格式转换
```

不依赖运行时引擎代码，只依赖 `Asset/Definitions/` 下的格式定义头文件和 `ThirdParty/` 下的库（assimp、stb_image、nlohmann）。

---

## 子命令

### `gen` — 生成程序化几何体

复用 `GeometryGenerator` 的逻辑，输出 `.dxmesh` 二进制文件。

```
AssetTool gen cube --size 1 -o cube.dxmesh
AssetTool gen cylinder --radius 1 --height 2 --segments 32 -o cylinder.dxmesh
AssetTool gen sphere --radius 1 --segments 16 -o sphere.dxmesh
AssetTool gen grid --width 10 --depth 10 --segments 10 -o grid.dxmesh
AssetTool gen torus --radius 2 --tubeRadius 0.5 -o torus.dxmesh
```

可选参数：`--static`（默认）、`--skinned`（以后扩展）。

### `import` — 外部格式转换

使用 assimp 读取外部模型文件，输出 `.dxmesh` + 可选 `.mat`。

```
AssetTool import statue.fbx -o statue.dxmesh
AssetTool import statue.fbx -o statue.dxmesh --skinned
AssetTool import statue.fbx -o statue.dxmesh --export-material statue.mat
```

转换流程：

```
assimp ReadFile(FBX) → M3dMeshData（现有结构）
  ↓
顶点格式转换（BLENDINDICES → R8G8B8A8_UINT 等）
  ↓
写入 DxMeshHeader + Vertex Data + Index Data
  ↓
（可选）根据 assimp 材质参数生成 .mat JSON
```

---

## 项目位置

```
Engine/Tools/AssetTool/
├── CMakeLists.txt         ← 独立 exe 目标
├── main.cpp               ← 入口（argparse）
├── GenCommand.cpp/.h      ← gen 子命令
├── ImportCommand.cpp/.h   ← import 子命令
├── DxMeshWriter.cpp/.h    ← .dxmesh 写入
└── MatWriter.cpp/.h       ← .mat JSON 写入
```

依赖链：

```
AssetTool
  ├── Asset/Definitions/Mesh/DxMeshFormat.h    ← 格式定义
  ├── Asset/Definitions/Material/MaterialDesc.h ← 格式定义
  ├── ThirdParty/nlohmann/json.hpp            ← JSON 写入
  ├── ThirdParty/assimp                       ← 模型导入
  └── Engine/Renderer/Utils/GeometryGenerator.h ← 程序化几何生成（提取公共部分）
```

---

## 开发路线

| 阶段 | 内容 | 前提 |
|:----:|:-----|:-----|
| P0 | gen 子命令：复用 `GeometryGenerator` 输出 `.dxmesh` | — |
| P1 | import 子命令：assimp → `.dxmesh` | assimp 集成 |
| P2 | import 子命令：assimp 材质 → `.mat` | P1 |
| P3 | 批量转换（场景 JSON → 所有依赖资产） | P1 + P2 |

---

## 不做的功能

- **UI/编辑器集成**：纯命令行，编辑器集成留给未来 Scene Editor
- **运行时热更新**：`AssetTool` 是离线工具，不参与运行时
- **纹理压缩**：DDS 转换由外部工具处理，`AssetTool` 只引用已存在的 `.dds`
