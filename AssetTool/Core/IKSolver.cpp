// ========================================================================
// IKSolver.cpp — FABRIK 多轴无约束 IK 求解器实现
// ========================================================================

#include "IKSolver.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iostream>

using namespace AssetTool;

// ==========================================================================
// IKMat4
// ==========================================================================

IKMat4 IKMat4::Identity() {
    IKMat4 r;
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

IKMat4 IKMat4::operator*(const IKMat4 &rhs) const {
    IKMat4 r;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            for (int k = 0; k < 4; ++k)
                r.m[row * 4 + col] += m[row * 4 + k] * rhs.m[k * 4 + col];
    return r;
}

IKMat4 IKMat4::Inverse() const {
    float a[4][8];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j)
            a[i][j] = m[i * 4 + j];
        for (int j = 4; j < 8; ++j)
            a[i][j] = (i == (j - 4)) ? 1.0f : 0.0f;
    }
    for (int col = 0; col < 4; ++col) {
        int sel = col;
        for (int i = col + 1; i < 4; ++i)
            if (std::fabs(a[i][col]) > std::fabs(a[sel][col]))
                sel = i;
        if (std::fabs(a[sel][col]) < 1e-10f)
            return Identity();
        if (sel != col)
            for (int j = 0; j < 8; ++j)
                std::swap(a[col][j], a[sel][j]);
        float div = a[col][col];
        for (int j = 0; j < 8; ++j)
            a[col][j] /= div;
        for (int i = 0; i < 4; ++i) {
            if (i == col)
                continue;
            float mul = a[i][col];
            for (int j = 0; j < 8; ++j)
                a[i][j] -= mul * a[col][j];
        }
    }
    IKMat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i * 4 + j] = a[i][j + 4];
    return r;
}

void IKMat4::TransformPoint(float &x, float &y, float &z) const {
    float nx = x * m[0] + y * m[4] + z * m[8] + m[12];
    float ny = x * m[1] + y * m[5] + z * m[9] + m[13];
    float nz = x * m[2] + y * m[6] + z * m[10] + m[14];
    float nw = x * m[3] + y * m[7] + z * m[11] + m[15];
    if (nw != 0.0f) {
        x = nx / nw;
        y = ny / nw;
        z = nz / nw;
    } else {
        x = nx;
        y = ny;
        z = nz;
    }
}

// ==========================================================================
// 辅助
// ==========================================================================

namespace {

/// 小写化（兼容 KD-06 大写命名 Arm1/Hand 等）
std::string ToLower(const std::string &s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s)
        r += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

/// 名字是否属于可 IK 的肢体链段（含 arm/leg/hand，排除武装/辅助）
bool IsLimbName(const std::string &lower) {
    if (lower.find("arm") == std::string::npos &&
        lower.find("leg") == std::string::npos &&
        lower.find("hand") == std::string::npos)
        return false;
    // 排除武装/特效/辅助节点
    if (lower.find("gun") != std::string::npos)      return false;
    if (lower.find("sword") != std::string::npos)    return false;
    if (lower.find("shield") != std::string::npos)   return false;
    if (lower.find("weapon") != std::string::npos)   return false;
    if (lower.find("missile") != std::string::npos)  return false;
    if (lower.find("output") != std::string::npos)   return false;
    if (lower.find("hit") != std::string::npos)      return false;
    if (lower.find("root") != std::string::npos)     return false;
    return true;
}

bool IsChainRootName(const std::string &lower) {
    return lower.find("arm") != std::string::npos ||
           lower.find("leg") != std::string::npos;
}

} // namespace

// ==========================================================================
// FindChains — 母版驱动链识别
// ==========================================================================

