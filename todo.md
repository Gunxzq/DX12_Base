# 

1. 配置管理器并没有合并json在内存中
2. 各类文档没有更新，内容并不准确



## 迁移

⚠️ 需要补充才能移除的功能
D3DApp 功能	说明
WM_ACTIVATE 暂停/恢复	游戏暂停时需要停止计时器
WM_ENTERSIZEMOVE/EXITSIZEMOVE	拖动窗口时暂停渲染
WM_GETMINMAXINFO	限制最小窗口大小
CalculateFrameStats	帧率显示
Set4xMsaaState	运行时切换 MSAA
全屏切换	Alt+Enter
鼠标消息处理	OnMouseDown/Up/Move



## 自建PBD

1. 流体/粒子：PBD-Fluid-in-Unity
这个仓库是学习 SPH（平滑粒子流体动力学）+ PBD 混合实现的经典案例。你提到的 GridHash（空间哈希网格优化）和 FluidSolver 是它的核心，非常适合用来参考如何编写 HLSL Compute Shader。
GitHub 链接：
👉 https://github.com/Scrawk/PBD-Fluid-in-Unity
重点关注文件：
Assets/ComputeShaders/GridHash.compute：学习如何用 GPU 做空间分区（这是流体性能的关键）。
Assets/ComputeShaders/FluidSolver.compute：学习密度计算、压力求解和位置更新的 Shader 写法。
2. 软体/布料：Genesis
Genesis 是一个基于 Python 的高性能物理仿真平台（由 MIT 等机构的大佬开发）。虽然它是 Python 写的，但它的 PBD 求解器逻辑（特别是 XPBD 部分）写得非常清晰，是理解“拉格朗日乘子”如何控制刚度的绝佳教材。
GitHub 链接：
👉 https://github.com/Genesis-Embodied-AI/Genesis
重点关注路径：
进入 genesis 文件夹 -> engine -> solver -> pbd。
查看 pbd_solver.py 或相关的约束处理文件，你会看到类似 self.lagrangian += ... 的代码，这就是 XPBD 的核心数学逻辑。
💡 食用指南
先看 Unity 项目 的 Shader 代码，把 HLSL 的框架搭起来（因为它是现成的 GPU 代码）。
再看 Genesis 的 Python 代码，把数学公式（特别是那个乘子的更新公式）抄下来，填入你的 C++ 或 HLSL 逻辑中。



在佛山这个周四的下午，结合你上传的 `Logger`、`SinkFactory` 和 `DebugOverlay` 代码，你的直觉非常精准。

**DebugUtils 确实是不必要的，它应该被日志系统（Logger）的一个 Sink 取代。**

你现在的代码结构其实已经**完美地**证明了这一点。让我结合你的代码文件，为你分析为什么这样做是“轻量”且符合架构规范的。

### 📂 1. 你的代码现状：DebugOverlay 已经是一个 Sink 了

看你的 `[2] SinkFactory.cpp` 文件，这行代码暴露了真相：

```cpp
#include "System/Logger/DebugOverlay.h"
```

在 `CreateLogWindowSink` 函数中，你实际上是把 `DebugOverlay` 的逻辑包装进了一个 `log_window_sink_mt`。这意味着：

*   **DebugOverlay 的职责**：创建窗口、处理 Win32 消息、绘制文本。
*   **Sink 的职责**：接收日志字符串、调用 `DebugOverlay` 的 `PushLog`。

你现在的架构其实是：`Logger` -> `LogWindowSink` -> `DebugOverlay`。

### 🤔 2. 为什么说 "DebugUtils" 是多余的？

如果你把内存统计功能放在一个叫 `DebugUtils` 的工具类里，你会遇到两个问题：

