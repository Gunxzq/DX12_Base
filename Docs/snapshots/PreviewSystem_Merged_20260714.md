# 会话快照 — 资产预览系统全链路（合并版）

## 时间跨度
2026-07-13 ~ 2026-07-14

## 当前分支
`Refactor` | HEAD `a012126`

## 工作目录状态
有未提交改动（大量文件修改 + 新增）

---

## 一、已完成的架构决策

| 决策 | 结论 |
|------|------|
| 预览渲染模式 | 回调模式替代事件分发，直接渲染到 ThumbnailArray slice |
| PreviewManager | 固定 4 槽池，纯数据容器，不沾渲染 |
| PreviewContext 存储 | mesh 句柄、相机参数、RT、SRV（ImGui 堆）、材质颜色、arraySlice |
| CB 上传方式 | FrameScratchAllocator（scratch allocator） |
| SRV 描述符堆 | 在 ImGui 的堆中分配（DebugUIManager::AllocateSrvDescriptor） |
| 渲染阶段 | RenderPhase::PostProcess（在 UI 之前） |
| 缩略图生成 | 预览渲染时直接渲染到 ThumbnailArray slice，无需 GPU 读回 |
| 预览 PBR 渲染器 | 独立 IRenderer 子类，支持纹理/纯色双模式 |
| 资产类型 | 原子资产（Mesh/Material/Texture 三元组）+ 复合资产（Scene/Terrain/ParticleSystem） |
| 加载器 | 按扩展名注册的 IAssetLoader 模式 |

---

## 二、已完成的工作

### Phase 1 — 基础预览系统（07-13）
- PreviewContext 数据结构
- PreviewManager 固定池化槽位
- RenderTargetPool 分配详情 RT
- 预览着色器（preview.hlsl）
- 预览面板 + Orbit 相机
- 资产浏览器双击回调 + 异步加载
- FileIconProvider + iconfont 合并加载
- HeapTag::ImGui 独立描述符堆分区

### Phase 2 — 缩略图系统（07-14）
- **直接渲染到 ThumbnailArray**：代替详情 RT 读回 → 缩放 → 上传的绕路流程
- **`CachePreviewThumbnail` 简化**：只创建 SRV + 注册到资产浏览器
- **资产浏览器显示缩略图**：`DrawContentIcons` 用 `ImGui::Image` 显示
- **旧缩略图保留**：切换 mesh 时不释放旧 slice，ThumbnailArray 作为内存缓存池
- **磁盘缓存**：关闭时打包为 `thumbnails.thumb` 单文件，启动时加载
- **PreviewCacheManager**：DDS 编解码 + 文件读写

### Phase 3 — 基础设施清理
- **Iconfont 修复**：路径使用 ProjectConfig->Root，职责分离到 Bootstrap
- **AssetType 独立**：从 AssetManager.h 移到 `Engine/Asset/Definitions/AssetType.h`
- **FileIconProvider**：纯函数 `switch(AssetType)` 分发颜色/图标
- **HandlePoolBase 清理**：移除 TypeEnum 模板参数、m_types、GpuResourceType、DataSlotType
- **UploadToSlice / ReadbackSlice**：添加 Signal+Wait 修复 GPU 资源竞态
- **项目约束更新**：.atomcode.md 第 19 条（资产体系）、第 20 条（命名空间规范）

### Phase 4 — 预览渲染器重构 + 纹理预览支持（07-14 续）

#### 4.1 缩略图缓存路径映射修复（P2 #1）
- **根因**：`thumbnails.thumb` 保存的是 `cacheKey`（路径哈希），加载时无法反查原始路径
- **修复**：
  - `Editor::Shutdown()`：保存时写入原始 `filePath` 而非哈希
  - `EditorAssetManager::LoadThumbnailPack()`：加载时读取 `filePath` 并调用 `RegisterThumbnail()` 建立 `filePath → slice/SRV` 映射
  - 资产浏览器 `DrawContentIcons()` 通过 `m_thumbnailMap.find(entry.path.string())` 查找缩略图

#### 4.2 预览 PBR 渲染器（PreviewPBRRenderer）
- **位置**：`Editor/EditorLib/Preview/PreviewPBRRenderer.h/.cpp`
- **基类**：继承 `DX12Engine::Renderer::IRenderer`
- **PSO**：单 PSO，POSITION + TEXCOORD 输入布局
- **根签名**：slot 0 CBV b0（WVP + color + hasTexture）+ slot 1 SRV t0（纹理描述符表）+ 静态采样器
- **纯色 mesh 兼容**：stride=12 的 mesh 也能用，`hasTexture=0` 时 shader 不采样 TEXCOORD
- **内置球体**：使用 `GeometryGenerator::CreateSphere(1.0f, 32, 16)`
- **上传方式**：DEFAULT 堆 + UPLOAD 堆 + COPY 队列 + DIRECT 队列屏障（MeshLoadTask 模式）
- **生命周期**：`SetDeviceContext()` → `Initialize()` 两步

