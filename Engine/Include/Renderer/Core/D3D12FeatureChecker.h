// Engine/Include/Renderer/Core/D3D12FeatureChecker.h
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine::Renderer {

/**
 * @brief D3D12 特性支持信息
 */
struct D3D12FeatureInfo {
    // MSAA 支持
    struct MsaaSupport {
        bool isSupported = false;
        UINT qualityLevels = 0;
    };

    // 适配器信息
    struct AdapterInfo {
        std::string description;
        DXGI_ADAPTER_DESC desc;
        bool isHardware = false;
        D3D_FEATURE_LEVEL maxSupportedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        MsaaSupport msaa4xR8G8B8A8; // 常见格式的 MSAA 支持
    };

    // 当前设备信息
    D3D_FEATURE_LEVEL currentFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    UINT rtvDescriptorSize = 0;
    UINT dsvDescriptorSize = 0;
    UINT cbvSrvUavDescriptorSize = 0;
};

/**
 * @brief D3D12 功能检测器
 *
 * 职责：
 * 1. 枚举可用的 DXGI 适配器
 * 2. 检查 D3D12 功能级别支持
 * 3. 检查特定格式的 MSAA 质量等级
 * 4. 获取描述符堆增量大小
 */
class D3D12FeatureChecker {
public:
    D3D12FeatureChecker() = default;
    ~D3D12FeatureChecker() = default;

    // 禁止拷贝
    D3D12FeatureChecker(const D3D12FeatureChecker &) = delete;
    D3D12FeatureChecker &operator=(const D3D12FeatureChecker &) = delete;

    /**
     * @brief 初始化并枚举所有适配器
     * @param enableDebugLayer 是否启用调试层以获取更多信息
     */
    void Initialize(bool enableDebugLayer = false);

    /**
     * @brief 获取所有可用适配器列表
     */
    const std::vector<D3D12FeatureInfo::AdapterInfo> &GetAdapters() const { return m_adapters; }

    /**
     * @brief 选择最佳适配器（优先硬件，其次功能级别）
     */
    const D3D12FeatureInfo::AdapterInfo *SelectBestAdapter() const;

    /**
     * @brief 创建指定适配器的 D3D12 设备并查询其特性
     * @param adapterIndex 适配器索引，-1 表示默认适配器
     * @param minFeatureLevel 最低要求的功能级别
     * @return 是否成功创建设备
     */
    bool CreateDevice(int adapterIndex = -1, D3D_FEATURE_LEVEL minFeatureLevel = D3D_FEATURE_LEVEL_11_0);

    /**
     * @brief 使用 WARP 软件适配器创建设备
     */
    bool CreateWarpDevice(D3D_FEATURE_LEVEL minFeatureLevel = D3D_FEATURE_LEVEL_11_0);

    /**
     * @brief 检查指定格式的 MSAA 支持情况
     */
    D3D12FeatureInfo::MsaaSupport CheckMsaaSupport(DXGI_FORMAT format, UINT sampleCount) const;

    /**
     * @brief 获取当前设备的描述符大小信息
     */
    const D3D12FeatureInfo &GetDeviceInfo() const { return m_deviceInfo; }

    /**
     * @brief 获取底层 D3D12 设备指针
     */
    ID3D12Device *GetDevice() const { return m_device.Get(); }

    /**
     * @brief 获取 DXGI 工厂指针
     */
    IDXGIFactory4 *GetFactory() const { return m_factory.Get(); }

private:
    void EnumerateAdapters();
    void LogAdapters() const;
    D3D12FeatureInfo::MsaaSupport CheckMsaaSupportOnDevice(ID3D12Device *device, DXGI_FORMAT format,
                                                           UINT sampleCount) const;

private:
    Microsoft::WRL::ComPtr<IDXGIFactory4> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;

    std::vector<D3D12FeatureInfo::AdapterInfo> m_adapters;
    D3D12FeatureInfo m_deviceInfo;
};

} // namespace DX12Engine::Renderer