std::vector<IKChain> IKSolver::FindChains(const HODData &hod) {
    std::vector<IKChain> chains;

    // 1. 收集所有"链根"候选：名字含 arm/leg 且其父不是 limb（避免把
    //    arm2 这种中段当作新链根）
    std::vector<int> roots;
    for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
        std::string lower = ToLower(hod.bones[bi].name);
        if (!IsChainRootName(lower) || !IsLimbName(lower))
            continue;
        int parent = hod.bones[bi].parentIndex;
        bool parentIsLimb = false;
        if (parent >= 0) {
            std::string pl = ToLower(hod.bones[parent].name);
            parentIsLimb = IsChainRootName(pl) && IsLimbName(pl);
        }
        if (!parentIsLimb)
            roots.push_back(bi);
    }

    // 2. 沿子链向下追踪：当前段 → 名字含 arm/leg 的子节点（取第一个），
    //    直到没有 limb 子节点；末端追加以 hand 结尾的叶子
    for (int root : roots) {
        IKChain chain;
        chain.name = hod.bones[root].name;

        int cur = root;
        while (true) {
            IKJoint j;
            j.name = hod.bones[cur].name;
            j.boneIndex = cur;
            chain.joints.push_back(j);

            // 找名字含 arm/leg 的子节点（取第一个，排除武装）
            int next = -1;
            for (uint32_t ci : hod.bones[cur].children) {
                int c = static_cast<int>(ci);
                std::string cl = ToLower(hod.bones[c].name);
                if (IsChainRootName(cl) && IsLimbName(cl)) {
                    next = c;
                    break;
                }
            }
            if (next < 0)
                break;
            cur = next;
        }

        // 末端：找名字含 hand 的子节点（取第一个）
        for (uint32_t ci : hod.bones[cur].children) {
            int c = static_cast<int>(ci);
            std::string cl = ToLower(hod.bones[c].name);
            if (cl.find("hand") != std::string::npos && IsLimbName(cl)) {
                IKJoint j;
                j.name = hod.bones[c].name;
                j.boneIndex = c;
                chain.joints.push_back(j);
                break;
            }
        }

        if (chain.joints.size() >= 2) {
            // 预计算每骨骼世界矩阵 + 关节位置 + 段长
            std::vector<IKMat4> world(hod.BoneCount());
            for (int bi = 0; bi < static_cast<int>(hod.BoneCount()); ++bi) {
                IKMat4 local;
                for (int k = 0; k < 16; ++k)
                    local.m[k] = static_cast<float>(hod.bones[bi].transform[k]);
                int parent = hod.bones[bi].parentIndex;
                world[bi] = (parent >= 0) ? (local * world[parent]) : local;
            }
            chain.worldMats.resize(chain.joints.size());
            for (size_t i = 0; i < chain.joints.size(); ++i) {
                int bi = chain.joints[i].boneIndex;
                chain.worldMats[i] = world[bi];
                chain.joints[i].position[0] = world[bi].m[12];
                chain.joints[i].position[1] = world[bi].m[13];
                chain.joints[i].position[2] = world[bi].m[14];
                if (i + 1 < chain.joints.size()) {
                    int bj = chain.joints[i + 1].boneIndex;
                    float dx = world[bj].m[12] - world[bi].m[12];
                    float dy = world[bj].m[13] - world[bi].m[13];
                    float dz = world[bj].m[14] - world[bi].m[14];
                    chain.joints[i].segmentLength = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
            }
            chains.push_back(chain);
        }
    }
    return chains;
}

// ==========================================================================
// SolveFABRIK — FABRIK 纯位置迭代
// ==========================================================================

