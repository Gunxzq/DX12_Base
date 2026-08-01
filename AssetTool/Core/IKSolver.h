#pragma once
// ========================================================================
// IKSolver — FABRIK 多轴无约束 IK 求解器（B 方案：AssetTool 离线验证）
//
// 背景（2026-08-01）：UKW 关节必然多轴（EXVS 目标 + 四台机体实测确认，
// 见 02_RobotAndAnimation.md §8.2），不做铰链锁轴。FABRIK（Forward And
// Backward Reaching IK）仅依赖关节世界位置 + 固定段长约束，天然支持
// 任意轴旋转，适合"脚贴地 / 手瞄准"的求解正确性离线验证（§8.5 B 方案）。
//
// 链定义：母版驱动（机体间命名不统一，KD-06 大写 Arm1/Arm2/Arm3 变体），
// 从 HOD 骨架按名字模式（arm/leg，大小写不敏感）+ 层级追踪动态识别，
// 不硬编码骨骼名。
// ========================================================================

#include "HODParser.h"

#include <string>
#include <vector>

namespace AssetTool {

/// 4×4 行主序矩阵（IKSolver 自包含，避免依赖 RobotMerger）
struct IKMat4 {
    float m[16] = {0};

    static IKMat4 Identity();
    IKMat4 operator*(const IKMat4 &rhs) const;
    IKMat4 Inverse() const;
    void TransformPoint(float &x, float &y, float &z) const;
};

/// IK 链上的一个关节（骨骼）
struct IKJoint {
    std::string name;              // 骨骼名（如 "arm1.x"）
    int boneIndex = -1;            // HOD 骨骼索引
    float position[3] = {0, 0, 0}; // 当前世界位置（FABRIK 求解中更新）
    float segmentLength = 0.0f;    // 与下一关节的段长（末端的为 0）
};

/// 一条 IK 链（根 → 末端）
struct IKChain {
    std::string name;                    // 链名（取根骨骼名，如 "arm1.x"）
    std::vector<IKJoint> joints;         // joints[0] = 根（固定），最后一个 = 末端
    std::vector<IKMat4> worldMats;       // 求解前每骨骼世界矩阵
    std::vector<IKMat4> solvedLocalMats; // 求解后每骨骼局部矩阵（相对 HOD 父）
};

/// FABRIK 求解结果
struct IKSolveResult {
    bool success = false;
    int iterations = 0;
    float error = 0.0f; // 末端最终误差（世界距离）
};

class IKSolver {
public:
    /// 从 HOD 骨架识别 IK 链（名字含 arm/leg，大小写不敏感）
    /// 沿子链追踪到末端（含 hand 的叶子），兼容变体：
    ///   arm1→arm2→Hand / Arm1→Arm2→Arm3→Hand / leg1→leg2→leg3
    /// 排除武装/辅助节点（gun/sword/shield/weapon_point/missile/hit 等）
    static std::vector<IKChain> FindChains(const HODData &hod);

    /// FABRIK 求解（纯位置）：根固定，末端移到 target，段长保持恒定
    /// joints 首元素为根；求解结果原地写回 joints[].position
    static IKSolveResult SolveFABRIK(std::vector<IKJoint> &joints, const float target[3], int maxIterations = 32,
                                     float tolerance = 1e-3f);

    /// 求解后的关节位置 → 每骨骼局部矩阵（保持原旋转，仅平移）
    /// 离线验证阶段以位置收敛为准；旋转朝向对齐（部件指向子关节）为下一步
    static void ApplyPositionsToLocal(IKChain &chain, const HODData &hod);
};

} // namespace AssetTool
