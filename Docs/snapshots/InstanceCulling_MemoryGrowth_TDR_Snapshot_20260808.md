# InstanceCulling RingBuffer 内存增长 + GPU TDR 调查快照 (2026-08-08)

> 现象：`LightManager::UpdateAndUpload` 断言 `mapped != nullptr` 崩溃（栈顶 ucrtbased.dll），**不控制相机也会复现**
> 调查链：log.txt（fmt error ×2 → std::bad_alloc → GPU TDR DEVICE_HUNG → 设备移除 → Map 全失败 → 断言）
> → VS2022 内存曲线：**逐步增长至 ~992MB/1GB 后跌落崩溃** → 定位 InstanceCulling RingBuffer 每帧分配 + 扩容翻倍
> 已落地：fmt 修复、GpuHandlePool::FreeSlot 泄漏修复、方案 B（FrameResourceManager 多段缓冲池）
> **已解决（2026-08-08 追加）**：命令池 Acquire 后 early-return 泄漏、InstanceCulling CullParams CBV 悬垂
> （GpuResourceHandle 无效值 0xFFFFFFFF）、剔除链路 visible=0 根因定位（见 §六）
> 关联：`Docs/architecture/core/Frame.md`（扩容策略权威设计）、`Docs/architecture/rendering/GPU-Drive.md`（L2 剔除）、
> `Docs/snapshots/GPUDriven_Snapshot_20260806.md`（前序快照）、`.atomcode.md` 第 24/25/26 条

---

## 一、症状与关键证据链

### 1.1 崩溃栈（多次一致）

```
ucrtbased.dll!...            ← CRT 内部（断言/堆操作）
DX12_Editor.exe!LightManager::UpdateAndUpload Line 207  ← assert(mapped != nullptr) 触发
DX12_Editor.exe!Editor::Initialize lambda_17 Line 561   ← ExecuteImmediate 每帧回调
FrameDriver::ExecuteImmediate → FrameDriver::Tick → Editor::Run
```

**关键**：`GetResource` 返回非空 `resource`，但 `Map` 后 `mapped == nullptr` → 底层 `ID3D12Resource*` 已失效。
这**不是** LightManager 自身 bug——它是 GPU 设备移除后第一个碰 GPU 的调用点（最后一环）。

### 1.2 log.txt 决定性顺序（build/Bin/x64/Debug/Editor/logs）

```
fmt::v12::format_error × 2        ← 日志格式串占位符与参数不匹配（已修，见 §二）
std::bad_alloc                    ← CPU 内存分配失败（先于 TDR！）
D3D12: Removing Device.
DXGI_ERROR_DEVICE_HUNG (TDR)      ← GPU 挂起 → 设备移除
_com_error
Assertion failed! LightManager.cpp:207  mapped != nullptr
```

### 1.3 VS2022 内存曲线（用户实测）

- 内存**逐步线性爬升**至 ~992MB/1GB 后跌落崩溃
- 崩溃点 `SwapChainManager::Present:204`（仅 backbuffer 取模，不分配——bad_alloc 暴露在帧末任意碰堆点）
- **"不控制相机也崩"** = 时间累积问题，与输入无关（推翻"相机运动触发"假设）

---

## 二、根因定位

### 2.1 内存增长源：InstanceCulling RingBuffer 每帧分配 + 回收失效 → 扩容翻倍

```
每帧 Upload → FrameResourceManager::Allocate("InstanceCulling", 548KB/帧)   ← 6853 实例 × 80B
   ↓ 分配 fence = m_currentFence(= nextFence)
BeginFrame(completedFence) → RingBuffer::Reclaim(completedFence)            ← 依赖 GPU fence 真实推进
   ↓ 若 completedFence 停滞/滞后 → 分配永不回收
16MB 满 → AllocateWithRetry 扩容（×2 无上限）→ 16→32→64→128→256→512→1024MB
   ↓ 且扩容 buffer.Initialize 内部先 Shutdown() 销毁旧资源（RingBuffer.cpp:12）
   ↓   GPU 上一帧 dispatch 仍引用旧 buffer 段 → 悬垂 → DEVICE_HUNG（TDR 次生）
→ 992MB ≈ 扩容到 1GB 前分配失败 → std::bad_alloc
```

### 2.2 三个独立 bug（均已落地修复）

