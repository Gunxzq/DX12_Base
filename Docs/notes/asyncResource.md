
// 变换组件（位置、旋转、缩放）
struct TransformComponent {
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 rotation = {0.0f, 0.0f, 0.0f}; // 欧拉角或四元数
    DirectX::XMFLOAT3 scale = {1.0f, 1.0f, 1.0f};

    DirectX::XMMATRIX GetMatrix() const {
        DirectX::XMMATRIX world = DirectX::XMMatrixScaling(scale.x, scale.y, scale.z);
        world *= DirectX::XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z);
        world *= DirectX::XMMatrixTranslation(position.x, position.y, position.z);
        return world;
    }
};

// 基础网格组件（所有可渲染实体都有）
struct MeshComponent {
    GeometryHandle geometryHandle;
    MaterialHandle materialHandle;
    TextureHandle textureHandle;

    bool IsValid() const { return geometryHandle.IsValid() && materialHandle.IsValid(); }
};

// LOD 组件
struct LODComponent {
    LODMeshHandle lodMeshHandle;
    float LODDistanceScale = 1.0f;
    float minLODDistance = 0.0f;
    float maxLODDistance = 1000.0f;
};

您这个问题触及了引擎架构的核心：**如何让同一个 System 处理多种资源类型，以及 TaskFlow 模式的灵活性**。

## 核心答案

**System 可以被多次触发，也可以基于条件处理。不需要为每种资源定义新的事件和处理函数。**

## 1. System 的多次触发机制

当前您的 `SystemRegistry` 已经支持：

```cpp
// 同一个 System 可以关注多个事件类型
SystemRegistry::Register({
    .name = "AssetLoadSystem",
    .interestedMessages = {
        TerrainLoadedEvent,
        WaterLoadedEvent,      // 新增水加载完成事件
        TextureLoadedEvent     // 新增纹理加载完成事件
    },
    .func = [this](Registry& reg, const MessageContext& ctx) {
        switch (ctx.messageType) {
        case TerrainLoadedEvent:
            HandleTerrainLoaded(ctx);
            break;
        case WaterLoadedEvent:
            HandleWaterLoaded(ctx);
            break;
        }
    }
});
```

## 2. 基于条件的处理

```cpp
SystemRegistry::Register({
    .name = "ConditionalLoadSystem",
    .alwaysRun = true,  // 每帧执行
    .func = [this](Registry& reg, const MessageContext& ctx) {
        // 条件1：检查是否有待处理的加载请求
        if (m_pendingLoads.empty()) return;
        
        // 条件2：检查相机速度
        float cameraSpeed = m_context->CameraMgr->GetSpeed();
        if (cameraSpeed > FAST_THRESHOLD) return;  // 太快就不加载
        
        // 条件3：检查资源优先级
        for (auto& load : m_pendingLoads) {
            if (load.priority == High && IsReadyToLoad()) {
                ExecuteLoad(load);
            }
        }
    }
});
```

## 3. 大型引擎的更好模式

### 模式 A：统一资产管理器（推荐）

```cpp
class AssetManager {
    struct LoadRequest {
        uint32_t id;
        std::string path;
        ResourceType type;  // Terrain, Water, Texture, Mesh
        std::function<void(LoadResult)> onComplete;
        int priority;
    };
    
    void Update() {
        // 每帧处理 N 个请求（动态预算）
        int budget = CalculateTimeBudget();
        for (int i = 0; i < budget && !m_pending.empty(); ++i) {
            ProcessRequest(m_pending.front());
            m_pending.pop();
        }
    }
    
    void RequestAsset(const std::string& path, ResourceType type, 
                      std::function<void(LoadResult)> callback) {
        m_pending.push({path, type, callback});
    }
};

// 使用
AssetManager::GetInstance().RequestAsset("water1.dds", Texture, 
    [this](LoadResult result) {
        m_waterTextureHandle = result.texture;
        CreateWaterEntity();
    });
```

### 模式 B：每个资源类型独立 System（当前方向）

