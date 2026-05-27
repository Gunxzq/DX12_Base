#include "DDSLoader.h"
#include "AssetDefinitions/Texture/DDS/DDSUtils.cpp"
#include <algorithm>
#include <cstring>
#include <d3d12.h>
#include <fstream>
#include <memory>

namespace DX12Engine::Resource {

bool DDSLoader::FillSubresourceData(const uint8_t *bitData, size_t bitSize, const DDSTextureInfo &info, size_t maxsize,
                                    std::vector<D3D12_SUBRESOURCE_DATA> &outSubresources, uint32_t &outSkipMip) {
    outSkipMip = 0;
    outSubresources.clear();

    size_t NumBytes = 0;
    size_t RowBytes = 0;
    const uint8_t *pSrcBits = bitData;
    const uint8_t *pEndBits = bitData + bitSize;

    size_t width = info.desc.Width;
    size_t height = info.desc.Height;
    size_t depth = (info.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? info.desc.DepthOrArraySize : 1;

    size_t totalSubresources = info.desc.MipLevels * info.desc.DepthOrArraySize;
    outSubresources.reserve(totalSubresources);

    for (size_t arrayIdx = 0; arrayIdx < info.desc.DepthOrArraySize; ++arrayIdx) {
        size_t w = width;
        size_t h = height;
        size_t d = depth;

        for (size_t mipIdx = 0; mipIdx < info.desc.MipLevels; ++mipIdx) {
            GetSurfaceInfo(w, h, info.desc.Format, &NumBytes, &RowBytes, nullptr);

            bool useMip =
                (info.desc.MipLevels <= 1) || (maxsize == 0) || (w <= maxsize && h <= maxsize && d <= maxsize);

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

    // 获取格式（传统 DDS，无 DX10 扩展）
    DXGI_FORMAT format = GetDXGIFormat(header->ddspf);
    if (format == DXGI_FORMAT_UNKNOWN)
        return false;

    // 确定资源维度和数组大小
    UINT arraySize = 1;
    D3D12_RESOURCE_DIMENSION resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    bool isCubeMap = false;

    if (header->flags & DDS_HEADER_FLAGS_VOLUME) {
        resDim = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    } else {
        if ((header->caps2 & DDS_CUBEMAP) && (header->caps2 & DDS_CUBEMAP_ALLFACES) == DDS_CUBEMAP_ALLFACES) {
            arraySize = 6;
            isCubeMap = true;
        }
        resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    }

    // 填充 D3D12_RESOURCE_DESC
    outInfo.desc = {};
    outInfo.desc.Dimension = resDim;
    outInfo.desc.Alignment = 0;
    outInfo.desc.Width = header->width;
    outInfo.desc.Height = header->height;
    outInfo.desc.DepthOrArraySize = (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? static_cast<UINT16>(header->depth)
                                                                                   : static_cast<UINT16>(arraySize);
    outInfo.desc.MipLevels = (header->mipMapCount == 0) ? 1 : static_cast<UINT16>(header->mipMapCount);
    outInfo.desc.Format = format;
    outInfo.desc.SampleDesc.Count = 1;
    outInfo.desc.SampleDesc.Quality = 0;
    outInfo.desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    outInfo.desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    outInfo.isCubeMap = isCubeMap;

    // 计算像素数据偏移（无 DX10 扩展，偏移固定）
    size_t offset = sizeof(uint32_t) + sizeof(DDS_HEADER);
    if (offset >= fileSize)
        return false;

    outInfo.pixelData = fileData + offset;
    outInfo.pixelDataSize = fileSize - offset;

    // 填充子资源数据
    uint32_t skipMip;
    if (!FillSubresourceData(outInfo.pixelData, outInfo.pixelDataSize, outInfo, 0, outInfo.subresources, skipMip)) {
        return false;
    }

    return true;
}

bool DDSLoader::LoadFromFile(const std::wstring &path, DDSTextureInfo &outInfo) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return false;

    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> fileData(fileSize);
    file.read(reinterpret_cast<char *>(fileData.data()), fileSize);
    file.close();

    return ParseDDS(fileData.data(), fileSize, outInfo);
}

bool DDSLoader::LoadFromMemory(const uint8_t *data, size_t dataSize, DDSTextureInfo &outInfo) {
    if (!data || dataSize == 0)
        return false;
    return ParseDDS(data, dataSize, outInfo);
}

} // namespace DX12Engine::Resource