| # | Bug | 修复 |
|:--|:--|:--|
| A | `ApplyTabState:1299` fmt 格式串 5 占位符 / 4 参数（缺 lights）→ `fmt::v12::format_error` | 补 `snap.lightDescs.size()` |
| B | `GpuHandlePool::FreeSlot` 用 `Validate()`（拒绝 PendingRelease）→ Update 释放路径槽位永不回收、dataPtr 悬垂 → 句柄池泄漏 | 改为 generation 匹配 + 非 Empty 校验，允许回收 PendingRelease |
| C | `AllocateWithRetry` 扩容：×2 无上限 + `Initialize` 内部销毁 GPU 在用旧资源 → 1GB 内存 + TDR | **方案 B**：多段缓冲池（§三） |

---

## 三、方案 B 落地：FrameResourceManager 多段缓冲池（2026-08-08）

> 方案 C（虚拟内存映射式）评估：**不可行**——D3D12 `CreateReservedResource` 仅支持纹理（TEXTURE2D/3D），Buffer 不能预留虚拟地址 + 按需提交物理内存。故执行方案 B（对齐 `Frame.md` 策略 1 Unreal 风格）。

### 3.1 改动（`FrameResourceManager.h/.cpp`）

```cpp
struct RingBufferEntry {
    std::string name;
    uint32_t alignment = 256;
    struct Segment {
        RingBuffer buffer;
        uint64_t lastFence = 0;   // 本段最后一次分配的 fence（回收判据）
    };
    std::vector<Segment> segments; // 段 0 = 配置初始段；扩容追加
    uint32_t currentSegment = 0;   // 当前分配段（SRV 段偏移依赖单段连续性）
};
```

| 函数 | 行为 |
|:--|:--|
| `Allocate` | 优先当前段 → 满则 `CreateSegment` 新建段 |
| `CreateSegment` | **1.5x 线性增长 + 256MB 硬上限**（Frame.md CalculateNewSize 同款）；旧段保留不销毁 |
| `BeginFrame` | 每段 `Reclaim(completedFence)`；**旧段延迟回收**：非当前段且 `lastFence <= completedFence` → 才 Shutdown 移除 |
| `GetBufferResource` | 返回当前段资源（SRV 段偏移 = GPU地址 − 当前段基址，InstanceCullingBuffer 消费） |

### 3.2 配套配置

- `Editor/Game Config/frame_resource.json`：`InstanceCulling` 条目 16MB → **64MB**（67108864）

---

## 四、已落地代码改动汇总（2026-08-08）

| 文件 | 改动 |
|:--|:--|
| `Editor/EditorLib/Scene/EditorSceneManager.cpp` | fmt 修复（1299 补参数）；场景加载去掉一次性 Upload（改每帧） |
| `Editor/EditorLib/Core/Editor.cpp` | 每帧 `Upload(FrameResourceManager*)` + 帧末 `EndFrame(fence)`；诊断断言（CullDiag） |
| `Engine/Renderer/Core/InstanceCullingBuffer.h/.cpp` | Upload 迁帧资源管理器（删 DEFAULT/COPY/GpuWorkItem 异步链）；临时 SRV 槽位；CreateUAVs 重建防泄漏 |
| `Engine/Renderer/FrameResources/FrameResourceManager.h/.cpp` | **多段缓冲池（方案 B）**：1.5x 增长 + 256MB 上限 + 旧段延迟回收 |
| `Engine/Renderer/FrameResources/RingBuffer.h` | `GetResource()`（SRV 段偏移用） |
| `Engine/Resource/Core/GpuHandlePool.cpp` | FreeSlot 允许回收 PendingRelease（句柄池泄漏修复） |
| `Editor/Config/frame_resource.json` / `Game/Config/frame_resource.json` | `InstanceCulling` 64MB |

---

## 五、遗留问题 → 已解决情况（2026-08-08 追加）

方案 B 落地后仍复现，经逐项排查后**全部闭环**：