1.  **依赖混乱**：`DebugUtils` 需要包含 `ResourceManager` 的头文件来读取数据。如果 `ResourceManager` 又想用日志（Logger）来记录分配，而 Logger 又依赖 DebugOverlay（如你的代码所示），你就创造了一个**循环依赖**（ResourceManager -> Logger -> DebugOverlay -> DebugUtils -> ResourceManager）。
2.  **职责不清**：`DebugUtils` 既想画图，又想处理 Win32 消息，又想接收日志。这违反了“单一职责原则”。

### 🚀 3. 最佳实践：让 Sink 承担一切

既然你的日志系统已经支持 `LogWindowSink`，最好的方案是**把内存统计也变成一行日志**。

**不要**让 `DebugOverlay` 去主动“拉取”内存数据，而是让资源管理器（ResourceManager）定时“推送”内存数据给 Logger，Logger 会自动通过 Sink 显示在窗口里。

#### ✅ 修改方案

1.  **移除** `DebugUtils` 类。
2.  **修改** `ResourceManager`：在分配/释放时，或者在每帧更新时，调用 `Logger::Info("Memory: {} MB", usage)`。
3.  **复用** `LogWindowSink`：你的 `[3] DebugOverlay.cpp` 已经能显示文本了。现在它不仅能显示 "Loading failed"，还能显示 "Current Memory: 120MB"。

#### 💻 代码示例

在你的 `ResourceManager` 中：

```cpp
void ResourceManager::UpdateStats() {
    // 每秒调用一次，或者每帧（如果想看实时）
    Logger::GetInstance()->GetLogger()->info("[Memory] Current: {:.2f} MB | Peak: {:.2f} MB", 
        m_CurrentMemoryUsage / 1024.0 / 1024.0, 
        m_PeakMemoryUsage / 1024.0 / 1024.0);
}
```

#### 📊 效果对比

| 你的原始想法 (DebugUtils) | 现在的推荐方案 (Log Sink) |
| :--- | :--- |
| `DebugUtils` 有一个循环，不断去问 `RM` 要数据。 | `RM` 有一个函数，把数据发给 `Logger`。 |
| `DebugUtils` 必须包含 Win32 头文件和 GUI 逻辑。 | `RM` 只需要包含 Logger 接口（依赖倒置）。 |
| 如果你关掉 DebugOverlay，`DebugUtils` 还在浪费 CPU 跑循环。 | 如果你关掉 LogWindow Sink，日志不会被处理，零开销。 |
| **重**：紧耦合，难以维护。 | **轻**：松耦合，符合现代 C++ 日志流思想。 |

### 📌 4. 总结

你的直觉是对的。**DebugUtils 是不必要的。**

你上传的代码 `[2] SinkFactory.cpp` 里，`CreateLogWindowSink` 已经证明了你倾向于使用 **Sink 模式**。既然如此，就贯彻到底：

*   **Logger** 是核心总线。
*   **Sink (DebugOverlay)** 是显示器。
*   **ResourceManager** 是数据源。

**数据流向应该是：** `ResourceManager` (推数据) -> `Logger` (总线) -> `LogWindowSink` (显示器)。

这样，你就不需要一个单独的 `DebugUtils` 来做轮询统计了。你只需要在 `ResourceManager` 里加一行日志代码，佛山这个周四下午的内存统计功能就完成了。




你的直觉非常敏锐，**确实存在冗余，而且内存占用确实会成倍增长**。

在 2026 年的游戏开发环境下，内存（RAM）虽然便宜，但**带宽**和**缓存**是昂贵的。如果设计不当，这种“双倍内存”确实可能导致 **OOM** 或者严重的性能抖动。

不过，业界通常有成熟的策略来规避这个问题，并不会真的让内存无限膨胀。我们来拆解一下这个“冗余”到底有多大，以及如何消除它。

### ⚖️ 1. 为什么会“成倍增长”？（冗余的来源）

假设你要加载一个 **100MB** 的超大贴图：

1.  **IO 线程自带内存区（临时缓冲）**：
    *   IO 线程从硬盘读出来的数据，首先得有个地方放。通常是一个 `std::vector<uint8_t>` 或者一块 `malloc` 出来的内存。
    *   **占用**：100MB。
