#pragma once
// ========================================================================
// RobotMerger — HOD + .x 部件合并核心逻辑
//
// 独立于 CLI/GUI 前端，提供统一的机体合并入口。
// 流程：解析 HOD → 筛选部件 → assimp 解析 .x → LR 交换
//       → 骨骼矩阵修正 (Body_d + 层级累乘 + Ry180)
//       → 输出 .dxmesh + hod.json + scene.json + 材质/纹理
// ========================================================================

#include "Asset/Definitions/Mesh/DxMeshFormat.h"
#include "HODParser.h"
#include "XFileParser.h"

#include <functional>
#include <string>
#include <vector>

namespace AssetTool {

/// 合并选项
struct RobotMergeOptions {
    bool lrSwap = true;     // 交换 _r 后缀部件与左部件的 boneIndex
    bool leftHanded = true; // dxmesh 输出左手系（右手 Z-up → 左手 Y-up，翻转 Z）
};

/// 合并结果
struct RobotMergeResult {
    bool success = false;
    std::string error;
    std::string stem; // 文件名主干 (e.g. "Robo")
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
    static RobotMergeResult Merge(const std::string &hodPath, const std::string &outputDir,
                                  const RobotMergeOptions &options = {});

    /// 带进度回调的合并（用于 GUI）
    /// callback(idx, total, message) 返回 false 可取消
    using ProgressCallback = std::function<bool(int idx, int total, const std::string &msg)>;
    static RobotMergeResult MergeWithCallback(const std::string &hodPath, const std::string &outputDir,
                                              const RobotMergeOptions &options, ProgressCallback callback = nullptr,
                                              const std::string *stemOverride = nullptr);

    /// 从 ANI 母版驱动合并：解析 Script.ani 母版骨架（部件名 + A/B 层级），
    /// 绑定矩阵由 ANI 首帧提取的 HOD 提供（不依赖同目录 Robo.hod），合并 .x 部件输出 dxmesh/hod.json/scene.json
    /// 母版骨架用于校验（部件数/部件名与 HOD 一致性），矩阵与合并流程复用 HOD
    static RobotMergeResult MergeFromANI(const std::string &aniPath, const std::string &outputDir,
                                         const RobotMergeOptions &options = {});

    /// 导出动画 FBX（B2.5）：解析 Script.ani 各动画组帧数据（标准 HOD 9847B，
    /// HODParser 直解），每帧每骨骼 TRS → aiNodeAnim 关键帧 → aiAnimation
    /// 输出 {outputDir}/{stem}_anim.fbx（独立动画文件，含骨骼节点树 + 动画通道）
    static RobotMergeResult ExportAnimationsFBX(const std::string &aniPath, const std::string &outputDir,
                                                const std::string &stem);

    /// 4×4 行主序矩阵（动画帧解析/层级累乘用）
    struct Mat4x4 {
        float m[16];
        static Mat4x4 Identity();
        Mat4x4 operator*(const Mat4x4 &rhs) const;
        Mat4x4 Inverse() const;
        void TransformPoint(float &x, float &y, float &z) const;
        void TransformDirection(float &x, float &y, float &z) const;
        void TransformPointRaw(float &x, float &y, float &z) const;
    };

private:
    // 筛选可渲染部件
    static bool IsRenderBone(const std::string &name);

    struct PartData {
        std::string name;
        int boneIndex = -1;
        int originalBoneIndex = -1;
        XFileMesh mesh;
    };
};

} // namespace AssetTool
