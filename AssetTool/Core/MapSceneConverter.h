#pragma once
// ========================================================================
// MapSceneConverter — UKW 地图场景管线：mpd2scene（拆解 + 合成）
//
// 依据 Docs/targets/UKW_PowerUpKit/08_MapScenePipeline.md（2026-08-03 定案）：
//   拆解：每唯一 piece .x → 子网格 dxmesh（SubMesh 表）+ 材质内容去重 + 纹理 XOR 解密
//   合成：MPD 对象实例（矩阵→TRS）+ SPT 环境 → scene.json（符合 Schemas/scene.schema.json）
//
// 复用：MPDSceneParser（权威结构）+ XFileParser（assimp 子网格）+ XORCipher
//       + XFileMaterial::ToMaterialDesc + DxMeshWriter（支持 SubMesh 表）
// 定案要点：
//   - materials 内联展开 MaterialDefinition，不输出独立 .mat
//   - 坐标系：右手 Y-up → 引擎左手系 Y-up（顶点/法线/切线 Z 取反 + 索引绕序翻转 (i0,i2,i1)
//     + 实例矩阵 Z 列取反），复用 FbxMeshConverter/importrobot 的 leftHanded 实现
//   - 材质槽数组 mesh.materials[] 长度 = dxmesh SubMesh 数，[i] 对应第 i 个子网格
//   - 透明 piece：对象脚本 @AlphaTestFlag 或子网格材质 alpha < 1
//   - 天空：同目录 Sky.png → dependencies.textures；几何程序化 {"type":"cube"}；
//     LoadSkyXFile 颜色 RGB/255 作兜底
//   - 不转换：Hit 碰撞盒、item 出生点、point_* 出生点/物品标记、水、BGM、MapSetting
// ========================================================================

#include <string>

namespace AssetTool {

/// mpd2scene 转换选项
struct MapSceneOptions {
    std::string mapDir;     // 含 map.mpd + *.x + *.dds + Script.spt 的目录（如 map/City）
    std::string outDir;     // 输出目录（Content/City 根；其下建 Meshes/ Textures/ Scenes/）
    bool leftHanded = true; // 右手 Y-up → 引擎左手系 Y-up（翻转 Z）
};

/// mpd2scene 转换结果统计
struct MapSceneResult {
    int pieceCount = 0;    // 唯一 piece dxmesh 数
    int instanceCount = 0; // 生成的实例实体数
    int materialCount = 0; // 去重后唯一材质数
    int textureCount = 0;  // 输出的纹理数
    std::string scenePath; // 输出的 scene.json 路径
    std::string error;     // 失败原因（error 非空即失败）
};

/// 地图场景转换器（拆解 + 合成一步完成）
class MapSceneConverter {
public:
    /// 执行转换。成功返回 true 并填充 result；失败返回 false（result.error 含原因）
    bool Convert(const MapSceneOptions &options, MapSceneResult &result);
};

} // namespace AssetTool
