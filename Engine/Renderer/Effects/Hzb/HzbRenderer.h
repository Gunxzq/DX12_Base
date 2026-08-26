#pragma once

#include <algorithm>
#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

#include "Resource/Core/DescriptorHeapCollection.h"

namespace DX12Engine::Renderer {
class CommandList;
} // namespace DX12Engine::Renderer

namespace DX12Engine {
namespace Renderer {

class HzbRenderer {
public:
    HzbRenderer() = default;
    ~HzbRenderer() = default;

    HzbRenderer(const HzbRenderer &) = delete;
    HzbRenderer &operator=(const HzbRenderer &) = delete;

    // ---- 生命周期 ----
    void SetDevice(ID3D12Device *device) { m_device = device; }
    void SetDescriptorHeaps(DX12Engine::Resource::DescriptorHeapCollection *heaps) { m_descriptorHeaps = heaps; }
    /// 堆域标签（规则 17：描述符与 HZB 纹理所在堆一致；Editor=EditorViewport，Game=Default）
    void SetHeapTag(DX12Engine::Resource::HeapTag tag) { m_heapTag = tag; }
    bool Initialize();
    void Shutdown();
    bool IsValid() const { return m_initialized; }

    /**
     * @brief 执行 HZB 构建命令（CS 降采样）
     * @param cmd       命令列表（封装）
     * @param depthRes  深度缓冲资源指针（屏障用）
     * @param depthSRV  深度图 SRV（GPU handle，与 HZB 同堆域）
     * @param hzbRes    HZB 纹理资源指针（屏障用）
     * @param mipUAVs   HZB 各 mip 的 UAV GPU handle（[0..mipCount-1]，含 mip0=深度图 1:1 拷贝）
     * @param mipCount  总 mip 层级数（含 mip0）
     * @param width     深度图宽度（mip0 尺寸）
     * @param height    深度图高度
     */
    void Execute(CommandList &cmd, ID3D12Resource *depthRes, D3D12_GPU_DESCRIPTOR_HANDLE depthSRV,
                 ID3D12Resource *hzbRes, const std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> &mipUAVs, uint32_t mipCount,
                 uint32_t width, uint32_t height);

private:
    bool CreatePipeline();

    // 辅助：第 k 级 mip 尺寸（D3D floor 语义：max(1, size>>k)）
    static uint32_t MipSize(uint32_t base, uint32_t mip) { return std::max(1u, base >> mip); }

private:
    ID3D12Device *m_device = nullptr;
    DX12Engine::Resource::DescriptorHeapCollection *m_descriptorHeaps = nullptr;
    DX12Engine::Resource::HeapTag m_heapTag = DX12Engine::Resource::HeapTag::Default;
    bool m_initialized = false;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
};

} // namespace Renderer
} // namespace DX12Engine
