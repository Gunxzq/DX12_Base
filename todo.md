# 

1. 配置管理器并没有合并json在内存中
2. 各类文档没有更新，内容并不准确



## 全套依赖

### 物理引擎
- **刚体**: Jolt Physics (碰撞检测、刚体模拟)
- **软体**: NVIDIA FleX / PBD (布料、软体、破坏)
- **流体**: DX12 Compute Shader (水、烟雾、流体模拟)

### 音频
- miniaudio

### 资源加载
- **模型**: Assimp
- **图片**: stb_image

### 数学库
- DirectXMath

### 系统/工具
- **内存分配**: mimalloc (微软出品，优化多线程内存碎片和锁竞争)
- **调试 UI**: Dear ImGui (运行时调试面板，引擎开发神器，支持实时调参无需重新编译)



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