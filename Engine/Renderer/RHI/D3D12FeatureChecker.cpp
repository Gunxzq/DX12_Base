#include "D3D12FeatureChecker.h"
#include "Common/ThrowHelper.h"
#include "Common/d3dx12.h"
#include <Windows.h>

using namespace Microsoft::WRL;
using namespace DX12Engine::Renderer;

// ============================================================================
// 工具函数：将宽字符串转换为 std::string（替代已弃用的 std::wstring_convert）
// ============================================================================
static std::string WideStringToString(const wchar_t* wideStr) {
    if (!wideStr || wideStr[0] == L'\0')
        return {};

    int bufSize = WideCharToMultiByte(CP_UTF8, 0, wideStr, -1, nullptr, 0, nullptr, nullptr);
    if (bufSize <= 1)  // bufSize 包含 null 终止符，<=1 表示空字符串
        return {};

    std::string result(bufSize - 1, '\0'); // 不含 null 终止符
    WideCharToMultiByte(CP_UTF8, 0, wideStr, -1, &result[0], bufSize, nullptr, nullptr);
    return result;
}

void D3D12FeatureChecker::Initialize(bool enableDebugLayer, bool enableGPUBasedValidation) {
    UINT dxgiFactoryFlags = 0;

#if defined(DEBUG) || defined(_DEBUG)
    if (enableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            OutputDebugStringA("[D3D12] Debug Layer enabled\n");

            // 只有明确启用时才开启 GBV（GPU-Based Validation）
            if (enableGPUBasedValidation) {
                ComPtr<ID3D12Debug1> debugController1;
                if (SUCCEEDED(debugController->QueryInterface(IID_PPV_ARGS(&debugController1)))) {
                    debugController1->SetEnableGPUBasedValidation(TRUE);
                    debugController1->SetEnableSynchronizedCommandQueueValidation(TRUE);
                    OutputDebugStringA("[D3D12] GPU-Based Validation (GBV) enabled\n");
                }
            } else {
                OutputDebugStringA("[D3D12] GPU-Based Validation (GBV) disabled\n");
            }

            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(m_factory.GetAddressOf())));

    EnumerateAdapters();

#ifdef _DEBUG
    LogAdapters();
#endif
}

void D3D12FeatureChecker::EnumerateAdapters() {
    m_adapters.clear();
    UINT i = 0;
    ComPtr<IDXGIAdapter1> adapter; // 修改为 IDXGIAdapter1

    while (m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND) { // 使用 EnumAdapters1
        DXGI_ADAPTER_DESC1 desc;                                            // 修改为 DXGI_ADAPTER_DESC1
        adapter->GetDesc1(&desc);                                           // 使用 GetDesc1

        // 跳过软件适配器（除非后续专门请求 WARP）
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter.Reset();
            ++i;
            continue;
        }

        D3D12FeatureInfo::AdapterInfo info = {};

        // 如果需要保留旧的 DXGI_ADAPTER_DESC 格式，可以手动转换或只使用需要的字段
        // 这里假设 AdapterInfo::desc 是 DXGI_ADAPTER_DESC 类型，我们需要从 desc1 映射过去
        // 或者更简单地，如果 AdapterInfo 允许，直接存储 description 字符串即可
        // 由于 DXGI_ADAPTER_DESC1 和 DXGI_ADAPTER_DESC 前几个字段兼容，但为了严谨：
        info.desc.Description[0] = '\0';
        wcscpy_s(info.desc.Description, desc.Description);
        info.desc.VendorId = desc.VendorId;
        info.desc.DeviceId = desc.DeviceId;
        info.desc.SubSysId = desc.SubSysId;
        info.desc.Revision = desc.Revision;
        info.desc.DedicatedVideoMemory = desc.DedicatedVideoMemory;
        info.desc.DedicatedSystemMemory = desc.DedicatedSystemMemory;
        info.desc.SharedSystemMemory = desc.SharedSystemMemory;
        // DXGI_ADAPTER_DESC 没有 Luid 和 Flags，所以忽略它们

        info.description = WideStringToString(desc.Description);
        info.isHardware = true;

        // 探测最高支持的功能级别
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                            D3D_FEATURE_LEVEL_11_0};

        for (auto level : levels) {
            ComPtr<ID3D12Device> tempDevice;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), level, IID_PPV_ARGS(&tempDevice)))) {
                info.maxSupportedFeatureLevel = level;

                // 检查常见格式的 MSAA 支持
                info.msaa4xR8G8B8A8 = CheckMsaaSupportOnDevice(tempDevice.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 4);
                break;
            }
        }

        m_adapters.push_back(info);
        adapter.Reset();
        ++i;
    }
}
const D3D12FeatureInfo::AdapterInfo *D3D12FeatureChecker::SelectBestAdapter() const {
    if (m_adapters.empty())
        return nullptr;

    // 简单策略：选择第一个硬件适配器（通常是最强的独显或集显）
    // 更复杂的策略可以比较显存、功能级别等
    return &m_adapters[0];
}

