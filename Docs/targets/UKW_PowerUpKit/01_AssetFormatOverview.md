# Ultimate Knight WindomXP PowerUp Kit — 资产格式总览

> 目标：复现该游戏的场景渲染，作为 DX12 异步资源管线改造的目标内容。
> 目录位置：`D:\APP\Ultimate Knight WindomXP PowerUp Kit`

---

## 一、目录结构

```
Ultimate Knight WindomXP PowerUp Kit/
├── Config.ini              # 游戏配置（窗口、按键、阴影/HDR开关等）
├── server.ini              # 联机服务器列表
├── PilotEditor.exe         # 驾驶员编辑器可执行文件
├── 说明.txt                # 汉化说明
│
├── Robo/                   # 机体数据
│   ├── KD-03/              #   KD-03 机体
│   │   ├── Robo.hod        #     机体组装文件（部件列表+变换）
│   │   ├── hangar.hod      #     机库展示组装文件
│   │   ├── Script.spt      #     状态机/角色定义（文本，Shift-JIS）
│   │   ├── Script.spt.a    #     编译后的状态机（二进制/加密）
│   │   ├── Script.ani      #     动画帧序列（文件名头 + HOD 块，1.008 原版明文）
│   │   ├── root.x          #     骨架根节点网格
│   │   ├── Body.x / Body_d.x    # 身体（正常/受损）
│   │   ├── Head.x / Head_.x     # 头部
│   │   ├── arm1/2.x / arm1/2_r.x  # 手臂
│   │   ├── leg1/2/3.x / *_r.x     # 腿部
│   │   ├── Hand.x / Hand_r.x     # 手部
│   │   ├── gun.x / sword.x       # 武器
│   │   ├── Shield.x / Shield_point.x  # 盾牌
│   │   ├── Missile.x / Missile2.x     # 导弹
│   │   ├── Output01~08.x   # 推进器喷口
│   │   ├── Weapon_point*.x # 武器挂点
│   │   ├── Hit.x / HitUp.x # 碰撞盒
│   │   ├── face.png / face2.png / select.png / Select2.png  # UI贴图
│   │   └── charaselect*.sdt     # 角色选择描述
│   ├── KD-04/
│   ├── KD-04_2/
│   ├── KD-04_3/
│   ├── KD-05 ~ KD-08_7_2/ # 多台机体及变体
│   └── senkan01/           # 战舰
│
├── map/                    # 地图场景数据
│   ├── City/               #   城市（外景，16 tile + 8建筑）
│   ├── City2/              #   城市2（外景变体）
│   ├── City3/              #   城市3（简化版，2 tile + 12建筑）
│   ├── City_tac/           #   城市战术版
│   ├── In/                 #   室内场景（5 tile）
│   ├── In2/                #   室内场景2
│   └── moon/               #   月球场景（5 tile + 星空）
│
├── X/                      # 试验/测试场景
│   ├── test01~04.hod       #   测试组装文件
│   ├── earth.x / galaxy.x  #   天球背景
│   ├── hangar/             #   机库场景
│   └── sen*.x              #   线段/箭头辅助
│
├── data/                   # 着色器效果文件（DX9 HLSL）
│   ├── Basic.txt           #   基础渲染（漫反射+高光+雾）
│   ├── Bloom.txt           #   Bloom 特效（高光提取+模糊）
│   ├── Reflection.txt      #   环境反射（立方体贴图）
│   └── ShadowMap.txt       #   阴影映射（4-tap PCF）
│
└── pilot/                  # 驾驶员数据
    └── */pilot.txt          #   对话/参数文本
```

---

## 二、核心文件格式总览