#### 4.3 预览着色器（preview.hlsl）
- 输入：`POSITION` + `TEXCOORD`
- CB：`worldViewProj` + `color` + `hasTexture`
- 纹理：`Texture2D gPreviewTexture : register(t0)` + `SamplerState s0`
- 模式切换：`hasTexture != 0` → 采样纹理输出；否则输出纯色 `color`

#### 4.4 PreviewContext 扩展
- 新增 `Resource::TextureHandle previewTexture`
- 新增 `Resource::MaterialHandle previewMaterial`

#### 4.5 Editor 改造
- 移除硬编码的测试 PSO（`m_testRootSig/m_testPSO/m_testVS/m_testPS`）
- 渲染回调使用 `m_previewRenderer.GetPSO()` + `GetRootSignature()`
- 双击回调支持 `.dds/.png/.jpg/.bmp/.tga` 纹理文件
- 纹理句柄生命周期：`TextureMgr->Retain()` / `Release()`
- 纹理预览使用内置球体 mesh（`GetPreviewSphere()`）

---

## 三、关键文件索引

### 引擎 CORE
- `Engine/Asset/Definitions/AssetType.h` — 资产类型枚举（独立）
- `Engine/Renderer/FrameResources/FrameScratchAllocator.h/.cpp` — 临时上传分配器
- `Engine/Common/HandlePoolBase.h` — 通用句柄池（已清理 TypeEnum）
- `Engine/Resource/Core/GpuHandlePool.h/.cpp` — GPU 句柄池（已清理 GpuResourceType）
- `Engine/Core/SharedDataStore/DataSlotPool.h/.cpp` — 数据槽位池（已清理 DataSlotType）
- `Engine/DebugUI/DebugUIManager.cpp` — iconfont 加载（MergeIconFont 方法）

### Editor 新建
- `Editor/EditorLib/Preview/PreviewManager.h/.cpp` — 预览管理器
- `Editor/EditorLib/Preview/PreviewContext.h` — 预览上下文
- `Editor/EditorLib/Preview/PreviewCacheManager.h/.cpp` — DDS 磁盘缓存
- `Editor/EditorLib/Preview/ThumbnailArray.h/.cpp` — 纹理数组管理
- `Editor/EditorLib/Preview/PreviewPBRRenderer.h/.cpp` — 预览 PBR 渲染器
- `Editor/EditorLib/FileIconProvider.h/.cpp` — 文件图标纯函数

### Editor 修改
- `Editor/EditorLib/Editor.h` — 预览系统成员 + 方法
- `Editor/EditorLib/Editor.cpp` — 预览渲染回调（含缩略图渲染）、CachePreviewThumbnail、Shutdown 缓存保存、Initialize 缓存加载
- `Editor/EditorLib/EditorLayout.h/.cpp` — 预览面板 + 缩略图透传
- `Editor/EditorLib/EditorAssetManager.h/.cpp` — 缩略图显示 + 磁盘缓存加载

### 着色器
- `Shaders/preview.hlsl` — 预览着色器（支持纹理采样 + hasTexture 标志）

---

## 四、新文件清单

```
Engine/Asset/Definitions/AssetType.h
Editor/EditorLib/FileIconProvider.h
Editor/EditorLib/FileIconProvider.cpp
Editor/EditorLib/Preview/PreviewPBRRenderer.h
Editor/EditorLib/Preview/PreviewPBRRenderer.cpp
Content/Fonts/iconfont.ttf
Content/Cache/Thumbnails/              (运行时创建)
Docs/architecture/AssetSpecification.md
Docs/architecture/AssetArchitecture.md
Docs/architecture/AssetTypeDefinition.md
```

---

## 五、遗留待办

| # | 任务 | 优先级 | 说明 |
|:-:|:-----|:-------|:------|
| 1 | 材质预览 | P2 | 需要预览属性卡，支持切换程序化模型（球体/立方体/环面等）和调整参数 |
| 2 | 纹理预览兼容性 | P2 | 不是所有纹理格式都支持，需排查并补充支持列表 |
| 3 | 预览属性卡 UI | P2 | ImGui 面板：切换程序化模型、调整材质参数、旋转/缩放控制 |
| 4 | 预制体预览 | P3 | 双击 `.prefab` JSON 组合 Mesh + Material 渲染 |
| 5 | 编辑器资源隔离 | P3 | iconfont 等编辑器资源移到 `Editor/Content/` |
| 6 | 缩略图懒加载策略 | P3 | 启动时异步加载，避免卡顿 |
| 7 | 蒙皮角色 JSON 加载 | P3 | 需 AssetTool 导出 `.dxmesh`（含骨骼） |

---

## 六、已删除的实验性文件
- `Editor/Config/preview_test_cube.json`
- `Editor/EditorLib/Preview/PreviewSystem.h/.cpp`
- `Engine/Resource/VisualAsset/`