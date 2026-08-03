# 目录结构定位

## 划分原则

| 层级 | 内容 | 示例 |
|------|------|------|
| **场景内容** (`Scene/`) | 由场景实体决定的数据，与具体渲染管线解耦 | Camera、Light、Terrain |
| **材质系统** (`Material/`) | 渲染器的高层协调者，引用纹理索引，输出 GPU 常量 | MaterialManager、MaterialHandle |
| **系统内置效果** (`Effects/`) | 开关控制的后处理/效果，与场景松耦合 | SSAO |
| **渲染管线** (`Pipeline/`) | 具体的 GPU 渲染 Pass | OpaqueRenderer、ShadowRenderer |
| **构建器** (`RenderItemBuilder/`) | 从 ECS 组件→渲染项的转换 | OpaqueRenderItemBuilder |
| **底层资源** (`Resource/`) | GPU 资源生命周期管理、磁盘资产加载 | GpuResourceManager、DepthStencilPool、TextureManager、AssetLoader |
| **通用工具** (`Resource/Utils/`) | 不依赖引擎模块的纯工具函数 | PathUtils、HashUtils、FileUtils、BitUtils |

## 区别要点

- **场景数据** VS **系统效果**：场景数据由实体决定（哪里有光、哪里是水），系统效果由开关控制，不与具体实体绑定
- **材质系统** VS **纹理系统**：材质是纹理索引的集合+渲染参数，不直接管理 GPU 资源；纹理是磁盘资产，属于底层资源范畴，保留在 `Resource/` 下
- **通用工具** 放在 `Resource/Utils/`（或未来迁到 `Engine/Common/`），等待编辑器模式激活其使用场景

## 当前目录结构

```
Engine/
├── Renderer/
│   ├── Effects/SSAO/          ← 系统内置效果
│   ├── Material/              ← 材质系统（句柄+数据+管理器）
│   ├── Scene/                 ← 场景内容
│   ├── Pipeline/              ← 渲染 Pass
│   ├── RenderItemBuilder/     ← ECS→渲染项转换
│   ├── Core/                  ← 剔除、LOD
│   ├── FrameResources/        ← 帧资源
│   ├── RHI/                   ← 设备抽象层
│   └── Utils/                 ← 工具
└── Resource/
    ├── Manager/               ← 几何体/骨骼管理器
    ├── Texture/               ← 纹理系统（磁盘资产）
    ├── Pool/                  ← 池化资源
    ├── GpuResourceManager     ← 底层 GPU 资源
    ├── AssetLoader/           ← 资产加载
    ├── Geometry/              ← 几何体类型定义
    ├── Struct/                ← 句柄/描述符定义
    └── Utils/                 ← 通用工具（编辑器场景下激活）
```
