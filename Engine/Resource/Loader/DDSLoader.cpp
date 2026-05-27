#include "DDSLoader.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>

namespace DX12Engine::Resource {

// ============================================================================
// DDS 文件结构定义（与 DDSTextureLoader.cpp 一致）
// ============================================================================
#pragma pack(push, 1)

const uint32_t DDS_MAGIC = 0x20534444; // "DDS "

struct DDS_PIXELFORMAT {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t RGBBitCount;
    uint32_t RBitMask;
    uint32_t GBitMask;
    uint32_t BBitMask;
    uint32_t ABitMask;
};

#define DDS_FOURCC 0x00000004
#define DDS_RGB 0x00000040
#define DDS_LUMINANCE 0x00020000
#define DDS_ALPHA 0x00000002
#define DDS_HEADER_FLAGS_VOLUME 0x00800000
#define DDS_CUBEMAP_POSITIVEX 0x00000600
#define DDS_CUBEMAP_NEGATIVEX 0x00000a00
#define DDS_CUBEMAP_POSITIVEY 0x00001200
#define DDS_CUBEMAP_NEGATIVEY 0x00002200
#define DDS_CUBEMAP_POSITIVEZ 0x00004200
#define DDS_CUBEMAP_NEGATIVEZ 0x00008200
#define DDS_CUBEMAP_ALLFACES                                                                                           \
    (DDS_CUBEMAP_POSITIVEX | DDS_CUBEMAP_NEGATIVEX | DDS_CUBEMAP_POSITIVEY | DDS_CUBEMAP_NEGATIVEY |                   \
     DDS_CUBEMAP_POSITIVEZ | DDS_CUBEMAP_NEGATIVEZ)
#define DDS_CUBEMAP 0x00000200

struct DDS_HEADER {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};

struct DDS_HEADER_DXT10 {
    DXGI_FORMAT dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};

#pragma pack(pop)

// ============================================================================
// 辅助函数：BitsPerPixel
// ============================================================================
static size_t BitsPerPixel(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 128;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 96;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return 64;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return 32;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
        return 16;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
        return 8;
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return 4;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return 8;
    default:
        return 0;
    }
}

// ============================================================================
// 辅助函数：GetSurfaceInfo
// ============================================================================
void DDSLoader::GetSurfaceInfo(size_t width, size_t height, DXGI_FORMAT format, size_t &outNumBytes,
                               size_t &outRowBytes) {
    size_t numBytes = 0;
    size_t rowBytes = 0;
    bool bc = false;
    size_t bpe = 0;

    switch (format) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        bc = true;
        bpe = 8;
        break;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        bc = true;
        bpe = 16;
        break;
    }

    if (bc) {
        size_t numBlocksWide = (width + 3) / 4;
        size_t numBlocksHigh = (height + 3) / 4;
        rowBytes = numBlocksWide * bpe;
        numBytes = rowBytes * numBlocksHigh;
    } else {
        size_t bpp = BitsPerPixel(format);
        rowBytes = (width * bpp + 7) / 8;
        numBytes = rowBytes * height;
    }

    outNumBytes = numBytes;
    outRowBytes = rowBytes;
}

// ============================================================================
// 辅助函数：GetDXGIFormat
// ============================================================================
#define ISBITMASK(r, g, b, a) (ddpf.RBitMask == r && ddpf.GBitMask == g && ddpf.BBitMask == b && ddpf.ABitMask == a)

