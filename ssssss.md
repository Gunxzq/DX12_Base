
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