#pragma once
// ========================================================================
// ScriptSPTParser — UKW Script.spt 场景配置文件解析器
//
// Script.spt 为 Shift-JIS 编码的文本文件，定义场景的完整配置：
//   - 天空盒 / 雾色 / 光照
//   - 水面
//   - 瓦片模型映射（LoadMapXFile）
//   - 建筑放置（SetBuilding）
//   - 地图设置（MapSetting）
//   - BGM
//
// 输出：scene.json 供引擎直接加载
// ========================================================================

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace AssetTool {

/// 瓦片映射：索引 → .x 文件名
struct MapTileEntry {
    int index = 0;
    std::string modelFile; // 模型 .x 文件
    std::string hitFile;   // 碰撞 .x 文件
};

/// 建筑实例
struct BuildingInstance {
    int mapNo = 0;                      // 所属瓦片
    int index = 0;                      // 建筑物在瓦片内的序号
    int buildNo = 0;                    // 建筑型号索引
    int hp = 0;                         // 耐久度
    float posX = 0, posY = 0, posZ = 0; // 世界坐标
};

/// 地图瓦片设置
struct MapSettingEntry {
    int index = 0;   // 瓦片索引
    int xfileNo = 0; // 模型编号
    int paramCount = 0;
    int params[10];         // MapSettingEx 扩展参数
    float waterHeight = -1; // 水面高度
};

/// 场景结果
struct SceneData {
    std::string sourceFile; // 原始 Script.spt 路径

    // 天空
    std::string skyModel; // Sky.x
    int fogR = 0, fogG = 0, fogB = 0;
    int lightR = 255, lightG = 255, lightB = 255;

    // 水面
    std::string waterModel; // Sea.x

    // BGM
    std::string bgmFile;

    // 碰撞
    std::string hitModel;

    // 材质
    std::string materialFile;

    // 建筑文件注册
    std::vector<std::string> buildingFiles;

    // 瓦片映射表
    std::vector<MapTileEntry> mapTiles;

    // 瓦片设置
    std::vector<MapSettingEntry> mapSettings;

    // 建筑实例
    std::vector<BuildingInstance> buildings;

    // 输出 JSON
    std::string ToJSON() const;

    // 输出可读文本
    std::string ToText() const;

    // 写入 JSON 文件
    bool WriteJSON(const std::string &outputPath) const;
};

class ScriptSPTParser {
public:
    ScriptSPTParser() = default;

    /// 解析脚本文件
    bool ParseFile(const std::string &filepath);

    /// 获取解析结果
    const SceneData &GetResult() const { return m_result; }

    /// 获取错误信息
    const std::string &GetError() const { return m_error; }

private:
    SceneData m_result;
    std::string m_error;

    /// 解析一行指令
    void ParseLine(const std::string &line);
};

} // namespace AssetTool
