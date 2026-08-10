# 水渲染系统架构设计

> 目标：将硬编码的水体创建 (`CreateWater`) 迁移为标准 ECS → Builder → Renderer 模式，
> 同时引入 WaterManager 管理全局波浪模拟和环境贴图共享。

---

## 概述

水渲染系统由四个角色组成：

```
 Scene JSON
     ↓
[SceneLoader] 解析
     ↓
 SceneDescription (含 WaterDesc)
     ↓
[SceneConstructor] 编排
     ├─ AssetManager::LoadBatch 加载 Mesh + Texture
     └─ OnDependenciesLoaded
          ├─ 注册材质到 MaterialManager
          └─ 设置 WaterManager 的环境贴图引用（来自 SkyboxManager）
               ↓
     GeneratorTaskCompleteEvent
               ↓
[SceneConstructSystem] 构造 ECS
     └─ ConstructEntity → WaterComponent
               ↓
          每帧
     [WaterRenderItemBuilder] (PreRender, Worker)
     └─ 扫描 WaterComponent → WaterRenderItem
          ↓
     [WaterRenderSystem] (PostProcess, alwaysRun)
     └─ 消费 TRenderQueue<WaterRenderItem> → WaterRenderer::DrawWater
```

---

## 角色职责

### WaterComponent — ECS 组件（每水体一个）

```cpp
struct WaterComponent {
    Resource::GeometryHandle geometryHandle;   // 水面网格（GridGeometry）
    Resource::MaterialHandle materialHandle;    // 水材质
    uint32_t waveParamIndex;                    // WaterManager 中的波浪参数索引
};
```

- 每个水体实体携带一个 `WaterComponent`
- 材质/几何体来自 SceneConstructor 的异步加载
- `waveParamIndex` 指向 WaterManager 中的波浪参数数组

### WaterManager — 全局水状态管理器（单例）

```cpp
class WaterManager {
public:
    static WaterManager &GetInstance();

    void Initialize();
    void Shutdown();

    // 波浪管理
    uint32_t RegisterWaveParams(const WaveParams &params);
    void UpdateWaveSimulation(float deltaTime);
    const WaveParams &GetWaveParams(uint32_t index) const;

    // 环境贴图（来自 SkyboxManager，水体共享）
    void SetEnvironmentMap(D3D12_GPU_DESCRIPTOR_HANDLE envMapSRV);

    // 读取
    D3D12_GPU_DESCRIPTOR_HANDLE GetEnvironmentMap() const;
    bool HasEnvironmentMap() const;
};

struct WaveParams {
    float amplitude = 0.5f;
    float frequency = 1.0f;
    float speed = 0.5f;
    float direction = 0.0f;       // 风向（弧度）
    DirectX::XMFLOAT2 waveOffset = {0, 0}; // 运行时偏移（每帧累加）
};
```

职责：
- 持有全局波浪数据（所有水体共享相同的波浪模拟，或者每个水体独立注册）
- 持有环境贴图 SRV 引用（来自 `SkyboxManager`，供所有水体在渲染时使用）
- 每帧更新波浪偏移（时间累计）

### WaterRenderItem — 渲染项

```cpp
struct WaterRenderItem {
    Resource::GeometryHandle geometryHandle;
    D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress;   // ObjectConstants
    uint32_t waveParamIndex;                       // WaterManager 中索引
    float depth;                                   // 到相机距离，用于透明排序
};
```

### WaterRenderItemBuilder — 构建器

```
BuildTyped 消费 "water" 桶（桶模式，非 ECS view 遍历——见 RenderPipelineSpecification.md §10.1）:
  m_cache->ForEachBucket("water", ...)
    → 精确视锥筛选
    → LOD 解析 GeometryHandle
    → 填充 WaterRenderItem { geoHandle, worldMatrix, materialIndex, depth }
    → 推入 TRenderQueue<WaterRenderItem>
```

注意：
- Builder **不分配 CB**（沿用 `FrameSync` 统一分配模式，**铁律见 `RenderPipelineSpecification.md` §10.5**）
- `objectCBAddress` 在 FrameSync 回调中从 RingBuffer 分配（`FrameResourceManager::Allocate`），`worldMatrix`/`materialIndex` 由 Builder 填 CPU 侧数据、上传时写入 `ObjectConstants`
- 构建器运行在 Worker 线程（只读 ECS，不修改）
- ❌ 禁止在 `ConstructEntity` 中创建持久 CB 承载 World 矩阵（`WaterObjCB_Persistent` 旁路，见 §11 缺陷 #6）

### WaterRenderSystem — 渲染系统

```
PostProcess 阶段，alwaysRun：
  if m_waterQueue.Empty() return;
  → 按 depth 排序（远到近）
  → for each item:
       waveParams = WaterManager::GetInstance().GetWaveParams(item.waveParamIndex)
       envMapSRV  = WaterManager::GetInstance().GetEnvironmentMap()
       m_waterRenderer->DrawWater(cmd, item.geometryHandle, worldMatrix,
                                  item.objectCBAddress, envMapSRV, waveParams)
```