2.  **资源管理器（最终存储）**：
    *   数据解压/处理后，要存入显卡显存或系统内存的纹理数组中。
    *   **占用**：100MB。

**峰值时刻**：在数据复制的那一微秒，内存里确实同时存在两份 100MB 的数据。
**总量**：200MB。

如果你同时加载 10 个这样的贴图，就需要额外 1GB 的临时内存。如果是在内存受限的设备（如手机或 Switch），这确实会 **爆**。

### 🛠️ 2. 如何消除冗余？（零拷贝与原地转换）

为了解决这个问题，现代引擎（包括你参考的架构）通常采用以下两种优化手段，**让“临时内存”几乎消失**。

#### 方案 A：原地加载（In-Place Loading）—— 最推荐
这是最彻底的办法。**根本不使用“IO 线程自带内存区”**。

*   **流程**：
    1.  IO 线程先向资源管理器申请一块 100MB 的内存（拿到 `Index` 和对应的指针 `Ptr`）。
    2.  IO 线程直接告诉硬盘驱动：“请把数据直接读到 `Ptr` 这个地址去”。
    3.  **结果**：数据从硬盘 -> 直接进入资源管理器的内存。
*   **优势**：**零冗余**。内存占用只有 100MB。
*   **前提**：资源管理器分配的内存必须是**连续的**且**可写的**。

#### 方案 B：内存交换（Move Semantics / Swap）
如果无法直接读到目标地址（比如需要先解密、先解压），不得不先用临时缓冲区。

*   **流程**：
    1.  IO 线程用临时缓冲区读完数据（100MB）。
    2.  资源管理器分配最终内存（100MB）。
    3.  **关键一步**：不使用 `memcpy` 复制，而是使用 `std::move` 或者指针交换。
    4.  资源管理器直接接管临时缓冲区的内存指针，不再重新分配新内存。
*   **优势**：避免了 CPU 拷贝数据的时间，虽然内存峰值还是双份，但可以通过**限制并发加载数量**来控制。

### 📉 3. 关于“会不会爆”的评估

**通常情况下，不会爆。** 原因如下：

1.  **加载是串行的或限流的**：
    虽然你有多个 IO 线程，但引擎通常会限制“同时加载的大资源数量”。比如，虽然开了 4 个线程，但同一时间只允许 2 个线程在处理 100MB 的大文件，其他的在排队。这样就把内存峰值控制住了。
2.  **临时内存是“短命”的**：
    临时缓冲区在数据复制完成后**立刻释放**。它不像资源管理器的内存那样长期驻留。这种短暂的峰值通常能被操作系统的虚拟内存机制（Virtual Memory）消化。
3.  **流式加载（Streaming）**：
    对于超大的资源（比如 1GB 的开放世界地图），引擎不会一次性读完。而是分块读取（比如每次 4MB）。
    *   **临时内存**：只需要 4MB。
    *   **资源管理器**：慢慢拼凑这 1GB。
    *   **结果**：内存占用极低，完全不会爆。

### 💡 4. 总结与建议

你担心的“内存成倍增长”在**极端情况下**（瞬间加载大量超大资源）是存在的，但在**正常设计**的引擎中是可控的。

**给你的建议：**

1.  **首选“原地加载”**：尽量让 IO 线程直接向资源管理器申请内存，并直接读入。这样就没有“自带内存区”了，彻底解决冗余。
2.  **次选“流式缓冲”**：如果必须用临时区，把临时区做小（比如固定 1MB - 4MB），分批次搬运数据。
3.  **不要过度优化**：在 PC 端，几百兆的临时内存峰值通常不是问题；只有在移动端才需要斤斤计较。

**结论**：只要你不让 10 个线程同时加载 10 个 10GB 的 uncompressed 纹理，内存就不会爆。放心大胆地设计吧！