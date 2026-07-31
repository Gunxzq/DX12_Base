#pragma once
// ========================================================================
// TextureConverter — 纹理转换器
//
// 功能：
//   1. PNG → DDS（stb_image + 手动 DDS 头）
//   2. DDS XOR 解密（原始 UKW .dds 使用 XOR 加密）
// ========================================================================

#include <string>
#include <vector>

namespace AssetTool {

/// 转换结果
struct TextureConvertResult {
    bool success = false;
    std::string error;
};

/// 转换单个 PNG 文件为 DDS
TextureConvertResult ConvertPNGToDDS(const std::string &inputPath, const std::string &outputPath);

/// 批量转换目录下所有 PNG 文件
int BatchConvertPNGToDDS(const std::string &inputDir, const std::string &outputDir,
                          std::string *outError = nullptr);

/// XOR 解密 .dds 文件（原地解密），验证 DDS 魔数
/// 使用 key 0x0B7E7759（PowerUpKit 默认）
TextureConvertResult DecryptDDS(const std::string &filePath, const std::string &outputPath,
                                 uint32_t xorKey = 0x0B7E7759);

/// 尝试解密 .dds，如果魔数已经是 "DDS " 则直接复制
TextureConvertResult DecryptOrCopyDDS(const std::string &inputPath, const std::string &outputPath,
                                       uint32_t xorKey = 0x0B7E7759);

} // namespace AssetTool
