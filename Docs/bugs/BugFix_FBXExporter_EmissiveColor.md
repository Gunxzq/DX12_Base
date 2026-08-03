# BugFix: assimp 6.0.4 FBX 导出器不写 EmissiveColor → X→FBX 链路发光材质丢失

> 日期：2026-08-02
> 关联：AssetTool `ani2anim`（`RobotMerger::ExportAnimationsFBX`）+ assimp 6.0.4 FBX 导出器（`FBXExporter.cpp`）+ vcpkg overlay port
> 状态：**✅ 已修复并验证生效（2026-08-02）**——vcpkg overlay 补丁（`assimp@6.0.4#2`）编译通过，产物 FBX 按材质写入 `EmissiveColor`
> 待办：#62 前置条件之一（蒙皮渲染路径 SubMesh 展开前需保证 X→FBX 材质完整）——已满足

---

## 现象

`ani2anim`（X→FBX 桥，2026-08-01 定案为"进 Blender 的桥"）导出的 FBX，发光材质（如 KD-03 眼部黄绿 Emissive=(0.945,1.0,0.451)）在 Blender 5.2 中导入后 **emissive 恒为 0**，眼部变成普通黄绿材质。

实测产物（`KD03_anim.fbx`，38 部件 → 8 唯一材质）二进制检查：

| 属性 | 出现次数 | 说明 |
|:---|:---|:---|
| `Emissive`（legacy 名） | 8 次 | 每个材质实例都写了，但属性名 Blender 5.2 不认 |
| `EmissiveColor` / `EmissiveFactor` | 各 1 次 | 只在 PropertyTemplate 里，是默认值 (0,0,0)/(1.0)，**非按材质写入** |

## 根因

assimp 6.0.4 `FBXExporter.cpp` 导出材质时有两套属性：

1. **modern 段**（L1458-1506，`Properties70` 标准属性）：写 AmbientColor / DiffuseColor / TransparentColor / ReflectionColor / SpecularColor——**唯独缺少 `AI_MATKEY_COLOR_EMISSIVE` → `"EmissiveColor"` 的映射**。
2. **legacy 段**（L1514-1516）：`m->Get(AI_MATKEY_COLOR_EMISSIVE, c); p.AddP70vector("Emissive", ...)`——属性名是 `"Emissive"`（Vector3D），Blender 5.2 导入器（import_fbx.py L2106-2107）只认 `"EmissiveColor"/Color`。

我方 `CreatePartMaterial`（`RobotMergerUtil.cpp` L73-74）**确实写了** `AI_MATKEY_COLOR_EMISSIVE`（内存模型数据完整），但导出器序列化时没有把它写进 Blender 可识别的属性 → 数据在序列化环节丢失。

> 补充：导入器侧是完整的。`FBXConverter.cpp` L2260 `GetColorPropertyFromMaterial(props, "Emissive")` 内部即读 `"EmissiveColor"` + `"EmissiveFactor"`（`GetColorPropertyFactored`），映射回 `AI_MATKEY_COLOR_EMISSIVE` + `AI_MATKEY_EMISSIVE_INTENSITY`。因此**只需补导出器一侧**，整条环路（X→FBX→Blender→FBX→fbxs2dxmesh）即可闭环。

## 修复方案（定案：vcpkg overlay port 补丁）

**方案对比**：

| 方案 | 说明 | 结论 |
|:---|:---|:---|
| A. vcpkg overlay port 补丁 | 仓库内 `vcpkg-overlays/assimp/` 复制 port + 一个补丁，CMakeLists 加 `VCPKG_OVERLAY_PORTS` | ✅ **采用**（标准、可复现） |
| B. AssetTool 导出后二进制改写 FBX | 解析产物把 legacy `Emissive` 改写/补充为 `EmissiveColor`+`EmissiveFactor` | 脆弱（二进制 FBX 属性结构可定位但易碎） |
| C. Blender 手动设 Emission | DCC 侧手动补材质 | 用户否决（手动设材质麻烦） |

## 已落地改动