IKSolveResult IKSolver::SolveFABRIK(std::vector<IKJoint> &joints,
                                    const float target[3],
                                    int maxIterations,
                                    float tolerance) {
    IKSolveResult res;
    const size_t n = joints.size();
    if (n < 2) {
        res.error = -1.0f;
        return res;
    }

    // 固定段长（从初始位置计算，迭代中保持恒定）
    std::vector<float> seg(n - 1);
    float totalLen = 0.0f;
    for (size_t i = 0; i < n - 1; ++i) {
        float dx = joints[i + 1].position[0] - joints[i].position[0];
        float dy = joints[i + 1].position[1] - joints[i].position[1];
        float dz = joints[i + 1].position[2] - joints[i].position[2];
        seg[i] = std::sqrt(dx * dx + dy * dy + dz * dz);
        totalLen += seg[i];
    }

    // 根位置固定
    const float rootX = joints[0].position[0];
    const float rootY = joints[0].position[1];
    const float rootZ = joints[0].position[2];

    // 目标到根的距离
    float tx = target[0] - rootX;
    float ty = target[1] - rootY;
    float tz = target[2] - rootZ;
    float distToTarget = std::sqrt(tx * tx + ty * ty + tz * tz);

    // 目标不可达（超过链总长）：整链拉直指向目标
    if (distToTarget >= totalLen - 1e-5f) {
        float ux = (distToTarget > 1e-9f) ? (tx / distToTarget) : 1.0f;
        float uy = (distToTarget > 1e-9f) ? (ty / distToTarget) : 0.0f;
        float uz = (distToTarget > 1e-9f) ? (tz / distToTarget) : 0.0f;
        joints[0].position[0] = rootX;
        joints[0].position[1] = rootY;
        joints[0].position[2] = rootZ;
        for (size_t i = 1; i < n; ++i) {
            joints[i].position[0] = joints[i - 1].position[0] + seg[i - 1] * ux;
            joints[i].position[1] = joints[i - 1].position[1] + seg[i - 1] * uy;
            joints[i].position[2] = joints[i - 1].position[2] + seg[i - 1] * uz;
        }
        res.success = true;
        res.iterations = 0;
        res.error = std::fabs(distToTarget - totalLen);
        return res;
    }

    // 可达：FABRIK 前后向迭代
    for (int iter = 0; iter < maxIterations; ++iter) {
        // 后向：末端 → 根（末端拉到目标，逐段回退）
        joints[n - 1].position[0] = target[0];
        joints[n - 1].position[1] = target[1];
        joints[n - 1].position[2] = target[2];
        for (size_t i = n - 2; i < n; --i) {  // size_t 下溢保护
            float dx = joints[i].position[0] - joints[i + 1].position[0];
            float dy = joints[i].position[1] - joints[i + 1].position[1];
            float dz = joints[i].position[2] - joints[i + 1].position[2];
            float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            float lam = (len > 1e-9f) ? (seg[i] / len) : 1.0f;
            joints[i].position[0] = joints[i + 1].position[0] + dx * lam;
            joints[i].position[1] = joints[i + 1].position[1] + dy * lam;
            joints[i].position[2] = joints[i + 1].position[2] + dz * lam;
            if (i == 0)
                break;
        }

        // 前向：根 → 末端（根回原位，逐段推进）
        joints[0].position[0] = rootX;
        joints[0].position[1] = rootY;
        joints[0].position[2] = rootZ;
        for (size_t i = 0; i + 1 < n; ++i) {
            float dx = joints[i + 1].position[0] - joints[i].position[0];
            float dy = joints[i + 1].position[1] - joints[i].position[1];
            float dz = joints[i + 1].position[2] - joints[i].position[2];
            float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            float lam = (len > 1e-9f) ? (seg[i] / len) : 1.0f;
            joints[i + 1].position[0] = joints[i].position[0] + dx * lam;
            joints[i + 1].position[1] = joints[i].position[1] + dy * lam;
            joints[i + 1].position[2] = joints[i].position[2] + dz * lam;
        }

        // 收敛检查：末端与目标距离
        float ex = joints[n - 1].position[0] - target[0];
        float ey = joints[n - 1].position[1] - target[1];
        float ez = joints[n - 1].position[2] - target[2];
        float err = std::sqrt(ex * ex + ey * ey + ez * ez);
        res.iterations = iter + 1;
        if (err <= tolerance) {
            res.success = true;
            res.error = err;
            return res;
        }
        res.error = err;
    }
    // 达到最大迭代次数：按最终误差判定是否收敛
    res.success = (res.error <= tolerance);
    return res;
}

// ==========================================================================
// ApplyPositionsToLocal — 关节位置 → 局部矩阵
// ==========================================================================

void IKSolver::ApplyPositionsToLocal(IKChain &chain, const HODData &hod) {
    const size_t n = chain.joints.size();
    chain.solvedLocalMats.assign(n, IKMat4());

    // 重建世界矩阵：保持原旋转，仅替换平移为求解后的关节位置
    std::vector<IKMat4> newWorld = chain.worldMats;
    for (size_t i = 0; i < n; ++i) {
        newWorld[i].m[12] = chain.joints[i].position[0];
        newWorld[i].m[13] = chain.joints[i].position[1];
        newWorld[i].m[14] = chain.joints[i].position[2];
    }

    // 世界 → 局部：local = inverse(parentWorld) × world
    for (size_t i = 0; i < n; ++i) {
        int bi = chain.joints[i].boneIndex;
        int parent = hod.bones[bi].parentIndex;
        if (parent >= 0) {
            // 父世界矩阵（HOD 原始累乘）
            IKMat4 parentWorld;
            {
                std::vector<IKMat4> pw(hod.BoneCount());
                for (int bj = 0; bj < static_cast<int>(hod.BoneCount()); ++bj) {
                    IKMat4 local;
                    for (int k = 0; k < 16; ++k)
                        local.m[k] = static_cast<float>(hod.bones[bj].transform[k]);
                    int pj = hod.bones[bj].parentIndex;
                    pw[bj] = (pj >= 0) ? (local * pw[pj]) : local;
                }
                parentWorld = pw[parent];
            }
            chain.solvedLocalMats[i] = parentWorld.Inverse() * newWorld[i];
        } else {
            chain.solvedLocalMats[i] = newWorld[i];
        }
    }
}