DXGI_FORMAT DDSLoader::GetDXGIFormat(const void *pixelFormat) {
    const DDS_PIXELFORMAT &ddpf = *reinterpret_cast<const DDS_PIXELFORMAT *>(pixelFormat);

    if (ddpf.flags & DDS_RGB) {
        switch (ddpf.RGBBitCount) {
        case 32:
            if (ISBITMASK(0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000))
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            if (ISBITMASK(0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000))
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            if (ISBITMASK(0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000))
                return DXGI_FORMAT_R10G10B10A2_UNORM;
            break;
        case 16:
            if (ISBITMASK(0x7c00, 0x03e0, 0x001f, 0x8000))
                return DXGI_FORMAT_B5G5R5A1_UNORM;
            if (ISBITMASK(0xf800, 0x07e0, 0x001f, 0x0000))
                return DXGI_FORMAT_B5G6R5_UNORM;
            break;
        }
    } else if (ddpf.flags & DDS_LUMINANCE) {
        if (8 == ddpf.RGBBitCount && ISBITMASK(0x000000ff, 0, 0, 0))
            return DXGI_FORMAT_R8_UNORM;
        if (16 == ddpf.RGBBitCount && ISBITMASK(0x0000ffff, 0, 0, 0))
            return DXGI_FORMAT_R16_UNORM;
    } else if (ddpf.flags & DDS_ALPHA) {
        if (8 == ddpf.RGBBitCount)
            return DXGI_FORMAT_A8_UNORM;
    } else if (ddpf.flags & DDS_FOURCC) {
        if (MAKEFOURCC('D', 'X', 'T', '1') == ddpf.fourCC)
            return DXGI_FORMAT_BC1_UNORM;
        if (MAKEFOURCC('D', 'X', 'T', '3') == ddpf.fourCC)
            return DXGI_FORMAT_BC2_UNORM;
        if (MAKEFOURCC('D', 'X', 'T', '5') == ddpf.fourCC)
            return DXGI_FORMAT_BC3_UNORM;
        if (MAKEFOURCC('A', 'T', 'I', '1') == ddpf.fourCC || MAKEFOURCC('B', 'C', '4', 'U') == ddpf.fourCC)
            return DXGI_FORMAT_BC4_UNORM;
        if (MAKEFOURCC('B', 'C', '4', 'S') == ddpf.fourCC)
            return DXGI_FORMAT_BC4_SNORM;
        if (MAKEFOURCC('A', 'T', 'I', '2') == ddpf.fourCC || MAKEFOURCC('B', 'C', '5', 'U') == ddpf.fourCC)
            return DXGI_FORMAT_BC5_UNORM;
        if (MAKEFOURCC('B', 'C', '5', 'S') == ddpf.fourCC)
            return DXGI_FORMAT_BC5_SNORM;
    }

    return DXGI_FORMAT_UNKNOWN;
}

// ============================================================================
// 辅助函数：FillSubresourceData
// ============================================================================
bool DDSLoader::FillSubresourceData(const uint8_t *bitData, size_t bitSize, const DDSTextureInfo &info, size_t maxsize,
                                    std::vector<D3D12_SUBRESOURCE_DATA> &outSubresources, uint32_t &outSkipMip) {
    outSkipMip = 0;
    outSubresources.clear();

    size_t NumBytes = 0;
    size_t RowBytes = 0;
    const uint8_t *pSrcBits = bitData;
    const uint8_t *pEndBits = bitData + bitSize;

    size_t width = info.width;
    size_t height = info.height;
    size_t depth = info.depth;

    size_t totalSubresources = info.mipLevels * info.arraySize;
    outSubresources.reserve(totalSubresources);

    for (size_t arrayIdx = 0; arrayIdx < info.arraySize; ++arrayIdx) {
        size_t w = width;
        size_t h = height;
        size_t d = depth;

        for (size_t mipIdx = 0; mipIdx < info.mipLevels; ++mipIdx) {
            GetSurfaceInfo(w, h, info.format, NumBytes, RowBytes);

            bool useMip = (info.mipLevels <= 1) || (maxsize == 0) || (w <= maxsize && h <= maxsize && d <= maxsize);

            if (useMip) {
                D3D12_SUBRESOURCE_DATA subData;
                subData.pData = pSrcBits;
                subData.RowPitch = static_cast<UINT>(RowBytes);
                subData.SlicePitch = static_cast<UINT>(NumBytes * d);
                outSubresources.push_back(subData);
            } else if (arrayIdx == 0) {
                ++outSkipMip;
            }

            if (pSrcBits + (NumBytes * d) > pEndBits) {
                return false;
            }

            pSrcBits += NumBytes * d;

            w = w >> 1;
            h = h >> 1;
            d = d >> 1;
            if (w == 0)
                w = 1;
            if (h == 0)
                h = 1;
            if (d == 0)
                d = 1;
        }
    }

    return !outSubresources.empty();
}