1. ~~`completedFence` 推进疑点（头号嫌疑）~~ **已排除**：`CommandManager::EndFrame`（CommandManager.cpp:87-89）虽用 `GetCurrentSequence()`（不递增）signal，但 FrameDriver::Tick 每帧先 `GetNextFence()`（fetch_add 递增，FrameDriver.cpp:146），EndFrame 实际 signal 的是递增后序号 → GPU fence 正常推进；且**扩容日志从未出现** = RingBuffer 64MB 从未耗尽 = 回收正常 → **RingBuffer 扩容与崩溃无直接关联**。
2. ~~InstanceCulling 剔除链路异常（visible=0）~~ **已定位并修复**（§六）：`visible=0` 不是"全部被剔除"，而是 **dispatch 从未执行**——`m_cullParamsUp` 默认构造 `{index=0,gen=0}` 被旧 `IsValid()`（`index != 0x3FFFFF`）误判为有效 → `CreateCullingPipeline` 跳过 `CreateBuffer` → dispatch 绑定 cbAddr=0 悬垂 CBV → 每帧跳帧。
3. ~~VS2022 内存曲线复查~~ **已确认**：无扩容日志 = RingBuffer 无辜；内存增长实为命令池泄漏（§六）所致。
4. ~~其他每帧累积物~~ **已修复**：`Editor.cpp` 四系统 + `AnimationViewportPanel.cpp` 的 Acquire 后 early-return 泄漏（API_Constraints.md 附录 §1 规则被违反），修复后命令池不再每帧 +1。

**下一步**：① 人工编译验证（项目规则 AI 不编译）；② 复核剔除 visible 计数随相机实时变化（已确认 85%→10%→74% 波动）。

---

## 六、2026-08-08 追加修复：剔除链路根因（GpuResourceHandle 无效值）与命令池泄漏

### 6.1 命令池 Acquire 后 early-return 泄漏（内存增长真正来源）

**根因**：`Editor.cpp` 的 `EditorOpaqueRenderSystem`（rtvs/dsvHandle 判空）、`EditorLightingRenderSystem`（sceneColorRes）、`EditorWaterRenderSystem`（sceneColorRes/sceneRTV）、`AnimationViewportPanel.cpp`（rtRes/rtvHandle）均在 `AcquireCommandListHandle` **之后** early-return → cmdH/allocH 未 Submit 未 Release → CommandListPool/CommandAllocatorPool 每帧 +1 → 池无限增长 → 内存线性爬升至 ~992MB → `std::bad_alloc`（崩溃点随机）。

**修复**：资源可用性检查全部移到 Acquire **之前**；新增 `ReleaseCullingResources()` 统一释放；`.atomcode.md` 新增规则 26（early-return 必须发生在 Acquire 之前）。

### 6.2 InstanceCulling CullParams CBV 悬垂（GBV #961 → TDR 次生）

**根因**：`GpuResourceHandle` 位域默认零初始化 `{index=0, generation=0}`，而 `IsValid()` 只查 `index != 0x3FFFFF` → **未分配的默认句柄被误判为有效** → `CreateCullingPipeline` 中 `if (!m_cullParamsUp.IsValid())` 跳过 `CreateBuffer` → `DispatchCulling` 的 `GetResource(m_cullParamsUp)` 返回 nullptr → `cbAddr=0` → 绑定地址 0 的 CBV → GBV #961 `root CBV 越界 (Bytes available: 0, Resource: <deleted>)` → Release 模式 DEVICE_HUNG/TDR。

**修复**（`Engine/Resource/Core/GpuHandlePool.h`）：
```cpp
struct GpuResourceHandle {
    uint32_t index : 22 = 0x3FFFFF;      // 默认全 1（0xFFFFFFFF 无效值）
    uint32_t generation : 10 = 0x3FF;    // 默认全 1
    static constexpr GpuResourceHandle Invalid() { return {0x3FFFFF, 0x3FF}; } // = 0xFFFFFFFF
    bool IsValid() const { return static_cast<uint32_t>(*this) != 0xFFFFFFFF; }
};
```
对齐 SamplerHandle `slot=UINT32_MAX` 约定；`InstanceCullingBuffer.h` 5 个成员显式 `= Invalid()` 防御。

### 6.3 验证结果（engine.log 12:52-12:53）

| 指标 | 修复前 | 修复后 |
|:--|:--|:--|
| `CullParams buffer allocated` | `valid=true handle=0x00000000`（假有效） | `valid=true handle=0x00401E56 (index=7766, gen=1)` |
| `CullParams CBV invalid (cbAddr=0)` | 89 次/每帧 | **0 次** |
| `[Verify] visible` | 恒 `0/6853 (0.0%)` | `5833→1294→5072→1559`（随相机实时波动） |
| 帧率 | 低（dispatch 空转 + 每帧错误日志） | **48-57 FPS**（剔除真实生效） |