| 格式 | 扩展名 | 性质 | 简要说明 |
|------|--------|------|----------|
| 场景组装脚本 | `Script.spt` | 文本（Shift-JIS） | 地图的装配指令（瓦片、建筑、光照、BGM） |
| 角色状态机 | `Script.spt` | 文本（Shift-JIS） | 机体属性、AI、颜色方案、武器定义 |
| 编译状态机 | `Script.spt.a` | 二进制（加密） | .spt 编译后的二进制形式 |
| 动画 | `Script.ani` | 二进制（文件名头 + HOD 块，1.008 原版；PUK 2.008 为 HD2 块） | 每帧部件局部矩阵 |
| 机体组装 | `.hod` | 二进制（"HOD"） | 部件 .x 文件列表 + 4x4 变换矩阵 |
| 瓦片地图 | `.mpd` | 二进制（"MPD"） | 瓦片格网索引表 |
| 网格 | `.x` | 二进制（"xof 0303bin"） | DirectX X File v3.03，含 Mesh/Normal/UV/Material |
| 贴图 | `.dds` / `.png` / `.bmp` | 图像 | DDS 为主，也有 PNG/BMP |
| 着色器 | `.txt` | 文本（Shift-JIS） | 实际是 DX9 .fx 效果文件 |

---

## 三、场景组装脚本（Map Script.spt）

### 3.1 语法

`Script.spt` 是 Shift-JIS 编码的命令式脚本。`'` 开头为注释。

### 3.2 指令表

| 指令 | 签名 | 说明 |
|------|------|------|
| `LoadMapData` | `(filename)` | 加载瓦片地图布局数据 |
| `LoadHitXFile` | `(filename)` | 加载场景碰撞网格 |
| `LoadWaterXFile` | `(filename)` | 加载水面网格 |
| `LoadSkyXFile` | `(filename, R, G, B)` | 加载天空球，RGB 为雾色 (0~255) |
| `SetLightColor` | `(R, G, B)` | 方向光颜色 |
| `BgmFileName` | `(id, filename.ogg)` | 背景音乐 |
| `LoadBuildingXFile` | `(id, normal.x, normal.x?, dmg.x?, dmg.x?)` | 加载建筑预设（4级损伤状态对应的 .x） |
| `LoadMaterialXFile` | `(filename)` | 瓦片材质定义（通常 = map00.x） |
| `LoadMapXFile` | `(index, mesh.x, hit.x)` | 加载一个瓦片（几何体 + 碰撞体） |
| `MapSetting` | `(tileIndex, xfileNo, waterHeight)` | 设置瓦片索引 → 几何体文件映射 |
| `MapSettingEx` | `(index, xfileNo, count, ...)` | 扩展瓦片映射（count + 瓦片编号数组） |
| `SetBuilding` | `(mapNo, bldNo, bldXNo, hp, x, y, z)` | 在某个瓦片上摆放建筑 |

### 3.3 示例（City 地图）

```
LoadMapData(MapData3.txt);
LoadHitXFile(Hit.x);
LoadWaterXFile(Sea.x);
LoadSkyXFile(Sky.x,194,214,240);
SetLightColor(255,255,255);
BgmFileName(0,fuse.ogg);

' 建筑预设定义（4参数 = 正常, 受损, 破坏, 废墟）
LoadBuildingXFile(0, Bill00.x,Bill00.x,,);
LoadBuildingXFile(1, Bill01.x,Bill01.x,Bill01_d.x,Bill01_d.x);

' 瓦片定义（16个）
LoadMaterialXFile(map00.x);
LoadMapXFile(0, map00.x, map00hit.x);
...

' 瓦片 → 几何体映射
MapSettingEx(0, 0,16, -1,..., -10000);
MapSetting(1, 1, -10000);
...

' 瓦片上的建筑
SetBuilding(0, 0, 1, 1500, 8.207660, 0.537087, 21.932104);
```

### 3.4 瓦片地图网格布局

- `map.mpd` 文件包含瓦片格网索引，格式：`MPD N` + tileCount + 每个瓦片引用的 .x 文件名
- `MapSetting` 将瓦片索引映射到具体的 .x 文件和水位高度
- `-10000` 水位 = 无水面；`-1` = 陆地

### 3.5 地图变体对比

| 地图 | Tile 数 | 建筑种类 | 水面 | 雾色 | 特点 |
|------|---------|---------|------|------|------|
| City | 16 | 8 | 有 | (194,214,240) | 完整城市街景 |
| City2 | 16 | 8 | 有 | (194,214,240) | 与 City 相同布局 |
| City3 | 2 | 12 | 有 | (194,214,240) | 简化版，密集建筑 |
| City_tac | 16 | 8 | 有 | (194,214,240) | 战术版 |
| In | 5 | 0 | 无 | (0,0,0) | 室内，纯黑雾 |
| In2 | 5 | 0 | 无 | (0,0,0) | 室内 |
| moon | 5 | 0 | 无 | (0,0,0) | 月球，galaxy 星空 |

