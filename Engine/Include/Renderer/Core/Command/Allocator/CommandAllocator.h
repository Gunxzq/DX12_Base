#pragma once
#include "Common/d3dUtil.h"
#include <cstdint>
#include <d3d12.h>
#include <stdexcept>
#include <wrl/client.h>

namespace DX12Engine::Renderer {

template <D3D12_COMMAND_LIST_TYPE Type> class CommandAllocator {
public:
    explicit CommandAllocator(ID3D12Device *device) {
        HRESULT hr = device->CreateCommandAllocator(Type, IID_PPV_ARGS(&m_allocator));
        if (FAILED(hr)) {
            throw std::runtime_error("Failed to create CommandAllocator");
        }
    }

    ~CommandAllocator() = default;

    CommandAllocator(const CommandAllocator &) = delete;
    CommandAllocator &operator=(const CommandAllocator &) = delete;

    ID3D12CommandAllocator *Get() const { return m_allocator.Get(); }

    void Reset() {
        // 注意：Reset 必须在 GPU 不再使用该 allocator 时调用
        ThrowIfFailed(m_allocator->Reset());
    }

    static constexpr D3D12_COMMAND_LIST_TYPE GetType() { return Type; }

    // ========================================================================
    // 元数据：记录该分配器最后一次提交时对应的 Fence 值
    // ========================================================================

    void SetLastUsedFenceValue(uint64_t value) { m_lastUsedFenceValue = value; }
    uint64_t GetLastUsedFenceValue() const { return m_lastUsedFenceValue; }

private:
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator;
    uint64_t m_lastUsedFenceValue = 0;
};

} // namespace DX12Engine::Renderer