# BugFix: assimp 6.0.4 FBX 导出器纹理路径崩溃（cannot dereference end map/set iterator）

> 日期：2026-08-01
> 关联：AssetTool FBX 导出（`RobotMerger::ExportAnimationsFBX` / `Merge` 6b）+ assimp FBX 导出器（`assimp-vc143-mtd.dll` v6.0.4）
> 状态：**第三方库 bug，已绕开（FBX 纹理导出暂缓）；社区 PR #6405 已确认修复，待合入**

---

## 现象

AssetTool 导出的 FBX 若在材质中设置了纹理属性（`AI_MATKEY_TEXTURE_DIFFUSE(0)`，外部引用 PNG），导出时崩溃：

```
Microsoft Visual C++ Runtime Library
Debug Assertion Failed!
Program: ...\assimp-vc143-mtd.dll
File: ...\include\xtree
Line: 182
Expression: cannot dereference end map/set iterator
```

之前还会先遇到 `Failed to get texture 0 ... GetTextureCount returned 1`（那是我们自己 `AddProperty` 走错重载的问题，见下"前置问题"）。

## 根因（assimp FBXExporter.cpp 内部 bug）

`FBXExporter.cpp`（v6.0.4）导出材质纹理时，`tpath_by_image` 这个 map **只在处理嵌入式纹理时才会被填充**：

```cpp
// FBXExporter.cpp 1616-1619（位于 if (embedded_texture != nullptr) 分支内）
auto elem = tpath_by_image.find(path);
if (elem == tpath_by_image.end()) {
    tpath_by_image[path] = newPath.str();
}
```

我们使用的是**外部引用纹理**（相对路径，非内嵌），因此 `tpath_by_image` 恒为空。后续导出 Texture 节点时：

```cpp
// FBXExporter.cpp 1738-1747：查找纹理路径
auto tp_elem = tpath_by_image.find(texture_path);   // ← 恒返回 end()
std::string tfile_path = texture_path;
if (tp_elem != tpath_by_image.end()) {
    tfile_path = tp_elem->second;
} else {
    ASSIMP_LOG_WARN(...);                            // ← 只警告，不 return
}
...
tnode.AddChild("FileName", tp_elem->second);          // ← 1769 解引用 end()！
tnode.AddChild("RelativeFilename", tp_elem->second);  // ← 1770 解引用 end()！
```

**关键缺陷**：1740 行已检查 `tp_elem == end()` 并回退到 `tfile_path`，但 1769-1770 行**仍无条件解引用 `tp_elem->second`**。Debug 构建触发 STL 迭代器断言崩溃；Release 构建不崩但会写出垃圾路径。`tfile_path` 变量算好了却没用——这是典型的"检查了 end 却继续解引用"缺陷。

## 社区确认（assimp PR #6405）

GitHub `assimp/assimp` PR #6405（"Feature/pjoe fix fbx export"）的 code review 恰好指出同一问题：

> **1738-1770: Unsafe use of `tp_elem` when texture path is not in `tpath_by_image`**
> You correctly fall back to `texture_path` in `tfile_path` when the lookup fails, but the subsequent `AddChild` calls still use `tp_elem->second` directly. When `tp_elem == tpath_by_image.end()`, this is **undefined behavior and will likely crash**.

官方建议修复：
```diff
- tnode.AddChild("FileName", tp_elem->second);
- tnode.AddChild("RelativeFilename", tp_elem->second);
+ tnode.AddChild("FileName", tfile_path);
+ tnode.AddChild("RelativeFilename", tfile_path);
```

PR 目标为 "Enhanced FBX export to properly resolve texture file paths"，上游正在合入——**属 assimp 6.0.4 未合入的已知缺陷，非我方用法问题**。

## 前置问题（我方，已修复）

首次设置纹理属性时遇到 `Failed to get texture ... GetTextureCount returned 1`：`aiMaterial` 中 `aiString` 有**专用 4 参数重载** `AddProperty(const aiString*, key, type, index)`，我方误传了 `pNumValues=1` 导致走模板二进制路径，`GetTextureCount` 有值但 `GetTexture` 失败。修复：去掉 `pNumValues` 参数。该问题修复后暴露了上述 assimp 库 bug。

## 处置决策（2026-08-01 用户定案）

UKW 贴图本身很少（DX9 时代），**FBX 导出不带纹理**（仅颜色材质），后续可在 Blender 内补充；待 assimp 修复（PR #6405 合入、升级 vcpkg assimp）后再启用纹理导出。

- `CreatePartMaterial`（`RobotMerger.cpp`）：仅颜色材质（Diffuse/Specular/Emissive/Shininess），不设纹理属性
- `XFileMaterial::textureFilename` 保留在解析端，将来启用时直接可用
- **.x 导出不受影响**：`.x`（xof 0302txt）为手写文本格式，`TextureFilename` 直接写文本、不经 assimp，纹理外部引用已正常实现（`WriteXMaterialBlock` + `CopyTextureRelative`，复制源 PNG 到 `{outDir}/textures/`）

## 规避路径（若需 FBX 纹理）

| 路径 | 做法 |
|:--|:--|
| A. 本地打补丁 + 重建 assimp | 改 `FBXExporter.cpp` 1769-1770 行 `tp_elem->second` → `tfile_path`，重建 assimp（vcpkg 或手动） |
| B. 等上游修复后升级 | PR #6405 合入后 `vcpkg upgrade assimp` |
| C. 绕开纹理（当前采用） | 不设纹理属性，仅颜色材质 |