---

## 四、机体组装文件（.hod）

### 4.1 二进制结构（实测逆向）

所有 entry 统一大小，不分根节点/子节点。矩阵存储为 **float（非 double）**，A/B 索引为 **1-based**。

#### 文件头（19 字节）

```
偏移    大小    说明
------  ------  ----------------------------------------
0x00    3B      "HOD" 魔术字
0x03    1B      0x1E（版本/标志）
0x04    4B      标志位
0x08    4B      数量字段
0x0C    4B      00 00 00 01（固定值，标志 extra flag）
0x10    3B      padding（全零，extra flag=1 时存在）
```

根节点名字从偏移 `0x0F` 开始。

#### Entry 结构（328 字节，统一大小）

```
偏移      大小    说明
------    ------  ----------------------------------------
+0x00     256B    文件名（null-terminated，剩余零填充）
+0x100    64B     4×4 变换矩阵（16 floats，行主序）
+0x140    4B      A = 节点索引（1-based）
+0x144    4B      B = 父节点索引（1-based）
```

> **关键发现**：每个 entry 末尾存储的 A/B 实际属于**下一个 entry**。
> 第一个 entry（root）没有存储槽位，隐式为 `A=0, B=1`（无父节点）。

#### 展示规则

| Entry | 实际存储的 A/B | 解码后的 A/B | 说明 |
|:------|:--------------|:-------------|:------|
| 0 (root.x) | 属于 entry 1 | A=0, B=1 | 隐式硬编码（root 等级 0，1 个子节点） |
| 1 (Body_d.x) | 属于 entry 2 | A=1, B=3 | 等级 1（取 entry 0 末尾），3 个子节点 |
| 2 (Body.x) | 属于 entry 3 | A=2, B=5 | 等级 2（取 entry 1 末尾），5 个子节点 |
| N (N≥1) | 属于 entry N+1 | 取 entry N-1 末尾 | — |

#### 命名约定（2026-07-29 修正版）

```
A = 部件等级（数字越小等级越高，0=根节点）
B = 子部件数量（0-10）
```

> ⚠️ **逆向修正**：早期文档将 A/B 误判为"父节点索引 A + 子节点数 B"或"节点索引 A + 父节点索引 B"。经社区文档确认，正确语义为 **A=等级, B=子节点数**。解析算法详见 `HODParser.cpp::BuildHierarchy()`。

#### 层级构建算法说明

文件按**深度优先**顺序存储，层级构建使用栈算法：

```
1. 根节点 (bone[0]) 隐式等级 0，入栈
2. 对于 bone[i] (i≥1):
   a. 其等级 = bone[i-1].rawA（前移机制：前一 entry 的 rawA 属于此 entry）
   b. 弹出栈顶直至栈顶等级 < 当前等级
   c. 若栈顶等级 == 当前等级-1 → 栈顶为父节点
   d. 否则回退到根节点
   e. 当前节点入栈
```

#### 逆向过程的关键 hex 参考

KD-03/Robo.hod 首个 entry（root）在偏移 `0x0F` 处：
```
0x0F: 0x72 'r' ← "root.x" 起始
...
0x14F-0x152: 01 00 00 00 → A=1（属于下一个 entry: Body_d.x）
0x153-0x156: 03 00 00 00 → B=3（属于下一个 entry: Body_d.x）
```

第二个 entry（Body_d.x）在 `0x0F + 328 = 0x15F` 处：
```
0x25F-0x29E: 64B 矩阵（含 1.362389 等位移值）
0x29F-0x2A2: 02 00 00 00 → A=2（属于 Body.x）
0x2A3-0x2A6: 05 00 00 00 → B=5（属于 Body.x）
```

### 4.2 典型 KD-03 部件树

