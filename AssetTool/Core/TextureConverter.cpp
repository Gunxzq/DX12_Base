#include "TextureConverter.h"
#include "XORCipher.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace AssetTool {

// ========================================================================
// DDS 文件格式定义（简化版，仅 R8G8B8A8_UNORM）
// ========================================================================

#pragma pack(push, 1)

/// DDS_PIXELFORMAT
struct DDSPixelFormat {
    uint32_t size = 32;
    uint32_t flags = 0x00000040; // DDPF_RGB (0x40), not DDPF_FOURCC (0x04)
    uint32_t fourCC = 0;         // 'DXT1', 'DXT3', 'DXT5', etc.
    uint32_t rgbBitCount = 32;
    uint32_t rBitMask = 0x000000FF; // DXGI_FORMAT_R8G8B8A8_UNORM: R at byte 0
    uint32_t gBitMask = 0x0000FF00; // G at byte 1
    uint32_t bBitMask = 0x00FF0000; // B at byte 2
    uint32_t aBitMask = 0xFF000000; // A at byte 3
};

/// DDS_HEADER (124 bytes)
struct DDSHeader {
    uint32_t magic = 0x20534444; // "DDS "
    uint32_t size = 124;         // sizeof(DDSHeader) - 4
    uint32_t flags = 0x0002100F; // DDSD_CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT|MIPMAPCOUNT
    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t pitchOrLinearSize = 0;
    uint32_t depth = 0;
    uint32_t mipMapCount = 1;
    uint32_t reserved1[11] = {};
    DDSPixelFormat pixelFormat;
    uint32_t caps = 0x00001000; // DDSCAPS_TEXTURE
    uint32_t caps2 = 0;
    uint32_t caps3 = 0;
    uint32_t caps4 = 0;
    uint32_t reserved2 = 0;
};
static_assert(sizeof(DDSHeader) == 128, "DDSHeader must be 128 bytes");

#pragma pack(pop)

// ========================================================================
// 实现
// ========================================================================

static bool WriteDDS(const std::string &outputPath, int width, int height,
                     const std::vector<uint8_t> &pixels) {
    DDSHeader header;
    header.width = static_cast<uint32_t>(width);
    header.height = static_cast<uint32_t>(height);
    header.pitchOrLinearSize = static_cast<uint32_t>(width * 4); // row pitch

    // Alpha flag: DDPF_RGB (0x40) | DDPF_ALPHAPIXELS (0x01) = 0x41
    header.pixelFormat.flags = 0x00000041;

    std::ofstream ofs(outputPath, std::ios::binary);
    if (!ofs)
        return false;

    ofs.write(reinterpret_cast<const char *>(&header), sizeof(header));
    ofs.write(reinterpret_cast<const char *>(pixels.data()), pixels.size());
    return true;
}

TextureConvertResult ConvertPNGToDDS(const std::string &inputPath, const std::string &outputPath) {
    TextureConvertResult result;

    // 使用 stb_image 加载 PNG
    int width = 0, height = 0, channels = 0;
    unsigned char *data = stbi_load(inputPath.c_str(), &width, &height, &channels, 4);
    if (!data) {
        result.error = "Failed to load image: ";
        result.error += stbi_failure_reason();
        return result;
    }

    // 确保输出目录存在
    fs::path outPath(outputPath);
    std::error_code ec;
    fs::create_directories(outPath.parent_path(), ec);

    // 写入 DDS
    std::vector<uint8_t> pixels(data, data + width * height * 4);
    stbi_image_free(data);

    if (!WriteDDS(outputPath, width, height, pixels)) {
        result.error = "Failed to write DDS file";
        return result;
    }

    result.success = true;
    return result;
}

int BatchConvertPNGToDDS(const std::string &inputDir, const std::string &outputDir,
                          std::string *outError) {
    fs::path inPath(inputDir);
    if (!fs::is_directory(inPath)) {
        if (outError) *outError = "Input directory does not exist";
        return 0;
    }

    int successCount = 0;
    for (const auto &entry : fs::recursive_directory_iterator(inPath)) {
        if (!entry.is_regular_file())
            continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)::tolower(c); });
        if (ext != ".png")
            continue;

        // 计算相对路径
        fs::path rel = fs::relative(entry.path(), inPath);
        fs::path outPath = fs::path(outputDir) / rel;
        outPath.replace_extension(".dds");

        auto r = ConvertPNGToDDS(entry.path().string(), outPath.string());
        if (r.success)
            ++successCount;
        else if (outError)
            *outError = r.error;
    }

    return successCount;
}

// ========================================================================
// DDS XOR 解密
// ========================================================================

TextureConvertResult DecryptDDS(const std::string &filePath, const std::string &outputPath,
                                 uint32_t xorKey) {
    TextureConvertResult result;

    // 读取文件
    std::ifstream ifs(filePath, std::ios::binary | std::ios::ate);
    if (!ifs) {
        result.error = "Cannot open file";
        return result;
    }
    size_t size = static_cast<size_t>(ifs.tellg());
    ifs.seekg(0);
    std::vector<uint8_t> data(size);
    ifs.read(reinterpret_cast<char *>(data.data()), size);
    if (!ifs) {
        result.error = "Cannot read file";
        return result;
    }

    // XOR 解密（4 字节块）
    XORCipher cipher(xorKey);
    cipher.DecryptBuffer(data.data(), data.size());

    // 验证 DDS 魔数
    if (data.size() < 4 || std::memcmp(data.data(), "DDS ", 4) != 0) {
        result.error = "Decryption failed: invalid DDS magic after XOR";
        return result;
    }

    // 写入输出
    fs::path outPath(outputPath);
    std::error_code ec;
    fs::create_directories(outPath.parent_path(), ec);

    std::ofstream ofs(outputPath, std::ios::binary);
    if (!ofs) {
        result.error = "Cannot write output file";
        return result;
    }
    ofs.write(reinterpret_cast<const char *>(data.data()), data.size());

    result.success = true;
    return result;
}

TextureConvertResult DecryptOrCopyDDS(const std::string &inputPath, const std::string &outputPath,
                                       uint32_t xorKey) {
    TextureConvertResult result;

    // 先检查是否已经是标准的 DDS 文件
    {
        std::ifstream ifs(inputPath, std::ios::binary);
        if (ifs) {
            char magic[4];
            ifs.read(magic, 4);
            if (ifs.gcount() == 4 && std::memcmp(magic, "DDS ", 4) == 0) {
                // 已经是标准 DDS，直接复制
                ifs.close();
                std::error_code ec;
                fs::copy(inputPath, outputPath, fs::copy_options::overwrite_existing, ec);
                if (!ec) {
                    result.success = true;
                    return result;
                }
                result.error = "Copy failed";
                return result;
            }
        }
    }

    // 不是标准 DDS，尝试 XOR 解密
    return DecryptDDS(inputPath, outputPath, xorKey);
}

} // namespace AssetTool
