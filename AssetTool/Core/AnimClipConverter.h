#pragma once
// ========================================================================
// AnimClipConverter — 动画 FBX → .anim 剪辑（anim2clip）
//
// 分工（2026-08-01 用户定案）：Blender 生产动画 FBX → AssetTool 转换 .anim
//   → 引擎 AnimationManager 播放。引擎不做动画编辑（非建模软件）。
//
// 数据流：
//   FBX AnimStack（aiAnimation）→ 每个剪辑一个 .anim（JSON）
//   - 通道名去 _bone 后缀、过滤 _end 末端节点（与 FbxMeshConverter 一致）
//   - position/rotation/scale 键帧按时间轴合并
//   - 坐标系：直接取 FBX 键帧值（与 .bone 的 rest pose 同源同约定，
//     保证动画采样矩阵与骨架层级相乘自洽；不额外翻转 Z——
//     若蒙皮验证发现坐标系不一致，.bone/.anim/顶点三处统一修正）
//
// 格式规范见 Docs/architecture/animation/AnimationAsset.md §二/§四
// ========================================================================

#include <string>
#include <vector>

namespace AssetTool {

/// 转换选项
struct AnimClipOptions {
    /// 只导出指定剪辑（空 = 全部）；名字匹配 FBX 动画名
    std::vector<std::string> clipFilter;
    /// 是否循环（缺省 true；后续可加 --noloop 控制）
    bool loop = true;
};

/// 转换结果
struct AnimClipResult {
    bool success = false;
    std::string error;
    std::string stem;                     // 文件名主干
    int clipCount = 0;                    // 导出剪辑数
    int channelCount = 0;                 // 总通道数
    int totalKeyframes = 0;               // 总键帧数
    std::vector<std::string> outputFiles; // 输出的 .anim 路径
};

/// 动画 FBX → .anim 剪辑转换器
class AnimClipConverter {
public:
    /// 执行转换：FBX → 每 AnimStack 一个 {name}.anim（JSON）
    static AnimClipResult Convert(const std::string &fbxPath, const std::string &outputDir,
                                  const AnimClipOptions &options = {});
};

} // namespace AssetTool