```
root (root.x)              A:0  B:1  单位矩阵
├── Body_d (Body_d.x)      A:1  B:3  位移 (0, 1.362, 0, 1)
│   ├── Body (Body.x)      A:2  B:5  单位矩阵
│   ├── Head (Head.x)      有旋转，10度
│   ├── arm1 (arm1.x)      有旋转/平移
│   ├── arm1_r (arm1_r.x)
│   ├── arm2 (arm2.x)
│   ├── arm2_r (arm2_r.x)
│   ├── leg1 (leg1.x)
│   ├── leg1_r (leg1_r.x)
│   ├── leg2 (leg2.x)
│   ├── leg2_r (leg2_r.x)
│   ├── leg3 (leg3.x)
│   ├── leg3_r (leg3_r.x)
│   ├── Hand (Hand.x)
│   ├── Hand_r (Hand_r.x)
│   ├── Shield (Shield.x)
│   ├── Missile (Missile.x)
│   └── Missile2 (Missile2.x)
├── Hit (Hit.x)
├── HitUp (HitUp.x)
├── root_Fannel (root_Fannel.x)
├── root_ (root_.x)
├── Weapon_point (Weapon_point.x)
└── Weapon_point2 (Weapon_point2.x)
```

每个部件的变换矩阵定义了其在骨架中的初始位置/旋转/缩放。

### 4.3 加密陷阱

HOD 的"加密"设计非常阴险，逆向时容易掉坑：

| 陷阱 | 说明 |
|:-----|:------|
| 名字偏移 | header 看似 19 字节，但不同文件 header 长度可能不同 |
| A/B 前移 | 每个 entry 末尾的 A/B 属于下一个 entry，第一个隐式硬编码 |
| 1-based 索引 | 二进制存的是 1-based，展示时要转 0-based |
| float 非 double | 矩阵是 16 个 float（64 字节），不是 double（128 字节） |
| 统一 entry 大小 | 根节点和子节点都是 328 字节，根节点没有 256 字节名字的特殊待遇 |
| 矩阵与 A/B 顺序 | `name → matrix → A → B`，不是 `name → A → B → matrix` |

### 4.4 其他 .hod 变体

- **Robo.hod** — 战斗模型组装
- **Robo2.hod** — 变体组装（有些机体有）
- **hangar.hod** — 机库展示组装（不同位置/姿态）
- **senkan.hod** — 战舰组装
- **test01~04.hod** — 测试组装

---

## 五、瓦片地图（.mpd）

### 5.1 二进制结构

```
偏移   长度   说明
------ ------ ---------------------
0x00   3B     "MPD" 魔术头
0x03   1B     " " (空格)
0x04   1B     "N" (? 格式版本)
0x05   3B     tileCount (小端 uint24)
0x08   N*64  每个 tile 的 .x 文件名（64B 固定长度）
```

### 5.2 与 MapSetting 的关系

`.mpd` 定义了**瓦片索引**与**.x 文件**的映射表，而 Script.spt 中的 `MapSetting` 定义了**地图网格位置**与**瓦片索引**的映射。两者结合得到完整的地图布局。

---

## 六、网格文件（.x — DirectX X File 3.03 二进制）

### 6.1 文件头

```
xof 0303bin 0032
```

- "xof" — DirectX X File 魔术字
- "0303" — 版本 3.03
- "bin" — 二进制格式
- "0032" — 32-bit 对齐

### 6.2 包含的数据类型

| 模板 | GUID | 说明 |
|------|------|------|
| `Vector` | `3d82ab5e-62cf-11cf-ab39-0020af71e433` | float3 位置/法线 |
| `Mesh` | `3d82ab44-62da-11cf-ab39-0020af71e433` | 网格容器 |
| `MeshFace` | `3d82ab5f-62cf-11cf-ab39-0020af71e433` | 三角面索引 |
| `MeshNormals` | `f6f48243-47da-11d2-8f52-00403394a3` | 法线数据 |
| `MeshTextureCoords` | `f6f48240-47da-11d2-8f52-00403394a3` | UV 坐标 |
| `Coords2d` | `f6f48244-47da-11d2-8f52-00403394a3` | float2 UV |
| `ColorRGBA` | `35ff44e0-6c7c-11cf-8f52-00403394a3` | 颜色（含 Alpha） |
| `ColorRGB` | `d3e16e81-7835-11cf-8f52-00403394a3` | 颜色（无 Alpha） |
| `Material` | `3d82ab4d-62da-11cf-ab39-0020af71e433` | 材质（颜色+光泽度+纹理引用） |
| `TextureFilename` | `a42790e1-7810-11cf-8f52-00403394a3` | 纹理文件名 |

