# WindowFrameResources — 窗口帧资源管理器

> 日期：2026-07-26
> 替代：`ApplicationRenderTargets`

---

## 一、设计意图

`WindowFrameResources` 是引擎 CORE 层组件，**集中管理所有依赖窗口尺寸的 GPU 资源**，
统一处理资源的创建、销毁和 resize 重建。

### 核心原则

```
窗口尺寸相关的 GPU 资源 → WindowFrameResources 统一管理
    └── resize 时阻塞式释放旧资源、创建新资源

尺寸无关的 GPU 资源（阴影贴图、反射探针、采样器）→ 各自的池/管理器
    └── 利用池的 FindMatchingEntry 在参数匹配时复用
```

### 定位

| 属性 | 说明 |
|:-----|:------|
| 层级 | 引擎 CORE（`DX12Engine::Renderer`） |
| 设计目标 | 消除两端（Game/Editor）重复的窗口尺寸资源管理逻辑 |
| 关键行为 | resize 时安全重建全部资源，不经过 `RenderTargetPool` |

---

## 二、管理的资源

### 当前范围

| 资源 | 类型 | 数量 | 说明 |
|:-----|:------|:-----|:------|
| G-buffer Albedo | RT (RTV + SRV) | 1 | 漫反射颜色 |
| G-buffer Normal | RT (RTV + SRV) | 1 | 法线（world space） |
| G-buffer Material | RT (RTV + SRV) | 1 | 材质属性（金属度/粗糙度/…） |
| G-buffer WorldPos | RT (RTV + SRV) | 1 | 世界坐标 |
| SceneColor | RT (RTV + SRV) | 1 | 光照 Pass 输出，供后处理/ImGui |
| DepthStencil | DS (DSV + SRV) | 1 | 深度/模板缓冲 |

### 未来可扩展

| 资源 | 说明 |
|:-----|:------|
| SSAO RT | 窗口尺寸相关，当前由 `AmbientOcclusionManager` 自管 |
| 其他后处理 RT | Bloom、SSR 等依赖窗口尺寸的临时 RT |

---

## 三、接口设计

```cpp
namespace DX12Engine::Renderer {

class WindowFrameResources {
public:
    WindowFrameResources() = default;
    ~WindowFrameResources();

    // 禁止拷贝
    WindowFrameResources(const WindowFrameResources &) = delete;
    WindowFrameResources &operator=(const WindowFrameResources &) = delete;

    /// 初始化：创建所有窗口尺寸资源
    /// @param device      D3D12 设备
    /// @param heaps       描述符堆集合
    /// @param width       窗口宽度
    /// @param height      窗口高度
    /// @param heapTag     描述符堆标签（Game=Default, Editor=EditorViewport）
    void Initialize(
        ID3D12Device *device,
        Resource::DescriptorHeapCollection *heaps,
        uint32_t width, uint32_t height,
        Resource::HeapTag heapTag = Resource::HeapTag::Default);

    /// Resize：安全重建所有资源
    /// 内部顺序：释放旧资源（ComPtr::Reset）→ 分配描述符槽位 → 创建新资源
    void OnResize(uint32_t width, uint32_t height);

    /// 销毁所有资源
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }

    // ── G-buffer ──

    ID3D12Resource *GetGBufferResource(int i) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetGBufferRTV(int i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetGBufferSRV(int i) const;

    // ── SceneColor ──

    ID3D12Resource *GetSceneColorResource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetSceneColorRTV() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSceneColorSRV() const;

    // ── DepthStencil ──

    ID3D12Resource *GetDepthResource() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetDSV() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetDepthSRV() const;

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    void AllocateResources(uint32_t width, uint32_t height);
    void FreeResources();

    // 内部数据结构
    struct {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint32_t rtvSlot = UINT32_MAX;
        uint32_t srvSlot = UINT32_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = {};
    } m_gbuffer[4];

    struct {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint32_t rtvSlot = UINT32_MAX;
        uint32_t srvSlot = UINT32_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = {};
    } m_sceneColor;

    struct {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint32_t dsvSlot = UINT32_MAX;
        uint32_t srvSlot = UINT32_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = {};
    } m_depthStencil;

    ID3D12Device *m_device = nullptr;
    Resource::DescriptorHeapCollection *m_heaps = nullptr;
    Resource::HeapTag m_heapTag = Resource::HeapTag::Default;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_initialized = false;
};

} // namespace DX12Engine::Renderer
```

