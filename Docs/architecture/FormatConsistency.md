# 格式一致性约束 (Format Consistency)

## 总则

DX12 中格式一致性不是 RTV↔DSV 之间的对应关系，而是同一资源的**全链路格式匹配**。

资源格式链：**资源创建 → 视图创建(DSV/SRV/RTV) → PSO 声明 → Shader 声明**

RTV（back buffer）和 DSV（depth buffer）是**两个独立资源**，无需互相对应。

---

## 深度缓冲格式链（以 D32_FLOAT 为例）

```
Config (renderer.json)
  ↓
RendererConfig::DepthStencilFormatEnum
  ↓
D3D12DeviceContext::InitParams::depthStencilFormat
  ↓
SwapChainManager::Params::depthStencilFormat  ──── 资源创建: DXGI_FORMAT_D32_FLOAT
  ↓                                                           ↓
PSO::DSVFormat (各渲染器)                          DSV: DXGI_FORMAT_D32_FLOAT
  ↓                                                           ↓
深度测试                                          SRV: DXGI_FORMAT_R32_FLOAT
                                                              ↓
                                                          Shader: float / uint
```

## 关键约束

### 1. 资源格式必须为 Typeless（如要同时使用 DSV + SRV）

| 资源格式 | DSV 格式 | SRV 格式 | 适用场景 |
|:--------:|:--------:|:--------:|----------|
| `R32_TYPELESS` | `D32_FLOAT` | `R32_FLOAT` | 需要深度测试 + SSAO 采样深度 |
| `R24G8_TYPELESS` | `D24_UNORM_S8_UINT` | `R24_UNORM_X8_TYPELESS` | 24 位深度 + 8 位模板 + SSAO |
| `D32_FLOAT` | `D32_FLOAT` | `R32_FLOAT` (DirectX 特殊允许) | 纯深度，无模板 |
| `D24_UNORM_S8_UINT` | `D24_UNORM_S8_UINT` | `R24_UNORM_X8_TYPELESS` (DirectX 特殊允许) | 深度 + 模板，无 Typeless |

> **说明**：DirectX 对 depth-stencil 资源有特殊规则，即使资源不是 Typeless 也允许创建不同格式的 SRV。但使用 Typeless 资源是更规范的做法，可读性更好。

### 2. 各环节格式对应关系（不可跨族）

| 资源创建格式 | 允许的 DSV 格式 | 允许的 SRV 格式 |
|:-----------:|:---------------:|:---------------:|
| `R32_TYPELESS` | `D32_FLOAT` | `R32_FLOAT`, `R32_UINT` |
| `R24G8_TYPELESS` | `D24_UNORM_S8_UINT` | `R24_UNORM_X8_TYPELESS`, `X24_TYPELESS_G8_UINT` |
| `D32_FLOAT` | `D32_FLOAT` | `R32_FLOAT` (特殊规则) |
| `D24_UNORM_S8_UINT` | `D24_UNORM_S8_UINT` | `R24_UNORM_X8_TYPELESS` (特殊规则) |

### 3. PSO DSVFormat 必须匹配实际绑定的 DSV 格式

所有使用 `m_context->GetDepthStencilFormat()` 的渲染器均自动与配置同步。

### 4. 不同深度格式之间不允许直接切换

如从 `D32_FLOAT` 切换到 `D24_UNORM_S8_UINT`，需同步修改：
- `renderer.json` 配置
- 资源创建格式（`SwapChainManager::CreateDepthStencilView`）
- SRV 创建格式（`D3D12DeviceContext::CreateDepthSRV` 及 `DepthStencilPool::CreateNewEntry`）

---

## 当前检查结果

### 配置

| 位置 | 值 |
|------|:--:|
| `Config/renderer.json:26` | `"D32_FLOAT"` |
| `RendererConfig.h:96,100` | `D24_UNORM_S8_UINT`（字符串默认值）→ `D32_FLOAT`（实际配置覆盖） |

### 格式链逐点检查

| # | 环节 | 文件:行 | 代码 | 格式 | 状态 |
|:-:|------|---------|------|:---:|:----:|
| 1 | Config → Enum | `RendererConfig.h:105-106` | `PostLoad()` | 按配置解析 | ✅ |
| 2 | Bootstrap → Context | `Bootstrap.cpp:173` | `params.depthStencilFormat = DepthStencilFormatEnum` | 按配置 | ✅ |
| 3 | Context → SwapChain | `D3D12DeviceContext.cpp:75` | `swapParams.depthStencilFormat = params.depthStencilFormat` | 按配置 | ✅ |
| 4 | **资源创建** | **`SwapChainManager.cpp:138`** | **`depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT`** | **硬编码** | ⚠️ 偏离配置 |
| 5 | Clear Value | `SwapChainManager.cpp:145` | `optClear.Format = DXGI_FORMAT_D32_FLOAT` | **硬编码** | ⚠️ 偏离配置 |
| 6 | DSV 格式 | `SwapChainManager.cpp:158` | `dsvDesc.Format = DXGI_FORMAT_D32_FLOAT` | **硬编码** | ⚠️ 偏离配置 |
| 7 | 主深度 SRV | `D3D12DeviceContext.cpp:168-171` | `D32_FLOAT→R32_FLOAT / else→R24_UNORM_X8_TYPELESS` | 动态分支 | ✅ |
| 8 | 池 DSV/SRV | `DepthStencilPool.cpp:139-143` | `switch(desc.format)` 支持 D32/D24/D16 | 动态分支 | ✅ |
| 9-14 | 各渲染器 PSO | `OpaqueRenderer/SkyRenderer/...` | `GetDepthStencilFormat()` | 统一接口 | ✅ |

### 发现的问题

**`SwapChainManager::CreateDepthStencilView`** 中资源格式、ClearValue、DSV 格式均**硬编码**为 `DXGI_FORMAT_D32_FLOAT`，未使用 `m_params.depthStencilFormat`。当前配置恰好也是 `D32_FLOAT` 因此功能正常，但**若改配置文件为 `D24_UNORM_S8_UINT`，实际创建的仍是 `D32_FLOAT` 资源**，会导致格式链断裂。

### 修复建议

`SwapChainManager.cpp:138,145,158` 三处硬编码改为使用 `m_params.depthStencilFormat`。

## 影响范围

| 模块 | 依赖格式 | 获取方式 |
|------|---------|----------|
| `SwapChainManager` | 资源/DSV/Clear | 当前硬编码，建议改为参数化 |
| `D3D12DeviceContext` | 主深度 SRV | 动态分支，按 `mDepthStencilFormat` |
| `DepthStencilPool` | 池化 DSV/SRV | `switch` 分支，支持所有主流格式 |
| `OpaqueRenderer` | PSO DSVFormat | `GetDepthStencilFormat()` |
| `SkyRenderer` | PSO DSVFormat | `GetDepthStencilFormat()` |
| `TerrainRenderer` | PSO DSVFormat | `GetDepthStencilFormat()` |
| `WaterRenderer` | PSO DSVFormat | `GetDepthStencilFormat()` |
| `BillboardRenderer` | PSO DSVFormat | `GetDepthStencilFormat()` |
| `SsaoRenderer` | PSO DSVFormat | `GetDepthStencilFormat()` |
| `ShadowRenderer` | PSO DSVFormat | `DXGI_FORMAT_D32_FLOAT`（独立深度缓冲） |
| `ReflectionProbeRenderer` | PSO DSVFormat | `DXGI_FORMAT_D32_FLOAT`（独立深度缓冲） |
