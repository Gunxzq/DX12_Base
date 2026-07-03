#pragma once

#include <cstdint>
#include <string>

namespace DX12Engine::Resource {

// -----------------------------------------------------------------
// 描述符配置标志
enum class DescriptorSlotFlags : uint32_t {
    None = 0,
    LinearAlloc = 1 << 0,  // 线性分配（否则优先复用）
    EnableExpand = 1 << 1, // 允许扩容
    DelayRelease = 1 << 2  // 延迟释放（基于围栏）
};

inline DescriptorSlotFlags operator|(DescriptorSlotFlags a, DescriptorSlotFlags b) {
    return static_cast<DescriptorSlotFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline DescriptorSlotFlags operator&(DescriptorSlotFlags a, DescriptorSlotFlags b) {
    return static_cast<DescriptorSlotFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool HasFlag(DescriptorSlotFlags value, DescriptorSlotFlags flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

// 描述符配置结构
struct DescriptorSlotAllocatorConfig {
    uint32_t initialCapacity = 4096;                               // 初始容量
    uint32_t maxCapacity = 0;                                      // 最大容量，0表示不限制
    DescriptorSlotFlags flags = DescriptorSlotFlags::EnableExpand; // 配置标志
};

// -----------------------------------------------------------------
// 描述符堆配置结构
struct DescriptorHeapConfig {
    D3D12_DESCRIPTOR_HEAP_TYPE type; // D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV 等
    uint32_t initialSize;
    uint32_t maxSize;
    DescriptorSlotFlags slotFlags;
    bool shaderVisible;
};

// -----------------------------------------------------------------
// 渲染目标描述结构
struct RenderTargetDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uint32_t mipLevels = 1;
    uint32_t arraySize = 1;
    DXGI_SAMPLE_DESC sampleDesc = {1, 0};
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clearValue = {};
    std::wstring name;  // 调试名称（RenderDoc 识别）
};

// -----------------------------------------------------------------
// 深度模板描述结构
struct DepthStencilDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    DXGI_FORMAT format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    uint32_t arraySize = 1;
    DXGI_SAMPLE_DESC sampleDesc = {1, 0};
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clearValue = {};
    std::wstring name;  // 调试名称（RenderDoc 识别）
};

} // namespace DX12Engine::Resource