```cpp
// 地形专用
TerrainLoadSystem (只关注 TerrainLoaded)
WaterLoadSystem (只关注 WaterLoaded)
TextureLoadSystem (只关注 TextureLoaded)
```

**优点**：职责单一，调试简单
**缺点**：代码重复（加载逻辑几乎相同）

### 模式 C：数据驱动的资源表

```cpp
// 配置文件定义资源依赖
{
    "water": {
        "texture": "water1.dds",
        "mesh": "water_grid.obj",
        "material": "water_mat"
    },
    "terrain": {
        "heightmap": "heightmap.png",
        "texture": "ground.dds"
    }
}

// 系统自动解析依赖，并行加载
LoadSystem::LoadResourceSet("water", [](ResourceSet& resources) {
    CreateWaterEntity(resources);
});
```

## 4. 针对您的水资源建议

**当前阶段不需要大改架构**，只需：

```cpp
// 1. 在 GameWorld 添加成员
DX12Engine::Async::WaterReadyStatePtr m_waterReadyState;
DX12Engine::Async::WaterLoadDataPtr m_waterLoadData;

// 2. 添加加载函数
void LoadWaterAsync() {
    // 复用 TerrainLoadTask 的模式
    auto task = WaterLoadTaskFactory::Create(...);
    m_backgroundExecutor->Submit(task);
}

// 3. 复用 TerrainGPUCreateSystem
SystemRegistry::Register({
    .interestedMessages = {TerrainLoadedEvent, WaterLoadedEvent},
    .func = [this](auto& reg, auto& ctx) {
        if (ctx.messageType == WaterLoadedEvent) {
            HandleWaterLoaded(ctx);  // 类似 HandleTerrainLoaded
        }
    }
});
```

## 最佳实践对比

| 方案 | 代码重复 | 灵活性 | 适合场景 |
|------|---------|--------|---------|
| 统一 AssetManager | 低 | 高 | 大量资源类型，需要优先级管理 |
| 独立 System | 中 | 中 | 资源类型少，逻辑差异大 |
| 数据驱动表 | 极低 | 极高 | 复杂项目，需要配置化 |

## 结论

对于当前阶段：**先复用现有模式，为 Water 单独定义事件和加载函数**。代码重复是可接受的，因为：
1. 水资源的加载逻辑可能与地形不同（需要生成动态网格）
2. 保持简单，先跑通
3. 等有 5+ 种资源后再考虑抽象统一管理器

您的方向正确，先跑通水资源加载，再考虑抽象。


19. 设法将地形的异步加载普适化
20. 后台任务执行器，目前还是阻塞模式，后续需要考虑非阻塞模式
Tick 的调用时机：
在 GameWorld::Update() 中每帧调用。这意味着：

后台任务完成的 GpuWorkItem 会在下一帧的 Tick 中被处理

这天然引入了一帧的延迟，是设计选择，不是 bug

任务积压处理：
当前没有限制 m_gpuWorkQueue 的大小。如果突发大量加载请求，队列可能膨胀。可以加一个水位线，超过时让后台线程暂时等待。




您说得完全正确。这是我之前分析中的一个重要遗漏。

## `AssetDataManager` 的三重角色

| 角色 | 用途 | 示例 |
|------|------|------|
| **CPU 资产存储** | 长期持有 CPU 端资源数据 | 顶点数据、解压后的纹理、音频采样 |
| **异步加载缓冲区** | 后台线程与主线程之间传递大对象 | `StoreTypedData` / `GetTypedData` |
| **消息系统扩展** | 事件负载太大时，存句柄而非数据 | 事件只传 `CpuResourceHandle`，数据存在 Manager 中 |

## 与事件系统的配合

```cpp
// 后台线程：加载完成后存储数据
std::vector<Vertex> vertices = ...;
AssetDataManager::GetInstance().StoreTypedData("terrain_1_vertices", 
    std::make_shared<std::vector<Vertex>>(std::move(vertices)));

// 发送事件（只传一个字符串 key）
PostEvent(TerrainLoaded, "terrain_1_vertices");

// 主线程：响应事件时取出数据
auto verticesPtr = AssetDataManager::GetInstance().GetTypedData<std::vector<Vertex>>(key);
// 注册到 GeometryResourceManager...
AssetDataManager::GetInstance().RemoveTypedData(key);
```

