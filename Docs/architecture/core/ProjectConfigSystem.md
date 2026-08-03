# 项目配置系统 — `ProjectConfig.ini`

> 日期：2026-07-06
> 关联：`Core/Paths.h`、`AssetFormatStrategy.md`

---

## 问题

引擎核心代码（Core）需要知道 Content、Config、Shaders 等目录的路径。
当前方式是在 CMake POST_BUILD 中把这些目录复制到 exe 输出目录下，导致：

- 改 Content 文件后需要重新构建才能生效
- 无法区分 Game 和 Editor 的配置
- 引擎和游戏内容混杂在一个目录

---

## 方案：项目标识文件 `ProjectConfig.ini`

每个项目根目录下放一个 `ProjectConfig.ini` 文件（空文件或 JSON），引擎启动时从 exe 所在目录向上查找此文件来确定项目根目录。

```
DX12_Base/
├── ProjectConfig.ini                        ← 项目标识，内容示例见下
├── Config/                           ← 项目配置
│   ├── renderer.json
│   ├── resource.json
│   ├── logging_config.json
│   └── frame_resource.json
├── Content/                          ← 游戏资产
│   ├── Meshes/
│   ├── Materials/
│   ├── Textures/
│   ├── Scenes/
│   └── Terrain/
├── Shaders/                          ← HLSL 着色器
│   └── *.hlsl
├── Engine/                           ← 引擎源码（不入打包）
├── Runtime/                          ← 游戏源码
├── build/                            ← 构建输出（不入打包）
└── ...
```

---

## 查找规则

```
DX12_Base.exe 启动时：
  1. 获取 exe 所在目录
  2. 在该目录中查找 ProjectConfig.ini
  3. 没找到 → 向上一级目录继续查找
  4. 找到为止（最多 5 级，防止无限向上遍历）

  找到 ProjectConfig.ini 后：
    项目根 = ProjectConfig.ini 所在目录
    Content根 = 项目根/Content
    Config根  = 项目根/Config
    Shaders根 = 项目根/Shaders
```

### ProjectConfig.ini 文件内容

最简单形式——空文件即可，存在性即标识：

```
# ProjectConfig.ini — 空文件，存在即标识项目根目录
# 也可包含元数据：
# {"name": "DX12_Base", "version": 1}
```

也可为 JSON 格式（未来扩展）：

```json
{
    "name": "DX12_Base",
    "version": 1,
    "contentRoot": "Content",
    "configRoot": "Config",
    "shaderRoot": "Shaders"
}
```

---

## 接口设计

```cpp
// Engine/Core/Paths.h
namespace DX12Engine::Core {

// 初始化：由应用层在 main() 启动时调用
void InitializeProjectPaths();

// 路径查询
std::string GetProjectRoot();
std::string GetContentRoot();
std::string GetConfigRoot();
std::string GetShaderRoot();

// 便捷函数：将相对路径解析为绝对路径
std::string ResolveContentPath(const std::string &relativePath);
std::string ResolveConfigPath(const std::string &relativePath);
std::string ResolveShaderPath(const std::string &relativePath);

}
```

### 实现逻辑

```cpp
void InitializeProjectPaths() {
    namespace fs = std::filesystem;
    fs::path exeDir = GetExeDirectory();

    // 从 exe 目录向上查找 ProjectConfig.ini
    fs::path searchDir = exeDir;
    for (int i = 0; i < 5; ++i) {
        if (fs::exists(searchDir / "ProjectConfig.ini")) {
            s_projectRoot = searchDir.string();
            return;
        }
        searchDir = searchDir.parent_path();
    }

    // 没找到：fallback 到 exe 目录
    s_projectRoot = exeDir.string();
}
```

---

## CMake 变更

去掉 POST_BUILD 复制 Content/Config/Shaders 的命令：

```cmake
# 删除（不再需要）：
# add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy_directory
#     ${CMAKE_SOURCE_DIR}/Content
#     $<TARGET_FILE_DIR:${PROJECT_NAME}>/Content
# )
```

开发时，exe 输出目录下不需要 Content/Config/Shaders，引擎通过 `ProjectConfig.ini` 找到源码目录直接读取。
发布时，手动将需要的 Content/Config/Shaders 复制到 exe 同级目录（`ProjectConfig.ini` 也在此目录）。

---

## 多项目共存示例

```
D:/Projects/
├── MyRPG/
│   ├── ProjectConfig.ini           ← Game 项目
│   ├── Content/
│   │   ├── Meshes/statue.dxmesh
│   │   └── Textures/stone.dds
│   ├── Config/
│   └── build/MyRPG.exe

├── MyFPS/
│   ├── ProjectConfig.ini           ← 另一个 Game 项目
│   ├── Content/
│   ├── Config/
│   └── build/MyFPS.exe

└── DX12Editor/
    ├── ProjectConfig.ini           ← 编辑器项目
    ├── Config/editor.json   ← 编辑器专属配置
    └── build/Editor.exe
```

三个项目共享同一套引擎二进制（`.lib`），但各自的 Content 和 Config 互不干扰。

---

## 实施步骤

| 步骤 | 内容 | 涉及文件 |
|:----:|:-----|:---------|
| 1 | 创建 `ProjectConfig.ini` 文件 | 项目根目录 |
| 2 | 实现 `Core/Paths.h/.cpp`（查找 + 路径解析） | Engine 新文件 |
| 3 | `Bootstrap` 初始化时调用 `InitializeProjectPaths()` | `Bootstrap.cpp` |
| 4 | 替换所有硬编码相对路径 | `AssetLoader`、`ShaderManager` 等 |
| 5 | 删除 CMake POST_BUILD 复制命令 | `CMakeLists.txt` |