---

## JSON 描述

```json
{
    "dependencies": {
        "meshes":    { "water_mesh": "Content/Models/water_plane.dxmesh" },
        "textures":  { "water_tex":  "Content/Textures/water1.dds" }
    },
    "materials": {
        "water": {
            "shader": "Water/Standard",
            "params": {
                "baseColor": [0.1, 0.3, 0.5, 0.6],
                "metallic": 0.0,
                "roughness": 0.1,
                "ao": 1.0
            },
            "textures": {
                "baseColor": "water_tex"
            }
        }
    },
    "entities": [
        {
            "name": "WaterBody",
            "components": {
                "transform": { "position": [0, 10, 0], "scale": [1, 1, 1] },
                "mesh": { "geometry": "water_mesh", "material": "water" },
                "water": {
                    "amplitude": 0.5,
                    "frequency": 1.0,
                    "speed": 0.5,
                    "direction": 0.0
                },
                "transparent": null
            }
        }
    ]
}
```

`WaterDesc` 结构体：
```cpp
struct WaterDesc {
    float amplitude = 0.5f;
    float frequency = 1.0f;
    float speed = 0.5f;
    float direction = 0.0f;
};
```

`EntityDesc` 增加：
```cpp
std::optional<WaterDesc> water;
```

---

## 迁移步骤

### 阶段 1：基础设施

| # | 改动 | 涉及文件 |
|--:|:-----|:---------|
| 1 | 创建 `WaterManager.h/.cpp` | 单例，波浪参数注册/更新，环境贴图引用 |
| 2 | 创建 `WaterComponent` | `Engine/ECS/Core/Components/Water.h` |
| 3 | 创建 `WaterDesc` + `from_json` | `SceneDescription.h` |
| 4 | `SceneLoader::ParseEntity` 解析 `"water"` 组件 | `SceneLoader.cpp` |

### 阶段 2：Builder + RenderSystem

| # | 改动 | 涉及文件 |
|--:|:-----|:---------|
| 5 | 创建 `WaterRenderItem` | 独立或扩展自 `TransparentRenderItem` |
| 6 | 创建 `WaterRenderItemBuilder` | h + cpp |
| 7 | `GameWorld::RegisterWaterRenderSystem` 改为消费队列 | `GameWorld_RenderSystems.cpp` |
| 8 | `GameWorld::RegisterBuilderSystems` 注册 BuildWater | `GameWorld_Builder.cpp` |

### 阶段 3：Manager 集成

| # | 改动 | 涉及文件 |
|--:|:-----|:---------|
| 9 | `Game::Initialize` 中初始化 `WaterManager` | `Game.cpp` |
| 10 | `SceneConstructor::OnDependenciesLoaded` 设置环境贴图 | `SceneConstructor.cpp` |
| 11 | `ConstructEntity` 处理 `WaterDesc` → `WaterComponent` | `SceneConstructor.cpp` |

### 阶段 4：清理

| # | 改动 | 涉及文件 |
|--:|:-----|:---------|
| 12 | 注释/删除 `GameWorld::CreateWater()` | `GameWorld_Scene.cpp` |
| 13 | 删除 `m_waterRenderer` 等硬编码成员 | `GameWorld.h` |
| 14 | 移除 `RegisterWaterConstantsCallback` | `GameWorld_Builder.cpp` |
| 15 | `async_test.json` 添加水体实体 | |

---

## 与天空盒管理器的关系

```
SkyboxManager (单例)
  └─ GetCubeSRV() ──────────────────────────┐
                                            ↓
WaterManager (单例)
  ├─ SetEnvironmentMap(cubeSRV) ← 来自 SceneConstructor
  ├─ waveParams[N] (全局波浪数据)
  └─ GetEnvironmentMap() ──┐
                           ↓
WaterRenderSystem → WaterRenderer::DrawWater(cmd, geo, cb, envMapSRV, waveParams)
```

- 环境贴图来自 `SkyboxManager`，由 `SceneConstructor` 在 `OnDependenciesLoaded` 中注入到 `WaterManager`
- `WaterManager` 负责波浪模拟更新（可在 Immediate 回调中累加时间）
- `WaterRenderer` 本身不需要持有状态——所有数据通过 `DrawWater` 参数传入

---

## 开放问题

1. **波浪数据共享 vs 独立**：所有水体共用一组波浪参数，还是每个水体独立注册？（当前方案：独立注册，`RegisterWaveParams` 返回索引）
2. **环境贴图 fallback**：当无天空盒时，水体反射使用纯色还是禁用反射？（建议：纯色 fallback，`WaterManager` 维持 `m_envMapSRV = {}`，`GetEnvironmentMap().ptr == 0` 时着色器走纯色反射）
3. **水面网格生成**：当前 `CreateWater` 在 CPU 侧生成 `GridGeometry`。迁移后，`WaterDesc` 应支持指定网格 key（从 `.dxmesh` 加载）或者内置生成（`"water": { "gridSize": 256, "segments": 64 }`）。建议 v1 要求 `geometry` key，后续再支持内置生成。
