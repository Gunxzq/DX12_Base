#pragma once
// ========================================================================
// MPDParser — UKW .mpd 地图瓦片文件解析器
//
// .mpd 格式（混合格式）：
//   头部：魔术头 "MPD " (4B) + 头信息
//   名称表：128B 固定条目的 .x 文件名列表
//   坐标段：二进制 tile 位置数据（格式因地图而异）
//   配置段：明文文本指令（SPT 风格）
//     LightColor(r,g,b); LightDir(x,y,z); FogColor(r,g,b);
//     BgmFileName(idx,name); PopInfo(idx, ...); AreaHitXFile;
// ========================================================================

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace AssetTool {

/// MPD 中的一个瓦片定义
struct MPDTile {
    std::string name;              // 文件名，如 "m_z00_x00.x"
    uint32_t tileIndex = 0;        // 板块编号（0-based）
    float posX = 0.0f;             // X 坐标
    float posZ = 0.0f;             // Z 坐标（原命名 posY 为历史遗留）
    std::vector<uint8_t> rawData;
    std::string renderText;        // 关联的渲染参数文本
};

/// MPD 中的一个 PopInfo 出生点
struct MPDPopInfo {
    int popIndex = 0;            // PopInfo 编号
    float posX = 0.0f;           // 出生点 X 坐标
    float posY = 0.0f;           // 出生点 Y 坐标（高度）
    float posZ = 0.0f;           // 出生点 Z 坐标
    int param1 = 0;              // 参数 1（uint16）
    int param2 = 0;              // 参数 2（uint16）
    std::string rawText;         // 原始文本行
};

/// MPD 解析结果
struct MPDData {
    std::string filepath;          // 原始 .mpd 文件路径
    std::vector<MPDTile> tiles;    // 所有瓦片
    std::vector<std::string> tileNames;  // 文件名列表
    std::vector<uint8_t> rawHex;   // 原始 hex 数据
    std::vector<std::string> textSections; // 明文标记段原始内容

    // ---- 文本配置段（从 MPD 明文指令中提取） ----
    int lightR = 255, lightG = 255, lightB = 255;
    int fogR = 0, fogG = 0, fogB = 0;
    float lightDirX = 0.3f, lightDirY = -0.7f, lightDirZ = 0.5f;
    std::string bgmFile;           // BGM 文件名
    std::vector<std::string> popInfo; // PopInfo 行原文
    std::vector<MPDPopInfo> popInfoEntries; // PopInfo 结构化条目

    // ---- 00 00 80 3F 文本标记（每条 tile 的渲染参数） ----
    std::vector<std::string> renderSettings; // Shift-JIS 解码后的文本
    int renderWithAt = 0;      // 含 @ 标记的条目数
    int renderWithoutAt = 0;   // 不含 @ 标记的条目数

    uint32_t TileCount() const { return static_cast<uint32_t>(tiles.size()); }

    /// 用真实资产目录的 .x 文件列表过滤名字表，剔除不存在的，重算索引
    void FilterByExistingFiles(const std::vector<std::string> &existingFiles);

    /// 输出为可读文本
    std::string ToText() const;

    /// 写入文本文件
    bool WriteText(const std::string &outputPath) const;
};

class MPDParser {
public:
    MPDParser() = default;

    /// 解析已解密的 .mpd 数据
    bool Parse(const uint8_t *data, size_t size);

    /// 从文件加载并解析（自动解密）
    bool ParseFile(const std::string &filepath, uint32_t decryptKey = 0x0B7E7759);

    /// 获取解析结果
    const MPDData &GetResult() const { return m_result; }

    /// 获取错误信息
    const std::string &GetError() const { return m_error; }

private:
    MPDData m_result;
    std::string m_error;

    /// 扫描二进制提取所有 .x 文件名
    void ExtractTileNames(const uint8_t *data, size_t size);

    /// 解析坐标段：搜索 [index] 00 01 模式
    void ParseCoordinateSection(const uint8_t *data, size_t size);

    /// 提取文本配置指令（LightColor, FogColor, BGM, PopInfo 等）
    void ExtractConfigText(const uint8_t *data, size_t size);

    /// 提取 00 00 80 3F 后的文本标记（渲染参数，Shift-JIS 编码）
    void ExtractRenderSettings(const uint8_t *data, size_t size);

    /// 提取 PopInfo 出生点数据（三标记模式）
    void ExtractPopInfo(const uint8_t *data, size_t size);
};

} // namespace AssetTool
