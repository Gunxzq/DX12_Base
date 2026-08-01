#pragma once
// ========================================================================
// RobotMergerUtil — RobotMerger 跨文件共享辅助函数
//
// 拆分自 RobotMerger.cpp（2026-08-01，适度拆分以缓解 C1060 编译堆不足）：
//   - CreatePartMaterial：FBX 颜色材质（无纹理，assimp 6.0.4 bug 暂缓）
//   - ParseFrameToLocalMatrices：动画帧 → 每骨骼局部矩阵
// ========================================================================

#include "RobotMerger.h"

#include <cstdint>
#include <string>
#include <vector>

// 前置声明（避免引入 assimp 头）
struct aiMaterial;

namespace AssetTool {

/// 创建部件 aiMaterial（颜色材质，无纹理；名称 = 传入的 matName）
aiMaterial *CreatePartMaterial(const std::string &matName, const XFileMaterial &xf);

/// 判断两个材质是否等效（faceColor/power/specular/emissive/textureFilename 全等）
/// 用于部件合并：同骨骼 + 同材质才合并顶点，不同材质保留独立 PartData（材质不丢失）
bool SameMaterial(const XFileMaterial &a, const XFileMaterial &b);

/// 解析一帧动画数据 → 每骨骼局部矩阵（按 hod 骨骼索引对齐）
/// frameData: 帧原始字节（HOD 魔术起 9847B 或 HD2 魔术起帧块）
/// hod: 基准骨架（层级/命名）；outLocal: 输出每骨骼局部矩阵（TRS → 4×4）
bool ParseFrameToLocalMatrices(const std::vector<uint8_t> &frameData, const HODData &hod,
                               std::vector<RobotMerger::Mat4x4> &outLocal);

/// 从 ANI 第一个动画帧提取 HOD 骨架，写入 {aniDir}/{stem}_frame0.hod（标准 HOD 9847B）
/// 骨骼信息只从 ANI 来（2026-08-01 用户定案，不依赖同目录 Robo.hod）。
/// 返回写入的 .hod 路径；失败返回空串（err 带原因）。
std::string WriteFirstFrameHOD(const std::string &aniPath, const std::string &stem, std::string &err);

} // namespace AssetTool
