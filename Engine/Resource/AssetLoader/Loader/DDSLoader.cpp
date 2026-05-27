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
    if (!bitData || bitSize == 0) {
        return false;
    }

    outSkipMip = 0;
    outSubresources.clear();

    size_t width = info.desc.Width;
    size_t height = info.desc.Height;
    size_t depth = (info.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? info.desc.DepthOrArraySize : 1;
    size_t mipCount = info.desc.MipLevels;
    size_t arraySize = info.desc.DepthOrArraySize;
    DXGI_FORMAT format = info.desc.Format;

    size_t NumBytes = 0;
    size_t RowBytes = 0;
    const uint8_t *pSrcBits = bitData;
    const uint8_t *pEndBits = bitData + bitSize;

    size_t totalSubresources = mipCount * arraySize;
    outSubresources.reserve(totalSubresources);

    size_t twidth = 0;
    size_t theight = 0;
    size_t tdepth = 0;

    for (size_t arrayIdx = 0; arrayIdx < arraySize; ++arrayIdx) {
        size_t w = width;
        size_t h = height;
        size_t d = depth;

        for (size_t mipIdx = 0; mipIdx < mipCount; ++mipIdx) {
            GetSurfaceInfo(w, h, format, &NumBytes, &RowBytes, nullptr);

            if ((mipCount <= 1) || !maxsize || (w <= maxsize && h <= maxsize && d <= maxsize)) {
                if (twidth == 0) {
                    twidth = w;
                    theight = h;
                    tdepth = d;
                }

                D3D12_SUBRESOURCE_DATA subData;
                subData.pData = pSrcBits;
                subData.RowPitch = static_cast<UINT>(RowBytes);
                subData.SlicePitch = static_cast<UINT>(NumBytes);
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

    DXGI_FORMAT format = GetDXGIFormat(header->ddspf);

    // 检查是否需要 DX10 扩展头
    const DDS_HEADER_DXT10 *dxt10Header = nullptr;
    size_t pixelDataOffset = sizeof(uint32_t) + sizeof(DDS_HEADER);

    if (format == DXGI_FORMAT_UNKNOWN && (header->ddspf.flags & DDS_FOURCC) &&
        header->ddspf.fourCC == MAKEFOURCC('D', 'X', '1', '0')) {

        if (fileSize < pixelDataOffset + sizeof(DDS_HEADER_DXT10))
            return false;

        dxt10Header = reinterpret_cast<const DDS_HEADER_DXT10 *>(fileData + pixelDataOffset);
        format = dxt10Header->dxgiFormat;
        pixelDataOffset += sizeof(DDS_HEADER_DXT10);

        if (format == DXGI_FORMAT_UNKNOWN)
            return false;
    } else if (format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    // 确定资源维度和数组大小
    UINT arraySize = 1;
    D3D12_RESOURCE_DIMENSION resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    bool isCubeMap = false;

    if (header->flags & DDS_HEADER_FLAGS_VOLUME) {
        resDim = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    } else {
        if (dxt10Header) {
            // 从 DX10 头获取维度和数组大小
            switch (dxt10Header->resourceDimension) {
            case 3: // D3D11_RESOURCE_DIMENSION_TEXTURE3D
                resDim = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
                break;
            case 4: // D3D11_RESOURCE_DIMENSION_TEXTURE2D
            default:
                resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                break;
            }
            arraySize = dxt10Header->arraySize;
            isCubeMap = (dxt10Header->miscFlag & 0x4) != 0; // D3D11_RESOURCE_MISC_TEXTURECUBE
        } else {
            if ((header->caps2 & DDS_CUBEMAP) && (header->caps2 & DDS_CUBEMAP_ALLFACES) == DDS_CUBEMAP_ALLFACES) {
                arraySize = 6;
                isCubeMap = true;
            }
        }
        resDim = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    }

    // 计算实际 Mip 级别数
    UINT mipLevels = (header->mipMapCount == 0) ? 1 : static_cast<UINT16>(header->mipMapCount);
    if (mipLevels == 1 && header->mipMapCount == 0) {
        // 完整 mip 链，计算实际数量
        UINT w = header->width, h = header->height;
        mipLevels = 1;
        while (w > 1 || h > 1) {
            w = std::max(1u, w >> 1);
            h = std::max(1u, h >> 1);
            mipLevels++;
        }
    }

    // 填充 D3D12_RESOURCE_DESC
    outInfo.desc = {};
    outInfo.desc.Dimension = resDim;
    outInfo.desc.Alignment = 0;
    outInfo.desc.Width = header->width;
    outInfo.desc.Height = header->height;
    outInfo.desc.DepthOrArraySize = (resDim == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? static_cast<UINT16>(header->depth)
                                                                                   : static_cast<UINT16>(arraySize);
    outInfo.desc.MipLevels = static_cast<UINT16>(mipLevels);
    outInfo.desc.Format = format;
    outInfo.desc.SampleDesc.Count = 1;
    outInfo.desc.SampleDesc.Quality = 0;
    outInfo.desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    outInfo.desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    outInfo.isCubeMap = isCubeMap;

    // 计算像素数据偏移（无 DX10 扩展，偏移固定）
    if (pixelDataOffset >= fileSize)
        return false;

    outInfo.pixelData = fileData + pixelDataOffset;
    outInfo.pixelDataSize = fileSize - pixelDataOffset;

    // 填充子资源数据
    uint32_t skipMip;
    if (!FillSubresourceData(outInfo.pixelData, outInfo.pixelDataSize, outInfo, 0, outInfo.subresources, skipMip)) {
        return false;
    }

    return true;
}

bool DDSLoader::LoadFromMemory(const uint8_t *data, size_t dataSize, DDSTextureInfo &outInfo) {
    if (!data || dataSize == 0)
        return false;
    return ParseDDS(data, dataSize, outInfo);
}

} // namespace DX12Engine::Resource