// ============================================================================
// 辅助函数：ParseDDS
// ============================================================================
bool DDSLoader::ParseDDS(const uint8_t *fileData, size_t fileSize, DDSTextureInfo &outInfo) {
    // 验证 magic number
    if (fileSize < sizeof(uint32_t) + sizeof(DDS_HEADER))
        return false;
    if (*(const uint32_t *)fileData != DDS_MAGIC)
        return false;

    const DDS_HEADER *header = reinterpret_cast<const DDS_HEADER *>(fileData + sizeof(uint32_t));

    // 验证头部
    if (header->size != sizeof(DDS_HEADER) || header->ddspf.size != sizeof(DDS_PIXELFORMAT))
        return false;

    // 检查 DX10 扩展
    bool hasDX10Ext = false;
    if ((header->ddspf.flags & DDS_FOURCC) && MAKEFOURCC('D', 'X', '1', '0') == header->ddspf.fourCC) {
        if (fileSize < sizeof(uint32_t) + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10))
            return false;
        hasDX10Ext = true;
    }

    // 获取格式
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t arraySize = 1;
    uint32_t resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    bool isCubeMap = false;

    if (hasDX10Ext) {
        const DDS_HEADER_DXT10 *ext =
            reinterpret_cast<const DDS_HEADER_DXT10 *>(fileData + sizeof(uint32_t) + sizeof(DDS_HEADER));
        format = ext->dxgiFormat;
        arraySize = ext->arraySize;
        if (arraySize == 0)
            return false;

        switch (ext->resourceDimension) {
        case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
            resDim = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
            resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            if (ext->miscFlag & D3D11_RESOURCE_MISC_TEXTURECUBE) {
                arraySize *= 6;
                isCubeMap = true;
            }
            break;
        case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
            resDim = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            if (!(header->flags & DDS_HEADER_FLAGS_VOLUME))
                return false;
            if (arraySize > 1)
                return false;
            break;
        default:
            return false;
        }
    } else {
        format = GetDXGIFormat(&header->ddspf);
        if (format == DXGI_FORMAT_UNKNOWN)
            return false;

        if (header->flags & DDS_HEADER_FLAGS_VOLUME) {
            resDim = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        } else {
            if ((header->caps2 & DDS_CUBEMAP) && (header->caps2 & DDS_CUBEMAP_ALLFACES) == DDS_CUBEMAP_ALLFACES) {
                arraySize = 6;
                isCubeMap = true;
            }
            resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        }
    }

    // 填充输出信息
    outInfo.width = header->width;
    outInfo.height = header->height;
    outInfo.depth = (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? header->depth : 1;
    outInfo.format = format;
    outInfo.mipLevels = (header->mipMapCount == 0) ? 1 : header->mipMapCount;
    outInfo.arraySize = arraySize;
    outInfo.isCubeMap = isCubeMap;
    outInfo.resourceDimension = (D3D12_RESOURCE_DIMENSION)resDim;

    // 计算像素数据偏移
    size_t offset = sizeof(uint32_t) + sizeof(DDS_HEADER);
    if (hasDX10Ext)
        offset += sizeof(DDS_HEADER_DXT10);

    if (offset >= fileSize)
        return false;

    outInfo.pixelData = fileData + offset;
    outInfo.pixelDataSize = fileSize - offset;

    return true;
}

// ============================================================================
// 公开接口实现
// ============================================================================

/**
 * @brief 从文件加载 DDS 纹理信息
 * @param path 文件路径
 * @param outInfo
 * @return bool
 * @date 2026-05-27
 */
bool DDSLoader::LoadFromFile(const std::wstring &path, DDSTextureInfo &outInfo) {
    // 打开文件
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    // 获取文件大小
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // 读取文件数据
    std::vector<uint8_t> fileData(fileSize);
    file.read(reinterpret_cast<char *>(fileData.data()), fileSize);
    file.close();

    // 解析 DDS
    return ParseDDS(fileData.data(), fileSize, outInfo);
}

/**
 * @brief 从内存加载 DDS 纹理信息
 * @param data DDS 数据指针
 * @param dataSize 数据大小
 * @param outInfo
 * @return bool
 * @date 2026-05-27
 */
bool DDSLoader::LoadFromMemory(const uint8_t *data, size_t dataSize, DDSTextureInfo &outInfo) {
    if (!data || dataSize == 0)
        return false;
    return ParseDDS(data, dataSize, outInfo);
}

} // namespace DX12Engine::Resource