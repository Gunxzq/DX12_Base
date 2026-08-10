#pragma once

#include "Common/Common.h"

#include <nlohmann/json.hpp>

namespace DX12Engine {
namespace Boot {

// ========================================================================
// 辅助转换函数
// ========================================================================

inline DXGI_FORMAT StringToDxgiFormat(const std::string &formatStr) {
    if (formatStr == "R8G8B8A8_UNORM")
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (formatStr == "R16G16B16A16_FLOAT")
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (formatStr == "D24_UNORM_S8_UINT")
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    if (formatStr == "D32_FLOAT")
        return DXGI_FORMAT_D32_FLOAT;
    if (formatStr == "D32_FLOAT_S8X24_UINT")
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    if (formatStr == "R10G10B10A2_UNORM")
        return DXGI_FORMAT_R10G10B10A2_UNORM;

    ErrorReporter::Fatal("Unknown DXGI Format string: %s, defaulting to UNKNOWN", formatStr.c_str());
    return DXGI_FORMAT_UNKNOWN;
}

inline D3D_FEATURE_LEVEL FloatToFeatureLevel(float level) {
    if (level >= 12.2f)
        return D3D_FEATURE_LEVEL_12_2;
    if (level >= 12.1f)
        return D3D_FEATURE_LEVEL_12_1;
    if (level >= 12.0f)
        return D3D_FEATURE_LEVEL_12_0;
    if (level >= 11.1f)
        return D3D_FEATURE_LEVEL_11_1;
    return D3D_FEATURE_LEVEL_11_0;
}

// ========================================================================
// 设备配置结构体
// ========================================================================

struct DeviceConfig {
    float minFeatureLevel = 11.0f;
    bool enableDebugLayer = true;
    bool enableGPUBasedValidation = false; // 控制 GBV 调试层，会显著影响性能
    bool warpFallback = true;

    D3D_FEATURE_LEVEL FeatureLevelEnum = D3D_FEATURE_LEVEL_11_0;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(DeviceConfig, minFeatureLevel, enableDebugLayer, enableGPUBasedValidation,
                                   warpFallback)

    void PostLoad() { FeatureLevelEnum = FloatToFeatureLevel(minFeatureLevel); }
};

// ========================================================================
// 交换链配置结构体
// ========================================================================

struct SwapChainConfig {
    uint32_t bufferCount = 2;
    uint32_t refreshRateNumerator = 60;
    uint32_t refreshRateDenominator = 1;
    std::string swapEffect = "FLIP_DISCARD";
    std::vector<std::string> flags;
    bool windowed = true;
    bool enableVsync = true; // 2026-08-10：Present(syncInterval)——false 时 Present(0,0) 打破 Vsync 60 锁（帧率上限）

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SwapChainConfig, bufferCount, refreshRateNumerator, refreshRateDenominator,
                                   swapEffect, flags, windowed, enableVsync)
};

// ========================================================================
// MSAA 配置结构体
// ========================================================================

struct MSAAConfig {
    bool enabled = false;
    uint32_t sampleCount = 4;
    bool qualityLevelAutoDetect = true;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(MSAAConfig, enabled, sampleCount, qualityLevelAutoDetect)
};

// ========================================================================
// 格式配置结构体
// ========================================================================

struct FormatConfig {
    std::string backBufferFormat = "R8G8B8A8_UNORM";
    std::string depthStencilFormat = "D24_UNORM_S8_UINT";

    // 运行时使用的转换值
    DXGI_FORMAT BackBufferFormatEnum = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT DepthStencilFormatEnum = DXGI_FORMAT_D24_UNORM_S8_UINT;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FormatConfig, backBufferFormat, depthStencilFormat)

    void PostLoad() {
        BackBufferFormatEnum = StringToDxgiFormat(backBufferFormat);
        DepthStencilFormatEnum = StringToDxgiFormat(depthStencilFormat);
    }
};

// ========================================================================
// 视口配置结构体
// ========================================================================

struct ViewportConfig {
    float minDepth = 0.0f;
    float maxDepth = 1.0f;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(ViewportConfig, minDepth, maxDepth)
};

// ========================================================================
// 顶层渲染器配置结构体
// ========================================================================

/**
 * @brief 完整渲染器配置
 * 对应 JSON 中的 "renderer" 对象
 */
struct RendererConfig {
    DeviceConfig device;
    SwapChainConfig swapChain;
    MSAAConfig msaa;
    FormatConfig formats;
    ViewportConfig viewport;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(RendererConfig, device, swapChain, msaa, formats, viewport)

    void PostLoad() {
        device.PostLoad();
        formats.PostLoad();
    }
};

} // namespace Boot
} // namespace DX12Engine