---

## 四、resize 数据流

```
OnResize(newWidth, newHeight)
  │
  ├─ (1) FreeResources()
  │     ├─ ComPtr::Reset() 释放所有 ID3D12Resource
  │     ├─ heaps->Free(Rtv, slot) 释放 RTV 描述符槽位
  │     ├─ heaps->Free(Dsv, slot) 释放 DSV 描述符槽位
  │     └─ heaps->Free(Texture, slot) 释放 SRV 描述符槽位
  │     （描述符立即归还 FreeList，槽位可被下一轮 Allocate 重用）
  │
  └─ (2) AllocateResources(newWidth, newHeight)
        ├─ heaps->Allocate(Rtv) → 分配 G-buffer×4 + SceneColor 的 RTV 槽位
        ├─ heaps->Allocate(Dsv) → 分配 Depth 的 DSV 槽位
        ├─ heaps->Allocate(Texture) → 分配 G-buffer×4 + SceneColor + Depth 的 SRV 槽位
        ├─ CreateCommittedResource → 创建 G-buffer×4（D3D12_RESOURCE_STATE_COMMON）
        ├─ CreateCommittedResource → 创建 SceneColor（D3D12_RESOURCE_STATE_COMMON）
        ├─ CreateCommittedResource → 创建 Depth（D3D12_RESOURCE_STATE_COMMON）
        ├─ CreateRenderTargetView → 创建 RTV 描述符
        ├─ CreateDepthStencilView → 创建 DSV 描述符
        └─ CreateShaderResourceView → 创建 SRV 描述符
```

**关键特性**：
- 不再需要 `completedFence` 参数 — 旧资源通过 `ComPtr::Reset()` 立即释放
- 描述符槽位通过 `heaps->Free` 立即归还，下一轮 Allocate 可直接复用
- 所有资源初始状态为 `COMMON`，各 System 使用时自行通过 ResourceBarrier 过渡

---

## 五、与两端集成

### Game 端（`GameRenderPipeline`）

```cpp
m_windowResources = std::make_unique<WindowFrameResources>();
m_windowResources->Initialize(device, heaps, w, h, HeapTag::Default);

// resize
m_windowResources->OnResize(newW, newH);
```

### Editor 端（`EditorViewport`）

```cpp
// 替换 EditorViewport 内部的 177 行 RT 自管理代码
m_windowResources = std::make_unique<WindowFrameResources>();
m_windowResources->Initialize(device, heaps, w, h, HeapTag::EditorViewport);

// resize 直接委托
m_windowResources->OnResize(newW, newH);
```

---

## 六、与 RenderTargetPool 的职责边界

| 职责 | `WindowFrameResources` | `RenderTargetPool` |
|:-----|:-----------------------|:-------------------|
| 窗口尺寸 RT | 直接管理，resize 时销毁+创建 | ❌ 不使用 |
| 固定尺寸 RT（阴影贴图、探针） | ❌ 不管理 | 池化管理、复用 |
| 资源生命周期 | `CreateCommittedResource` / `ComPtr::Reset` | `Allocate` / `Free`（延迟释放） |
| 描述符管理 | 通过 `DescriptorHeapCollection` 直接分配/释放 | 通过 `DescriptorHeapCollection` 分配，池内缓存 |

---

## 七、相关文件

- `Engine/Renderer/WindowFrameResources.h` — 头文件
- `Engine/Renderer/WindowFrameResources.cpp` — 实现
- `Engine/Renderer/ApplicationRenderTargets.h/.cpp` — 旧类，将被移除
- `Game/Game/RenderPipeline/GameRenderPipeline.h/.cpp` — Game 端集成
- `Editor/EditorLib/Scene/EditorViewport.h/.cpp` — Editor 端集成
