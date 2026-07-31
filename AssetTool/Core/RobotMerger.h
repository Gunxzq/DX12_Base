#pragma once
// ========================================================================
// RobotMerger — HOD + .x 部件合并核心逻辑
//
// 独立于 CLI/GUI 前端，提供统一的机体合并入口。
// 流程：解析 HOD → 筛选部件 → assimp 解析 .x → LR 交换
//       → 骨骼矩阵修正 (Body_d + 层级累乘 + Ry180)
//       → 输出 .dxmesh + hod.json + scene.json + 材质/纹理
// ========================================================================

#include "HODParser.h"
#include "XFileParser.h"
#include "Asset/Definitions/Mesh/DxMeshFormat.h"

#include <functional>
#include <string>
#include <vector>

namespace AssetTool {

/// 合并选项
struct RobotMergeOptions {
    bool lrSwap = true;     // 交换 _r 后缀部件与左部件的 boneIndex
    bool exportX = false;   // 导出 .x（层级帧结构，供 DE 验证）
    bool exportFBX = false; // 导出 FBX（含骨骼层级 + 蒙皮绑定）
    bool leftHanded = true;  // dxmesh 输出左手系（右手 Z-up → 左手 Y-up，翻转 Z）
};

/// 合并结果
struct RobotMergeResult {
    bool success = false;
    std::string error;
    std::string stem;              // 文件名主干 (e.g. "Robo")
    int partCount = 0;
    int vertexCount = 0;
    int indexCount = 0;
    std::vector<std::string> materialKeys; // 材质 key 列表
    std::vector<std::string> outputFiles;  // 输出的文件路径
};

/// HOD + .x 合并器
class RobotMerger {
public:
    /// 执行合并
    static RobotMergeResult Merge(const std::string &hodPath,
                                   const std::string &outputDir,
                                   const RobotMergeOptions &options = {});

    /// 带进度回调的合并（用于 GUI）
    /// callback(idx, total, message) 返回 false 可取消
    using ProgressCallback = std::function<bool(int idx, int total, const std::string &msg)>;
    static RobotMergeResult MergeWithCallback(const std::string &hodPath,
                                               const std::string &outputDir,
                                               const RobotMergeOptions &options,
                                               ProgressCallback callback = nullptr);

private:
    // 筛选可渲染部件
    static bool IsRenderBone(const std::string &name);

    // 4×4 行主序矩阵
    struct Mat4x4 {
        float m[16];
        static Mat4x4 Identity();
        Mat4x4 operator*(const Mat4x4 &rhs) const;
        Mat4x4 Inverse() const;
        void TransformPoint(float &x, float &y, float &z) const;
        void TransformDirection(float &x, float &y, float &z) const;
        void TransformPointRaw(float &x, float &y, float &z) const;
    };

    struct PartData {
        std::string name;
        int boneIndex = -1;
        int originalBoneIndex = -1;
        XFileMesh mesh;
    };
};

} // namespace AssetTool