**这正是您当前架构中 `TerrainReadyState` + `TerrainLoadData` 应该使用的方式。**

## 当前方案 vs `AssetDataManager` 方案

| 当前 | `AssetDataManager` 方案 |
|------|------------------------|
| `TerrainLoadData` 自定义结构 | `StoreTypedData<TerrainLoadData>` |
| `TerrainReadyState` 自定义结构 | `StoreTypedData<TerrainReadyState>` |
| `m_terrainLoadData` 成员变量 | 通过 key 查找，无成员变量 |
| 手动原子标志 | 复用 `CpuResourceState` |

## 结论

`AssetDataManager` 已经设计为**异步数据传递的基础设施**。您当前的 `TerrainLoadData` 和 `TerrainReadyState` 完全可以被它替代，从而消除 `GameWorld` 中的临时成员变量。

当前的实现是正确的验证方向，后续重构应该把这两个临时结构迁移到 `AssetDataManager` 中管理。







您说得完全正确。我之前的理解有偏差。

## 重新理解 `AssetDataManager`

`AssetDataManager` 的设计定位是：**CPU 端数据的长期存储和管理器**，不仅仅是临时缓存。

```cpp
enum class CpuResourceType : uint8_t { 
    Unknown, Mesh, Texture, Audio, Shader, 
    UploadBuffer, ReadbackBuffer 
};
```

它管理的内容包括：
- **Mesh**：顶点/索引数据（CPU 端副本，可能长期存在）
- **Texture**：解压后的纹理数据（上传前）
- **UploadBuffer**：上传缓冲区的句柄和状态
- **ReadbackBuffer**：回读缓冲

`CpuResourceState` 状态机：
```
Empty → Loading → Ready → PendingRelease → (Reclaim)
```

这是一个完整的生命周期管理。

## `TerrainLoadData` 与 `AssetDataManager` 的关系

| 当前方案 | `AssetDataManager` 方案 |
|---------|------------------------|
| `TerrainLoadData` 持有 `vector<Vertex>` | `AssetDataManager` 通过 `RegisterData` 存储同样的数据 |
| `m_terrainLoadData` 成员变量 | `cpuHandle` 存储在 `AssetDataManager` 中 |
| 手动管理生命周期 | 通过 `CpuResourceState` 自动管理 |

**理论上，`TerrainLoadData` 完全可以被 `AssetDataManager` 替代。**

## 为什么当前绕过了？

因为：
1. **传递路径长**：后台线程需要把几何体数据传给主线程，`AssetDataManager` 的 `StoreTypedData` 可以做到，但需要额外编码
2. **`TerrainLoadData` 更简单直接**：`shared_ptr` + 原子标志，实现快速验证
3. **异步架构验证期**：先跑通流程，再整合到正式系统

## 长期重构方向

```cpp
// 后台线程
CpuResourceHandle handle = assetDataMgr.AllocateSlot(CpuResourceType::Mesh, poolId);
assetDataMgr.RegisterData(handle, data->vertices.data(), data->vertices.size() * sizeof(Vertex));
assetDataMgr.SetState(handle, CpuResourceState::Ready);

// 主线程
if (assetDataMgr.GetState(handle) == CpuResourceState::Ready) {
    void* vertices = assetDataMgr.GetData(handle);
    // 注册到 GeometryResourceManager...
    assetDataMgr.ScheduleRelease(handle, fenceValue);
}
```

这样 `TerrainLoadData` 和 `TerrainReadyState` 都不再需要，统一由 `AssetDataManager` 管理。

## 结论

您是对的。当前 `TerrainLoadData` 和 `TerrainReadyState` 是验证期的临时方案，长期应该融入 `AssetDataManager` 和 `GpuResourceManager` 的现有状态系统。但作为架构验证，当前方式完全可接受——**先跑通，再重构**。