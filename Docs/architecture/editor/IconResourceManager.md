# 图标资源管理器 (IconResourceManager) 设计

## 概述

统一管理编辑器图标资源，支持多种来源，在初始化阶段完成 GPU 上传，确保渲染线程安全。

## 职责

- 预加载图标资源到 GPU（初始化阶段，主线程）
- 提供运行时图标查询接口
- 支持多种图标来源
- 可用于资产预览、着色器预览、文件类型图标等

## 图标来源

| 来源 | 类型 | 说明 |
|------|------|------|
| **Icon Font** | 文本字符 | 加载 FontAwesome 等图标字体，合并到 ImGui 字体中，直接用字符显示图标 |
| **纹理图集** | GPU 纹理 | 将所有图标合并到一张大纹理中，用 UV 坐标选取，单个 SRV 描述符 |
| **系统图标** | GPU 纹理 | 通过 SHGetFileInfo 获取 Windows 系统图标，转为纹理缓存 |
| **程序化生成** | GPU 纹理 | 运行时生成简单图标（文件夹、文件类型标记等） |

### Icon Font 方案（推荐）

FontAwesome 是最常用的 ImGui 图标方案，通过 `ImFontConfig::MergeMode` 合并到 ImGui 字体中：

```cpp
// 初始化时加载
ImFontConfig config;
config.MergeMode = true;
io.Fonts->AddFontFromFileTTF("fa-solid-900.ttf", 13.0f, &config, io.Fonts->GetGlyphRangesDefault());
// 使用时
ImGui::Text(ICON_FA_FOLDER "  FolderName");
```

优势：
- 零 D3D12 纹理开销
- 始终可用，无需描述符堆管理
- 矢量缩放不失真
- 丰富的图标库（FontAwesome 免费版 1600+ 图标）

### 纹理图集方案

适用于需要高定制化图标的场景（资产预览缩略图等）：

1. 初始化时加载所有图标到一张图集纹理
2. 创建单个 SRV 描述符（在 ImGui 的描述符堆中分配）
3. 运行时用 UV 坐标选取具体图标
4. ImGui::Image(texture, size, uv0, uv1)

## 接口设计

```cpp
class IconResourceManager {
public:
    static IconResourceManager &GetInstance();

    // 初始化（主线程，帧驱动器之外）
    void Initialize(ID3D12Device *device, Renderer::CommandManager *cmdMgr);

    // 资源类型枚举
    enum class IconType {
        Folder, File, Mesh, Texture, Material, Scene, Json, Unknown
    };

    // 获取图标（用于 AssetManager 等）
    ImTextureID GetIconTexture(IconType type);

    // 获取图标字体字符（用于文本模式）
    const char *GetIconChar(IconType type);

    // 释放
    void Shutdown();
};
```

## 集成路径

1. 创建 `Editor/EditorLib/IconResourceManager.h/.cpp`
2. 在 `Editor::Initialize()` 中调用 `IconResourceManager::GetInstance().Initialize(...)`
3. 在 `EditorAssetManager` 中通过 `IconResourceManager` 获取图标
4. 后续可扩展着色器预览、材质球预览等

## 注意事项

- 所有 GPU 资源创建必须在初始化阶段完成，不能在渲染线程中创建临时命令列表
- 纹理图集的 SRV 描述符必须在 ImGui 的描述符堆中分配（通过 `DebugUIManager::AllocateSrvDescriptor`）
- Icon Font 方案最为简单，优先采用