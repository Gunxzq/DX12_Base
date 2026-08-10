#pragma once
// ========================================================================
// MPDSceneParser — UKW .mpd 地图权威解析器（社区源码 mpd.cs 结构）
//
// 2026-08-03 依据 WindomXP-Map-Editor（MugenAttack）mpd.cs 权威结构重写，
// 取代旧 MPDParser 的逆向猜测实现（FFFF0001 / Type A-B 标记均作废）。
//
// 权威结构（详见 Docs/targets/UKW_PowerUpKit/05_MPD_Format_Analysis.md §〇）：
//   [头部] "MPD"(3B) + int32 WorldParts(=20000) + int16 PiecesCount(=200)
//   [Pieces] PiecesCount 条 × (256B visual + 256B collision + 3B 填充
//             + int32 脚本长 + 脚本 SJIS)
//   [Grid]  int16 x(=100) + int16 y(=100) + float unk(=30)
//           + x×y 格 × (int32 count + count × (16×float 列主序矩阵
//             + int16 pieceID + int32 脚本长 + 脚本 SJIS))
//
// 要点：对象矩阵 = 完整世界变换（列主序），平移列 m03,m13,m23 即世界坐标；
//       格子仅是位置/30 归桶；MPD 本身不加密（.x/.spt 亦明文）。
// ========================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace AssetTool {

/// MPD piece（网格名对 + 条级脚本）
struct MPDScenePiece {
    std::string visualMesh;    // 如 "mapChip03.x"（UTF-8）
    std::string collisionMesh; // 如 "mapChip03hit.x"（可为空）
    std::string scriptText;    // 条级脚本（可为空）
};

/// MPD 对象实例（grid 内）
struct MPDSceneObject {
    float matrix[16] = {}; // 列主序世界变换（m03,m13,m23 = 平移 X/Y/Z）
    int pieceID = 0;       // 索引 pieces（0-based；198=Sky.x 199=Hit.x）
    int gridX = 0;         // 所在格子 x（桶 = posX / unk）
    int gridY = 0;         // 所在格子 y（桶 = posZ / unk）
    std::string script;    // 对象级脚本（@CullFar=50; 等，UTF-8）
};

/// MPD 权威解析结果
struct MPDScene {
    std::string filepath;
    std::vector<MPDScenePiece> pieces;
    std::vector<MPDSceneObject> objects; // 全部对象（跨格子）
    int gridX = 0, gridY = 0;            // 格数（100×100）
    float unk = 0.0f;                    // 格尺寸（30.0f）
    uint32_t worldParts = 0;             // 固定 20000

    size_t ObjectCount() const { return objects.size(); }
    size_t PieceCount() const { return pieces.size(); }
};

/// MPD 权威解析器（按社区编辑器 mpd.cs 结构）
class MPDSceneParser {
public:
    MPDSceneParser() = default;

    /// 从文件解析（自动检测 XOR 加密；MPD 通常明文，先试明文）
    bool ParseFile(const std::string &filepath);

    /// 从内存解析（已解密数据）
    bool Parse(const uint8_t *data, size_t size);

    const MPDScene &GetResult() const { return m_scene; }
    const std::string &GetError() const { return m_error; }

private:
    static std::string ReadFixedString(const uint8_t *data, size_t size, size_t &off, size_t len);
    static std::string ReadVarString(const uint8_t *data, size_t size, size_t &off, uint32_t len);
    static std::string CP932ToUTF8(const std::string &sjis);

    MPDScene m_scene;
    std::string m_error;
};

} // namespace AssetTool
