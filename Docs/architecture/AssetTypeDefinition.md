# 资产类型定义 (`AssetType`)

## 定义位置

`Engine/Asset/Definitions/AssetType.h`

```cpp
namespace DX12Engine::Resource {
enum class AssetType : uint8_t {
    Mesh,       // .dxmesh, .obj
    Texture,    // .dds, .png, .jpg
    Material,   // .material
    Skeleton,   // .bone     — 骨骼树 + rest pose（HOD 解析导出）
    Animation,  // .anim     — 骨骼动画剪辑
    Terrain,    // 地形数据
    Scene,      // .scene
    Character,  // .character — 骨架 + 网格 + 材质槽 + 动画剪辑打包
    Prefab,     // .prefab（预留）
    ParticleSystem, // .particle（预留）
    Audio       // 预留
};
}
```

> 枚举顺序说明：`Mesh`/`Texture`/`Material`/`Terrain`/`Scene` 保持原有顺序不变（二进制兼容）；`Skeleton`/`Animation` 为原子资产新增；`Character` 为复合资产新增；`Prefab`/`ParticleSystem`/`Audio` 为预留。详见 `Docs/architecture/CharacterAsset.md`。

## 新增资产类型时需同步修改的部分

### 1. 枚举定义
**文件**: `Engine/Asset/Definitions/AssetType.h`
在枚举末尾添加新类型，注意不要修改已有值的顺序（二进制兼容）。

### 2. 加载逻辑
**文件**: `Engine/Resource/AssetManager/AssetManager.cpp`
`AssetManager::Load()` 中的 `switch (type)` 需添加新分支，创建对应的加载任务。

新增类型对应加载器：
| 类型 | 加载器 | 输出 |
|:-----|:-------|:-----|
| `Skeleton` | `SkeletonLoader`（`.bone`） | `SkeletonHandle` |
| `Animation` | `AnimLoader`（`.anim`） | `ClipHandle` |
| `Character` | `CharacterLoader`（`.character`） | `CharacterHandle` |

`Character` 是复合资产：`CharacterLoader` 先递归加载依赖的原子资产（`.dxmesh` / `.bone` / `.material` / `.anim`），全部就绪后才创建 Handle。依赖收集逻辑见 `Docs/architecture/CharacterAsset.md` §6.2。

### 3. 文件图标
**文件**: `Editor/EditorLib/FileIconProvider.cpp`
两处修改：
- `ExtensionToAssetType()` — 添加扩展名到新类型映射（`.bone` → Skeleton、`.anim` → Animation、`.character` → Character）
- `GetFileIconInfo()` switch 中添加新类型分支，指定颜色和图标

### 4. 场景构造器
**文件**: `Engine/Scene/SceneConstructor.cpp`
如果新类型需要从场景描述文件加载，在 `LoadDefaultScene()` 或 `Construct()` 中添加对应的依赖收集和加载逻辑。

`Character` 以场景组件形式被引用（`character: { asset, startClip }`），`SceneConstructor` 需：
- 依赖收集时把 `.character` 的依赖（`.dxmesh` / `.bone` / `.material` / `.anim`）递归加入；
- 装配时创建 `MeshComponent`（材质槽）+ `SkinnedComponent`（skeletonHandle + currentClip），见 `Docs/architecture/CharacterAsset.md` §七。

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