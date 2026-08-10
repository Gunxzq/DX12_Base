# BugFix: InstanceCulling CullParams CBV 悬垂 → GBV #961 / 剔除从未执行

## 日期

2026-08-08

## 症状

编辑器加载 City 场景后：

1. **GPU-BASED VALIDATION 报 #961**（修复防御前）：
```
D3D12 ERROR: GPU-BASED VALIDATION: Dispatch, Root descriptor access out of bounds (results undefined):
  Resource: <deleted>, Root Descriptor Type: CBV, Highest byte offset from view start accessed: [99],
  Bytes available from view start based on remaining resource size: 0.
  Shader Stage: COMPUTE, Root Parameter Index: [0], Shader Code: InstanceCulling.cs.hlsl(37,5-33)
```
2. **剔除链路完全空转**：`[InstanceCulling][Verify] visible=0 / total=6853 (0.0%)` 恒为 0；
   `[InstanceCullingBuffer] CullParams CBV invalid (cbAddr=0), skip dispatch` **每帧报错**（89 次/帧）
3. 帧率低（dispatch 空转 + 每帧错误日志开销）

## 根因

### 1. GpuResourceHandle 默认构造被 IsValid 误判为有效（本质根因）

`Engine/Resource/Core/GpuHandlePool.h` 原定义：

```cpp
struct GpuResourceHandle {
    uint32_t index : 22;      // 位域默认零初始化 {index=0, generation=0}
    uint32_t generation : 10;

    static constexpr GpuResourceHandle Invalid() {
        return {0x3FFFFF, 0}; // 仅 index 全 1（= 0x003FFFFF）
    }

    bool IsValid() const { return index != 0x3FFFFF; }  // ← 0 != 0x3FFFFF → 默认 {0,0} 误判为有效！
};
```

- 未显式初始化的成员（如 `GpuResourceHandle m_cullParamsUp;`）位域零初始化得到 `{index=0, generation=0}`
- 而 `index=0` 恰是句柄池**第一个合法槽位**——`IsValid()` 只查 `index != 0x3FFFFF`，无法区分"槽位 0"与"未分配"

### 2. 误判导致 CreateCullingPipeline 跳过分配

```cpp
// InstanceCullingBuffer.cpp: CreateCullingPipeline
if (!m_cullParamsUp.IsValid())            // ← {0,0} 被误判有效 → 条件为 false → 跳过创建！
    m_cullParamsUp = GpuResourceManager::GetInstance().CreateBuffer(...);
```

`m_cullParamsUp` 从未分配，句柄保持 `{index=0, gen=0}`。

### 3. dispatch 绑定悬垂 CBV

```cpp
// DispatchCulling
if (m_cullParamsUp.IsValid()) {           // 误判 true → 进入
    auto *cullRes = gpuMgr.GetResource(m_cullParamsUp); // 槽位 0 从未分配 → Validate 失败 → nullptr
    if (cullRes) { ... cbAddr = ...; }    // 不进入 → cbAddr 保持 0
}
if (cbAddr == 0) { ... skip dispatch ... } // 每帧跳过
```

- `GetResource` 内部 `Validate(handle)`（HandlePoolBase.h:96）要求 generation 匹配 + 状态非 Empty/PendingRelease——槽位 0 未分配，返回 nullptr
- 绑定地址 0 的 CBV → GBV #961；Release 模式为空指针访问 → DEVICE_HUNG/TDR（次生，之前的 TDR 崩溃与此相关）

## 修复

### 1. GpuResourceHandle 无效值改为整值 0xFFFFFFFF（`GpuHandlePool.h`）

对齐系统其他句柄约定（`SamplerHandle.slot = UINT32_MAX`）：

```cpp
struct GpuResourceHandle {
    // 默认成员初始化器 = 全 1（0xFFFFFFFF）无效值：默认构造的句柄天然无效，
    // 杜绝"位域零初始化 {index=0,gen=0} 被 IsValid 误判为有效"
    uint32_t index : 22 = 0x3FFFFF;      // 默认全 1
    uint32_t generation : 10 = 0x3FF;    // 默认全 1

    static constexpr GpuResourceHandle Invalid() {
        return {0x3FFFFF, 0x3FF}; // 整个 32 位 = 0xFFFFFFFF
    }

    bool IsValid() const { return static_cast<uint32_t>(*this) != 0xFFFFFFFF; }
};
```