### 6.3 场景文件规模参考

| 文件 | 大小 | 内容 |
|------|------|------|
| Sky.x | 8.8 KB | 天空球（大面片） |
| Road.x | 30.9 KB | 地面/道路 |
| bill01_d.x | 23 KB | 建筑受损状态 |
| bill04_d.x | 29 KB | 建筑受损状态 |
| map*.x | 3~8 KB | 地图瓦片 |
| root.x | 3 KB | 机体的根节点 |
| Body.x | ~15 KB | 机体身体部件 |

---

## 七、着色器（data/*.txt）

详见独立文档 `02_ShadersAndMaterials.md`

---

## 八、与其他系统的关系

### 8.1 异步资源管线（目标）

| 阶段 | 对应资产 | DX12 管线环节 |
|------|---------|--------------|
| 1. 场景解析 | Script.spt → .mpd → .x 列表 | CPU 预处理（System） |
| 2. 网格加载 | .x (xof 0303bin) → Mesh | 异步 Upload Bundle |
| 3. 贴图加载 | .dds/.png → Texture | 异步 Upload Bundle |
| 4. 骨架组装 | .hod → BoneTree + Transforms | ECS 组件构建 |
| 5. 材质绑定 | .x Material → Pass 选择 | MaterialDesc / PSO 创建 |
| 6. 动画 | .ani → BoneKeyframes | 动画 System |
| 7. 渲染 | Script.spt 定义的场景布局 | 各个 RenderSystem |

### 8.2 已知社区工具

