# BugFix: 窗口缩放时反射探针系统引起 GPU TDR

- **发现日期**: 2026-07-01
- **模块**: `ReflectionProbeManager` / `ProbeCaptureSystem`
- **现象**: 调整窗口大小时触发 `DXGI_ERROR_DEVICE_HUNG`，GPU 超时 2s+，设备移除。

## 触发路径

```
窗口缩放
  → WindowResizeSystem (EarlyUpdate)
    → DeviceContext->OnResize()
      → FlushAllQueues()
      → 重建后缓冲 + 主深度缓冲
    → CameraMgr->OnResize()        // 投影矩阵更新
    → OpaqueRenderer->OnResize()   // 投影矩阵更新
    → ❌ 无 ReflectionProbeManager resize

下一帧：
  → ProbeCaptureSystem (Render phase)
    → 引用缩放后失效的资源（DSV / SRV / 常量缓冲区）
    → GPU 访问无效资源 → hang → TDR
```

## 根因

`ReflectionProbeManager` **没有 `OnResize` 方法**，窗口缩放后其内部的以下资源与新的后缓冲/深度缓冲尺寸不匹配：

| 资源 | 说明 |
|:-----|:------|
| 探针捕获私有深度缓冲 | `DepthStencilPool` 中分配，缩放后未重建 |
| Cubemap RTV（6 个面） | `RenderTargetPool` 中分配，缩放后未重建 |
| `ProbeCaptureInfo.dsvSlot` | DSV 描述符槽位，缩放后可能引用已释放资源 |
| `ProbeCaptureInfo.rtvBaseSlot` | RTV 描述符槽位，同上 |
| `captureCBAddress` | 常量缓冲区地址，在 `RegisterProbeSceneDataCallback` 中分配 |

## 修复方案

### 方案 A：添加 OnResize（推荐）

1. 在 `ReflectionProbeManager` 中添加：

```cpp
void OnResize(uint32_t width, uint32_t height);
```

2. 实现：遍历所有探针，Release + 重建 `ProbeRuntimeResources`（RT 句柄、SRV 槽位），重新分配 DSV 槽位。

3. 在 `WindowResizeSystem`（Game.cpp）中添加调用：

```cpp
if (m_context->ReflectionProbeMgr) {
    m_context->ReflectionProbeMgr->OnResize(width, height);
}
```

### 方案 B：固定探针分辨率

探针捕获使用固定分辨率（如 256x256），不依赖窗口尺寸，则不需要重建。但需确保 `DepthStencilPool` 和 `RenderTargetPool` 的池条目在缩放后不被释放。

## 临时屏蔽

```cpp
// GameWorld.cpp
RegisterProbeSceneDataCallback();  // 已注释
RegisterProbeCaptureSystem();      // 已注释
```
