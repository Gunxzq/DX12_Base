#include "DDSLoader.h"
#include "AssetDefinitions/Texture/DDS/DDSUtils.cpp"
#include <algorithm>
#include <cstring>
#include <d3d12.h>
#include <fstream>
#include <iostream>
#include <memory>

namespace DX12Engine::Resource {

static HRESULT FillInitDataForInfo(_In_ size_t width, _In_ size_t height, _In_ size_t depth, _In_ size_t mipCount,
                                   _In_ size_t arraySize, _In_ DXGI_FORMAT format, _In_ size_t maxsize,
                                   _In_ size_t bitSize, _In_reads_bytes_(bitSize) const uint8_t *bitData,
                                   _Out_ size_t &twidth, _Out_ size_t &theight, _Out_ size_t &tdepth,
                                   _Out_ size_t &skipMip,
                                   _Out_writes_(mipCount *arraySize) D3D12_SUBRESOURCE_DATA *initData) {
    if (!bitData || !initData)
        return E_POINTER;

    skipMip = 0;
    twidth = 0;
    theight = 0;
    tdepth = 0;

    size_t NumBytes = 0;
    size_t RowBytes = 0;
    const uint8_t *pSrcBits = bitData;
    const uint8_t *pEndBits = bitData + bitSize;

    size_t index = 0;
    for (size_t j = 0; j < arraySize; j++) {
        size_t w = width;
        size_t h = height;
        size_t d = depth;
        for (size_t i = 0; i < mipCount; i++) {
            GetSurfaceInfo(w, h, format, &NumBytes, &RowBytes, nullptr);

            if ((mipCount <= 1) || !maxsize || (w <= maxsize && h <= maxsize && d <= maxsize)) {
                if (!twidth) {
                    twidth = w;
                    theight = h;
                    tdepth = d;
                }

                initData[index].pData = (const void *)pSrcBits;
                initData[index].RowPitch = static_cast<UINT>(RowBytes);
                initData[index].SlicePitch = static_cast<UINT>(NumBytes);
                ++index;
            } else if (!j) {
                ++skipMip;
            }

            if (pSrcBits + (NumBytes * d) > pEndBits) {
                return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
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

    return (index > 0) ? S_OK : E_FAIL;
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
        if (dxt10Header) {
            format = dxt10Header->dxgiFormat;
            // 调试输出
            switch (format) {
            case DXGI_FORMAT_BC7_UNORM:
                OutputDebugString(L"Format: BC7_UNORM (linear)\n");
                break;
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                OutputDebugString(L"Format: BC7_UNORM_SRGB (sRGB)\n");
                break;
            default:
                OutputDebugString(L"Format: Other\n");
                break;
            }
        }
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

    outInfo.pixelData.resize(fileSize - pixelDataOffset);
    memcpy(outInfo.pixelData.data(), fileData + pixelDataOffset, fileSize - pixelDataOffset);

    size_t mipCount = outInfo.desc.MipLevels;
    size_t width = outInfo.desc.Width;
    size_t height = outInfo.desc.Height;
    size_t depth = (outInfo.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? outInfo.desc.DepthOrArraySize : 1;

    outInfo.subresources.resize(mipCount * arraySize);

    size_t twidth, theight, tdepth, skipMip;
    HRESULT hr = FillInitDataForInfo(width, height, depth, mipCount, arraySize, format, 0, outInfo.pixelData.size(),
                                     outInfo.pixelData.data(), // ← 使用 pixelData
                                     twidth, theight, tdepth, skipMip, outInfo.subresources.data());

    if (FAILED(hr)) {
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