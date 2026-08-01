// ========================================================================
// RobotMergerMath.cpp — RobotMerger::Mat4x4 实现
// 拆分自 RobotMerger.cpp（2026-08-01，适度拆分以缓解 C1060 编译堆不足）
// ========================================================================

#include "RobotMerger.h"

#include <algorithm>
#include <cmath>

using namespace AssetTool;

// ==========================================================================
// Mat4x4 实现（行主序 4×4）
// ==========================================================================

RobotMerger::Mat4x4 RobotMerger::Mat4x4::Identity() {
    Mat4x4 r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

RobotMerger::Mat4x4 RobotMerger::Mat4x4::operator*(const Mat4x4 &rhs) const {
    Mat4x4 r = {};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            for (int k = 0; k < 4; ++k)
                r.m[row * 4 + col] += m[row * 4 + k] * rhs.m[k * 4 + col];
    return r;
}

void RobotMerger::Mat4x4::TransformPoint(float &x, float &y, float &z) const {
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

void RobotMerger::Mat4x4::TransformDirection(float &x, float &y, float &z) const {
    float nx = x * m[0] + y * m[4] + z * m[8];
    float ny = x * m[1] + y * m[5] + z * m[9];
    float nz = x * m[2] + y * m[6] + z * m[10];
    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1e-8f) {
        x = nx / len;
        y = ny / len;
        z = nz / len;
    }
}

void RobotMerger::Mat4x4::TransformPointRaw(float &x, float &y, float &z) const {
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

// ── 4×4 矩阵求逆（高斯消元） ──
RobotMerger::Mat4x4 RobotMerger::Mat4x4::Inverse() const {
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
    Mat4x4 r = {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i * 4 + j] = a[i][j + 4];
    return r;
}
