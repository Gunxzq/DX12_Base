## 阶段任务划分
PostCulling 阶段
    │
    ├── 线程1（PostCulling 阶段）：
    │
    ├── UI 检测（0.01ms）
    │   ├── 命中 → 设置 hasUIHit，跳过 Visible 和 Scene
    │   └── 未命中 → 继续
    │
    ├── Visible 检测（0.5ms）
    │   ├── 命中 → 设置 hasVisibleHit，跳过 Scene
    │   └── 未命中 → 继续
    │
    └── Scene 检测（2ms）← 仅在 UI 和 Visible 都未命中时执行
    │    └── 执行完整的全场景射线检测
    │
    ├── 线程2：OcclusionSystem（独立，可并行）
    │
    ├── 线程3：LODSystem（独立，可并行）


1. 可见集与场景集的检测实际上应该是分支，输入系统的上下文可以明确的决定使用可见集还是场景集。