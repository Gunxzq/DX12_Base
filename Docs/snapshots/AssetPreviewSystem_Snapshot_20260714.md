# 资产预览系统实现快照 (2026-07-14)

## 当前实现状态

### ✅ 已完成

| 阶段 | 模块 | 状态 |
|------|------|------|
| 第一阶段 | `PreviewContext` 数据结构 | ✅ |
| 第一阶段 | 预览渲染（回调模式替代事件分发） | ✅ |
| 第一阶段 | `RenderTargetPool` + 固定池化槽位 | ✅ |
| 第二阶段 | `FileIconProvider` — iconfont 合并加载 | ✅ |
| 第二阶段 | 资产浏览器文件夹/通用文件图标替换 | ✅ |
| 第二阶段 | `HeapTag::ImGui` 独立描述符堆分区 | ✅ |
| 第三阶段 | `PreviewCacheManager` — DDS 磁盘缓存 | ✅ |
| 第三阶段 | `ThumbnailArray::UploadToSlice` — RGBA8→GPU | ✅ |
| 第三阶段 | `ThumbnailArray::ReadbackSlice` — GPU→CPU | ✅ |
| 第三阶段 | `Editor::CachePreviewThumbnail` — 缩放+DDS+上传 | ✅ |

### 🔄 当前架构

```
Editor 主循环
  ├── FrameDriver::Tick()          ← 渲染（含 PreviewRender 回调）
  ├── CachePreviewThumbnail(id)    ← 渲染完成后触发
  │     ├── ReadbackSlice(0)       ← GPU→CPU (COPY队列)
  │     ├── 双线性缩放 256x256
  │     ├── PreviewCacheManager::WriteDDS  ← 存磁盘
  │     └── ThumbnailArray::UploadToSlice   ← 上传到纹理数组
  └── ...

PreviewManager (固定4槽池)
  ├── AcquirePreview(oldId, type)  ← 复用旧 ID 槽位
  ├── SetRenderCallback(callback)  ← 回调模式渲染
  └── RenderPreviews()             ← 遍历活跃上下文调用回调

DebugUIManager
  └── HeapTag::ImGui 分区 (2048槽 CBV_SRV_UAV)
        └── 内部 DescriptorAllocator (线程安全，mutex保护)

文件图标方案
  ├── 文件夹 → iconfont \uec17 (棕色块)
  ├── .json/.material/.scene → iconfont \ueb91 (灰色块)
  ├── .dds/.png/.jpg → "T" (绿色块，预留缩略图)
  └── .dxmesh/.obj → "M" (蓝色块，预留缩略图)
```

### ⏳ 待办

1. **资产浏览器显示缩略图**：`DrawContentIcons` 中根据文件路径查 `ThumbnailArray` slice，替换 "T"/"M" 文本块
2. **GPU 同步**：`ReadbackSlice` 需要等待 DIRECT 队列渲染完成后再执行 COPY，否则可能读到脏数据
3. **关闭前回写脏 slice**：`Editor::Shutdown` 时遍历 ThumbnailArray，`ReadbackSlice` → `WriteDDS`
4. **场景图标**：iconfont 中已有场景图标，`DrawContentIcons` 补充 `.scene` 分支

### 📁 新增文件

```
Editor/EditorLib/Preview/
  ├── PreviewCacheManager.h        ← 缓存管理器声明
  ├── PreviewCacheManager.cpp      ← DDS编解码、文件读写

Content/Fonts/
  └── iconfont.ttf                 ← 图标字体（文件夹\uec17 + 文件\ueb91）

Content/Cache/Thumbnails/          ← 缩略图缓存目录（运行时创建）
```

### ⚙️ 关键集成点

| 集成点 | 位置 |
|--------|------|
| iconfont 合并到 ImGui 字体 | `DebugUIManager::Initialize()` 末尾 |
| ImGui 堆分区注册 | `Bootstrap.cpp` 描述符初始化区 |
| PreviewCache 初始化 | `Editor::Initialize()` |
| 缩略图缓存触发 | `Editor::CachePreviewThumbnail()` + 主循环 |

### 🔧 已知问题

- `ReadbackSlice` 未与 DIRECT 渲染队列做 fence 同步，可能读到未完成渲染的帧
- `CachePreviewThumbnail` 固定读回 slice 0，需要改用实际分配的 slice
- iconfont 目前仅 2 个图标，纹理/网格用 "T"/"M" 文本块回退，等待缩略图生成后替换
