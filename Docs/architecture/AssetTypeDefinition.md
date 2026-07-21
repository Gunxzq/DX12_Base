# 资产类型定义 (`AssetType`)

## 定义位置

`Engine/Asset/Definitions/AssetType.h`

```cpp
namespace DX12Engine::Resource {
enum class AssetType : uint8_t {
    Mesh,       // .dxmesh, .obj
    Texture,    // .dds, .png, .jpg
    Material,   // .material
    Terrain,    // 地形数据
    Scene       // .scene
};
}
```

## 新增资产类型时需同步修改的部分

### 1. 枚举定义
**文件**: `Engine/Asset/Definitions/AssetType.h`
在枚举末尾添加新类型，注意不要修改已有值的顺序（二进制兼容）。

### 2. 加载逻辑
**文件**: `Engine/Resource/AssetManager/AssetManager.cpp`
`AssetManager::Load()` 中的 `switch (type)` 需添加新分支，创建对应的加载任务。

### 3. 文件图标
**文件**: `Editor/EditorLib/FileIconProvider.cpp`
两处修改：
- `ExtensionToAssetType()` — 添加扩展名到新类型映射
- `GetFileIconInfo()` switch 中添加新类型分支，指定颜色和图标

### 4. 场景构造器
**文件**: `Engine/Scene/SceneConstructor.cpp`
如果新类型需要从场景描述文件加载，在 `LoadDefaultScene()` 或 `Construct()` 中添加对应的依赖收集和加载逻辑。

### 5. 资产预览
**文件**: `Editor/EditorLib/Editor.cpp`
双击回调中的扩展名白名单判断（`.dxmesh` / `.obj`）可能需要扩展。

### 6. 文档
**文件**: `Docs/architecture/AssetLoaderImprovement.md`
如果加载逻辑有变更，同步更新该文档。

## 搜索参考

查找所有 `AssetType::` 的引用可定位所有使用点：
```
grep -rn "AssetType::" --include="*.cpp" --include="*.h"
```