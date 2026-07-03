# SSAO 环境贴图资源状态错乱导致 ResourceBarrier 不匹配

## 症状

D3D12 Validation Layer 报错：
```
ResourceBarrier: Before state (COMMON|PRESENT) of resource ('SSAO_Ambient0') 
does not match with the state (PIXEL_SHADER_RESOURCE) specified in the previous call
```

## 根因

### 原因一：`InitializeResourceStates()` 做了多余的 `COMMON → PS_SRV` 转换

`AmbientOcclusionManager::InitializeResourceStates()` 在第 0 帧上传阶段把 ambient0/ambient1 从 `COMMON` 转为 `PS_SRV`。但 SSAO System 外层屏障后来被改为 `COMMON → RT`，这一转换已是多余。当 `OnResize` 触发第二次 `BuildResources` 时，池子复用了处于 `PS_SRV` 状态的旧资源，导致后续 SSAO 外层 `COMMON → RT` 屏障不匹配。

**修复**：删除 `InitializeResourceStates()` 及其调用，资源保持 `COMMON` 初始状态。

### 原因二：`SsaoRenderer::BlurAO` 提前返回未回退屏障

`SsaoRenderer::Execute` 中将 ambient0 转为 `PS_SRV` 供 BlurAO 读取，但 `BlurAO` 内部在 blur PSO 或根签名未就绪时 `return`，导致后续 `PS_SRV → RT` 屏障被跳过，ambient0 残留 `PS_SRV` 状态。

**修复**：外层 `Execute` 检查 `blurPSO && m_blurRootSig`，不满足时回退 ambient0 状态。

### 原因三：`OnResize` 无条件重建

窗口初始化过程中 `OnResize` 被多次触发，每次都释放并重建资源。池化复用的旧资源残留了第一次 `InitializeResourceStates` 遗留的 `PS_SRV` 状态。

**修复**：`OnResize` 检查尺寸是否真实变化，相同尺寸不重建。

## 涉及文件

| 文件 | 改动 |
|:----|:-----|
| `AmbientOcclusionManager.h` | 注释 `InitializeResourceStates()` 声明 |
| `AmbientOcclusionManager.cpp` | 注释 `InitializeResourceStates()` 实现；`OnResize` 加尺寸检查；`BuildResources` 记录尺寸 |
| `Game.cpp` | 移除 `InitializeResourceStates()` 调用 |
| `SsaoRenderer.cpp` | `Execute()` 中 blur 部分加条件检查 + else 回退屏障 |
| `RenderTargetPool.cpp` | 确认池化复用不重置状态（非 bug，是设计约束） |

## 经验教训

1. 资源管理器/池不负责追踪 GPU state，每个 system 必须自行处理屏障
2. `OnResize` 应检查尺寸变化，避免重复重建
3. 多条 code path 都必须保证屏障完整回退
