#pragma once
// ========================================================================
// FbxMeshConverter — Blender 优化后 FBX → 引擎资产（fbxs2dxmesh）
//
// 路线定案（2026-08-01）：引擎资产唯一来源 = Blender 优化后的最终 FBX。
// importrobot（.x 直接拼接）退役，本转换器替代其产出：
//   .dxmesh（DxMeshSkinnedVertex 蒙皮格式）
//   .bone（骨架 JSON，以 FBX Armature 解析结果为准）
//   Materials/*.mat + Textures/*.dds + scene.json
//
// 关键决策（详见 Docs/targets/UKW_PowerUpKit/07_EngineAssetPipeline.md §三）：
//   1. 子网格按材质槽拆，每子网格顶点从 FBX 权重读骨骼（不靠文件名推断）
//   2. 骨骼名去 _bone 后缀、过滤 _end 末端节点；TRS 从 FBX 节点变换取
//   3. Blender Principled BSDF → 引擎 .mat
//   4. Y-up 右手系 → 引擎左手 Y-up（翻转 Z）
// ========================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace AssetTool {

/// 转换选项
struct FbxConvertOptions {
    bool leftHanded = true; // dxmesh/bone 输出左手系 Y-up（翻转 Z）
};

/// 转换结果
struct FbxConvertResult {
    bool success = false;
    std::string error;
    std::string stem; // 文件名主干（e.g. "KD-03"）
    int meshCount = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int boneCount = 0;                     // .bone 骨骼数（过滤 _end 后）
    std::vector<std::string> materialKeys; // 材质 key 列表（与子网格顺序一致）
    std::vector<std::string> outputFiles;  // 输出的文件路径
};

/// FBX → 引擎资产转换器
class FbxMeshConverter {
public:
    /// 执行转换：FBX → .dxmesh + .bone + Materials/*.mat + Textures/* + scene.json
    static FbxConvertResult Convert(const std::string &fbxPath, const std::string &outputDir,
                                    const FbxConvertOptions &options = {});
};

} // namespace AssetTool