bool D3D12FeatureChecker::CreateDevice(int adapterIndex, D3D_FEATURE_LEVEL minFeatureLevel) {
    ComPtr<IDXGIAdapter> adapter;

    if (adapterIndex >= 0 && adapterIndex < static_cast<int>(m_adapters.size())) {
        // 注意：这里需要重新获取适配器接口，因为之前枚举时可能已经 Release
        // 简化起见，这里直接使用默认适配器或重新枚举
        // 实际生产中应保存 ComPtr 或重新 EnumAdapters
        ThrowIfFailed(m_factory->EnumAdapters(adapterIndex, &adapter));
    }

    HRESULT hr = D3D12CreateDevice(adapter.Get(), minFeatureLevel, IID_PPV_ARGS(m_device.GetAddressOf()));

    if (FAILED(hr))
        return false;

    // 填充设备信息
    m_deviceInfo.currentFeatureLevel = minFeatureLevel;
    m_deviceInfo.rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_deviceInfo.dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_deviceInfo.cbvSrvUavDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return true;
}

bool D3D12FeatureChecker::CreateWarpDevice(D3D_FEATURE_LEVEL minFeatureLevel) {
    ComPtr<IDXGIAdapter> warpAdapter;
    ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

    HRESULT hr = D3D12CreateDevice(warpAdapter.Get(), minFeatureLevel, IID_PPV_ARGS(m_device.GetAddressOf()));

    if (FAILED(hr))
        return false;

    m_deviceInfo.currentFeatureLevel = minFeatureLevel;
    m_deviceInfo.rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_deviceInfo.dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_deviceInfo.cbvSrvUavDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return true;
}

D3D12FeatureInfo::MsaaSupport D3D12FeatureChecker::CheckMsaaSupport(DXGI_FORMAT format, UINT sampleCount) const {
    if (!m_device)
        return {};
    return CheckMsaaSupportOnDevice(m_device.Get(), format, sampleCount);
}

D3D12FeatureInfo::MsaaSupport D3D12FeatureChecker::CheckMsaaSupportOnDevice(ID3D12Device *device, DXGI_FORMAT format,
                                                                            UINT sampleCount) const {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels = {};
    msQualityLevels.Format = format;
    msQualityLevels.SampleCount = sampleCount;
    msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    msQualityLevels.NumQualityLevels = 0;

    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msQualityLevels,
                                             sizeof(msQualityLevels));

    D3D12FeatureInfo::MsaaSupport support = {};
    if (SUCCEEDED(hr) && msQualityLevels.NumQualityLevels > 0) {
        support.isSupported = true;
        support.qualityLevels = msQualityLevels.NumQualityLevels;
    }

    return support;
}

void D3D12FeatureChecker::LogAdapters() const {
    for (const auto &adapter : m_adapters) {
        std::string msg = "[D3D12FeatureChecker] Adapter: " + adapter.description +
                          ", Feature Level: " + std::to_string(adapter.maxSupportedFeatureLevel) + "\n";
        OutputDebugStringA(msg.c_str());
    }
}