### 2. InstanceCullingBuffer 成员显式 Invalid 初始化（防御）

```cpp
// InstanceCullingBuffer.h
Resource::GpuResourceHandle m_appendBuffer    = Resource::GpuResourceHandle::Invalid();
Resource::GpuResourceHandle m_indirectArgs    = Resource::GpuResourceHandle::Invalid();
Resource::GpuResourceHandle m_zeroUpload      = Resource::GpuResourceHandle::Invalid();
Resource::GpuResourceHandle m_cullParamsUp    = Resource::GpuResourceHandle::Invalid();
Resource::GpuResourceHandle m_visibleReadback = Resource::GpuResourceHandle::Invalid();
```

### 3. DispatchCulling cbAddr==0 防御（已在上次修复落地）

```cpp
if (cbAddr == 0) {
    // 打印 valid/handle index/generation，区分"从未创建"与"创建后被释放"
    ...Error("CullParams CBV invalid (cbAddr=0), skip dispatch: valid={} handle=0x{:08X} ...");
    return; // 屏障未录制，状态对称无残留
}
```

## 调用链

```
InstanceCullingBuffer 单例构造
  └─ m_cullParamsUp 默认构造 {index=0, gen=0}（旧语义）
       └─ CreateCullingPipeline: if (!m_cullParamsUp.IsValid()) → 误判有效 → 跳过 CreateBuffer
            └─ DispatchCulling（每帧 Render 阶段）
                 ├─ IsCullingReady() 通过（PSO/UAV 有效，且 m_cullParamsUp.IsValid() 误判 true）
                 ├─ GetResource(m_cullParamsUp) → nullptr（槽位 0 未分配）
                 ├─ cbAddr = 0 → SetComputeRootConstantBufferView(0, 0) → GBV #961
                 └─ visible=0（dispatch 从未执行 → readback 恒 0）
```

## 验证

| 指标 | 修复前 | 修复后 |
|:--|:--|:--|
| `CullParams buffer allocated` | `valid=true handle=0x00000000`（假有效） | `valid=true handle=0x00401E56 (index=7766, gen=1)` |
| `CullParams CBV invalid (cbAddr=0)` | 89 次/帧 | **0 次** |
| `[Verify] visible` | 恒 `0/6853 (0.0%)` | `5833→1294→5072→1559`（随相机实时波动） |
| 帧率 | 低（dispatch 空转） | **48-57 FPS** |

## 关联

- `Docs/snapshots/InstanceCulling_MemoryGrowth_TDR_Snapshot_20260808.md` §六（快照含本次修复的完整调查链）
- `Engine/Resource/Core/GpuHandlePool.h`（GpuResourceHandle 无效值 0xFFFFFFFF）
- `Engine/Resource/Core/GpuHandlePool.cpp`（FreeSlot 允许回收 PendingRelease，此前句柄池泄漏修复）
- `Engine/Renderer/Core/InstanceCullingBuffer.h/.cpp`（CullParams 分配/释放/防御日志）
- `Shaders/InstanceCulling.cs.hlsl` 第 37 行（`if (dtid.x >= gInstanceCount)`，访问 b0 CullParams 的 gInstanceCount，偏移 96-99 与 GBV 报错 [99] 吻合）

## 备注

- 其他位域句柄（`CharacterHandle`/`ClipHandle`/`GeometryHandle`/`LODMeshHandle`/`SkeletonHandle`/`TextureHandle`/`MaterialHandle`）仍使用 `{0x3FFFFF, 0}` 无效值——它们由各自 Manager 显式 `Invalid()` 初始化，未暴露本问题；若后续统一无效值语义可参照本修复对齐为 0xFFFFFFFF
- `async_test.scene`（仅 camera、0 实例）路径：`CreateCullingPipeline` 的 `m_instanceCount==0` 分支会 `ReleaseCullingResources()` 后返回 false——0 实例场景不创建剔除资源，属预期行为