| 工具 | 用途 | 来源 |
|------|------|------|
| **WindomTranscoder** | 通用文件加解密（.x / .spt / .ani） | [GitHub - LightningDragon](https://github.com/LightningDragon/WindomTranscoder) |
| **WindomXP File Decoder** | SPT/ANI 文件加解密 | 社区论坛 |
| **Entranscode** | 通用加解密（.x 文件解密为可编辑格式） | Windom Extreme VS 论坛 |
| **ANI Tool V1.03** | ANI 反汇编/编辑（分解/打包 .ani） | 社区论坛 |
| **Tail.dat Helper** | Tail.dat 自动解密为文本 | Mediafire |
| **WindomXP Animations Editor (β0.6)** | HOD 编辑器 + 动画预览 | Mediafire |
| **Mecha Editor for Windom 2.008** | 综合编辑器（ani/hod/script） | Mediafire |
| **HxD Hex Editor** | 十六进制编辑器（手动编辑 ANI/Tail.dat） | 通用 |
| **Deep Exploration 5.7 / 3DSMax 2010 / Maya 2011** | 3D 模型编辑（解密后的 .x 文件） | 商业软件 |

### 8.3 加密机制与批量转换方案

#### 加密算法

所有加密文件均使用 **XOR 对称加密**（同一操作既加密又解密），出自 `WindomTranscoder`（GitHub: LightningDragon/WindomTranscoder）：

```c
#define WINDOM_CIPHER_KEY     0x0B7E7759  // ← 原版 WindomXP PowerUp Kit
#define SEEDMOD_CIPHER_KEY    0x95127634
#define MSVMOD_CIPHER_KEY     0x19870430
#define SEEDMOD205_CIPHER_KEY 0xAC510B91
#define UNITEDMOD_CIPHER_KEY  0x13322366
#define RAIDMOD_CIPHER_KEY    0xEF452301
#define THEEPICOFWAR_CIPHER_KEY 0x33333323
#define UN_EVO_CIPHER_KEY     0x33322166

// XOR 逐 int (4字节) 处理整个文件
void CipherFile(int key, char* path) {
    // ...
    *(int*)(Buffer + index) ^= key;
    // ...
}
```

- **PowerUp Kit 对应 key**: `0x0B7E7759`
- **支持的扩展名**: `.ani`, `.fx`, `.mpd`, `.sdt`, `.hod`
- **注意**: `.x` 和 `.spt` **不在**此 XOR 加密范围内（有单独的处理方式）

#### 各文件的加密状态

| 扩展名 | 加密方式 | 解密后格式 | 说明 |
|--------|---------|-----------|------|
| `.x` | **未加密** | 标准 DirectX XFile v3.03 | PowerUp Kit 已明文，可直接解析 |
| `.hod` | XOR (key=0x0B7E7759) | 二进制（部件名+矩阵） | 需解密后读取 |
| `.ani` | 1.008 原版实测明文（XOR 仅部分版本） | 二进制（文件名头 + HOD/HD2 块序列；PUK 2.008 为 AN2+HD2） | 直接读取 |
| `.mpd` | XOR (key=0x0B7E7759) | 二进制（"MPD" 瓦片索引） | 需解密后读取 |
| `.sdt` | XOR (key=0x0B7E7759) | 二进制（选择描述） | 需解密后读取 |
| `.fx` | XOR (key=0x0B7E7759) | 文本 (HLSL) | 需解密后读取（但 data/*.txt 已明文） |
| `.spt` (文本) | **无加密** | Shift-JIS 文本 | 明文可直接读取 |
| `.spt.a` | 自定义压缩/加密 | Shift-JIS 文本 | 需专用解码器解压 |

#### 批量转换方案

**方案 A：编写命令行工具**（推荐）

已知 XOR key 和算法后，可写一个小工具（C/Python/Powershell）批量转换整个目录：

```
批量解密:
  foreach 文件 in Robo/*/:
    if 扩展名 in (.hod, .ani, .mpd, .sdt):
      XOR 0x0B7E7759 按 4B 块 → 写入原文件
      
批量加密（重新打包）:
  同上，XOR 是对称的
```

**方案 B：GUI 工具文件夹模式**

WindomTranscoder 已有文件夹批量处理功能（勾选扩展名过滤），选择 Robo/ 目录后一键解密全部 .hod/.ani 文件。

**方案 C：社区工具**

工具包内的 `[起动战士XP] 文件加密器.exe` 和 `ウィンダムXP 文件加解密程序.exe` 也支持文件/文件夹操作。

### 8.4 WindomTranscoder 注意事项

从源码分析发现一个**逻辑反转 bug**：

```c
// HasValidExtension 的返回值语义是反的：
//   返回 TRUE  = 扩展名不在合法列表中（"无效"）
//   返回 FALSE = 扩展名在合法列表中（"有效"）
// 而调用方检查 HasValidExtension() == TRUE 才处理文件
// 所以:
//   ☑ 勾选 "Limit By Extensions" → 实际处理的是"不合法"扩展名的文件
//   ☐ 不勾选 → 正确处理所有文件
```

建议使用该工具时**取消勾选** "Limit By Extensions" 复选框，或确认处理后文件大小发生变化来判断是否已解密。

### 8.5 多 Mod 兼容性（覆盖所有版本）

WindomXP 有多个衍生 MOD，各自使用不同的 XOR key：

| MOD 版本 | XOR Key | 说明 |
|----------|---------|------|
| **WindomXP 原版 / PowerUp Kit** | `0x0B7E7759` | 本次的目标版本 |
| SeedMod | `0x95127634` | SEED 高达 MOD |
| MSV_MOD | `0x19870430` | MSV 版 |
| SeedMod 2.0.5 | `0xAC510B91` | SeedMod 升级版 |
| United Mod | `0x13322366` | UNITED 版 |
| Raid Mod | `0xEF452301` | RAID 版 |
| The Epic of War | `0x33333323` | 战争史诗 MOD |
| UN Evo | `0x33322166` | UNITED Evolution |

> **转换策略**: 解析器可以做成 key 可配置的，遇到不同 MOD 的文件只需换 key 即可，解析逻辑完全复用。

### 8.6 离线转换方案总结

```
┌────────────────────────────────────────────────────────┐
│                    整体转换策略                         │
├────────────────────────────────────────────────────────┤
│                                                        │
│  输入资产目录（Robo/ map/ data/）                      │
│       │                                                │
│       ├── .x 文件 → 标准 DirectX 格式，直接解析        │
│       ├── .hod → XOR 解密 → 骨架树 + 变换矩阵         │
│       ├── .ani → XOR 解密 → 骨骼关键帧                │
│       ├── .mpd → XOR 解密 → 瓦片格网                  │
│       ├── .spt → Shift-JIS 文本，直接解析              │
│       └── .dds/.png → 标准图像，直接加载              │
│                                                        │
│  输出 → 项目的自有格式（DxMesh / MaterialDesc / Asset）│
│                                                        │
│  核心: 整个管线零外部依赖，完全集成在资源管理器中      │
│        不同 MOD 版本仅切换 XOR key 即可兼容             │
│                                                        │
└────────────────────────────────────────────────────────┘
```

---

## 九、实际管线映射（2026-07-29 定案）

根据 SubMesh 材质槽系统（`Docs/architecture/rendering/SubMeshMaterialSlots.md`）与实际资产结构，管线映射如下：

### 9.1 网格资产：部件合并

```
.hod 解析 → 部件列表          多个 .x 文件              合并后的 .dxmesh
┌──────────┐                ┌─────────────┐           ┌────────────────────┐
│ Robo.hod │                │ Body.x      │           │ KD-03.dxmesh       │
│          │ → 部件名列表   │ Head.x      │ → 逐个   │ ├─ VB (全部顶点)    │
│          │                │ arm1.x      │   XFile   │ ├─ IB (全部索引)    │
│          │                │ leg1.x      │   Parser  │ ├─ SubMesh[0]=Body │
│          │                │ ...         │           │ ├─ SubMesh[1]=Head │
└──────────┘                └─────────────┘           │ ├─ SubMesh[2]=arm1 │
                                                      │ └─ ...             │
                                                      └────────────────────┘
```

每个部件 = 网格的一个 SubMesh，索引经过 rebase 对齐。详见 `06_SubMeshPipeline.md`。

### 9.2 骨架资产

```
.hod 解析 → 骨架树 JSON          → 引擎 SkeletonManager
  ├─ 部件名 (Body.x, Head.x, ...)    └─ 骨骼层级 + 绑定姿势矩阵
  ├─ 父子关系索引
  └─ TRS 变换矩阵
```

HODParser 已有完整输出，引擎侧需实现 JSON 加载接口。

### 9.3 动画资产（暂缓）

```
.ani 文件 → 未来解析为 AnimationClip → 动画 System
  ├─ 文件名头 "ANIRobo.hod" + 连续 HOD 块（每块 = 一帧部件局部矩阵）
  ├─ 每骨骼位置(float3) + 旋转(quaternion)
  └─ 状态机定义（Script.spt 中）
```

当前不处理，仅存储骨架。

### 9.4 场景资产

```
原方案: MPD → 自动生成场景（放弃）

新方案: 编辑器手动布景 + hod/部件合并生成的 scene.json 片段
  ├─ 合并网格 + 骨架 → 作为单实体导入
  └─ 材质从 .x Material 块提取 → 映射到 PBR 参数
```

### 9.5 整体管线示意图

```
AssetTool (命令行/GUI)
  │
  ├── Robo/*/*.hod + .x  →  "导入机体" 按钮
  │       ↓
  │   Content/Models/<机体名>/
  │     ├── <机体名>.dxmesh      ← 合并网格，含 SubMesh 表
  │     ├── <机体名>.hod.json    ← 骨架数据
  │     └── <机体名>.scene.json  ← 场景片段
  │
  ├── map/*.mpd + .x  →  已放弃自动解析（改为编辑器布景）
  │
  └── data/*.txt      →  着色器参考（人工分析后映射到引擎材质系统）

引擎 (运行时)
  │
  ├── GeometryResourceManager  ← .dxmesh SubMesh 展开
  ├── MaterialManager          ← .x Material → PBR 参数
  ├── SkeletonManager          ← hod.json 骨架加载
  ├── OpaqueRenderItemBuilder  ← SubMesh × materialSlots 展开
  └── OpaqueRenderer           ← 渲染
```