| 位置 | 改动 |
|:---|:---|
| `vcpkg-overlays/assimp/vcpkg.json` | 复制原 port（版本 6.0.4） |
| `vcpkg-overlays/assimp/portfile.cmake` | 复制原 port，`PATCHES` 追加 `fbx_emissive_export.patch` |
| `vcpkg-overlays/assimp/build_fixes.patch` | 复制原 port 既有补丁（overlay 引用文件必须齐全） |
| `vcpkg-overlays/assimp/fbx_emissive_export.patch` | **新增**：`FBXExporter.cpp` modern 段（DiffuseColor 之后）补写：<br>```cpp
if (m->Get(AI_MATKEY_COLOR_EMISSIVE, c) == aiReturn_SUCCESS) {
    p.AddP70colorA("EmissiveColor", c.r, c.g, c.b);
    ai_real emissiveIntensity = 1.0;
    m->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissiveIntensity);
    p.AddP70numberA("EmissiveFactor", emissiveIntensity);
}
``` |
| `CMakeLists.txt` | `project()` 前加 `VCPKG_OVERLAY_PORTS`（指向 `vcpkg-overlays/`） |

**验证**：补丁已在临时 git 仓库对 `FBXExporter.cpp`（v6.0.4 原始源码）`git apply --check` 通过并实际应用成功。

## 生效步骤（人工执行，AI 不编译）—— ✅ 已完成 2026-08-02

```
# 1. 必须先用 vcpkg remove assimp 移除已安装的旧版本！
#    原因：vcpkg 对已安装的包判定"已满足"直接跳过，不比较 overlay 版本
#    （即使 overlay port-version 从 1 提升到 2，vcpkg install 也显示 "already installed"）
vcpkg remove assimp
vcpkg install assimp --overlay-ports=D:/project/DX12_Base/vcpkg-overlays
```

> 安装日志确认：`Installing 1/1 assimp:x64-windows@6.0.4#2...` + `Applying patch fbx_emissive_export.patch` → **补丁已应用**。

随后 CMake 重新 configure（读 `VCPKG_OVERLAY_PORTS`）+ 重编 AssetTool。

## 验证记录（2026-08-02，补丁生效后）

重跑 `AssetTool.exe ani2anim C:/Users/32199/Desktop/tool/data/Robo/KD-03/Script.ani <out_dir> KD03`，产物 `KD03_anim.fbx` 二进制检查：

| 属性 | 修复前（基线） | 修复后 | 结论 |
|:---|:---|:---|:---|
| `EmissiveColor` | 1（仅 PropertyTemplate 默认值） | **9**（8 材质实例 + 1 模板） | ✅ 按材质写入 |
| `EmissiveFactor` | 1（仅模板默认值） | **9**（同上） | ✅ 按材质写入 |
| `Emissive`（legacy） | 8 | 8 | 保留（兼容 assimp 自读） |

38 部件 → 8 唯一材质 / 65 clips / 15 骨骼通道均正常，发光数据在 X→FBX 链路不再丢失。

## 遗留说明

- 该补丁只影响 FBX **导出**；`fbxs2dxmesh`（FBX→引擎资产）读 emissive 的链路已在上次会话验证（`ExtractMaterial` 读 `AI_MATKEY_COLOR_EMISSIVE` + `AI_MATKEY_EMISSIVE_INTENSITY`，见 `MaterialSlots_Emissive_Snapshot_20260802.md` §6.2）。
- 既有 `RobotMergerUtil::CreatePartMaterial` 的"emissive 合入 diffuse"兼容 hack（`min(1.0, face+emissive)`）**保留**：它保证自发光部件经旧链路进 Blender 后不黑（可见性兜底），与新补丁（modern 属性真实写入）不冲突；Blender 导入后 emissive 属性真实存在时不再依赖合入颜色。
- 若将来升级 assimp 且上游已修复（写入 `EmissiveColor`），可移除 overlay port；跟踪上游：assimp/assimp FBXExporter.cpp modern 段 emissive 映射。
