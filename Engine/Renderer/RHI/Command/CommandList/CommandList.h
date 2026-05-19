#pragma once
#include "Common/d3dUtil.h"
#include <d3d12.h>
#include <wrl/client.h>

namespace DX12Engine::Renderer {

/**
 * @brief 命令列表封装类
 * @note 此类不包含所有权，仅作为 ID3D12GraphicsCommandList 的句柄和便捷接口
 */
class CommandList {
public:
    CommandList() : m_cmdList(nullptr) {}

    // 允许从裸指针构造（通常由 Pool 内部使用）
    explicit CommandList(ID3D12GraphicsCommandList *cmdList) : m_cmdList(cmdList) {}

    ~CommandList() = default;

    // 允许拷贝和移动，因为只是指针包装
    CommandList(const CommandList &) = default;
    CommandList &operator=(const CommandList &) = default;
    CommandList(CommandList &&) = default;
    CommandList &operator=(CommandList &&) = default;

    // 获取底层接口
    ID3D12GraphicsCommandList *Get() const { return m_cmdList; }

    // 检查有效性
    bool IsValid() const { return m_cmdList != nullptr; }

    // ========================================================================
    // 便捷方法转发
    // ========================================================================

    void Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState) {
        if (m_cmdList) {
            ThrowIfFailed(m_cmdList->Reset(pAllocator, pInitialState));
        }
    }

    void Close() {
        if (m_cmdList) {
            ThrowIfFailed(m_cmdList->Close());
        }
    }

    void ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER *pBarriers) {
        if (m_cmdList) {
            m_cmdList->ResourceBarrier(NumBarriers, pBarriers);
        }
    }

    void DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation,
                       UINT StartInstanceLocation) {
        if (m_cmdList) {
            m_cmdList->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
        }
    }

    void SetPipelineState(ID3D12PipelineState *pPipelineState) {
        if (m_cmdList) {
            m_cmdList->SetPipelineState(pPipelineState);
        }
    }

    void SetComputeRootSignature(ID3D12RootSignature *pRootSignature) {
        if (m_cmdList) {
            m_cmdList->SetComputeRootSignature(pRootSignature);
        }
    }

    void Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
        if (m_cmdList) {
            m_cmdList->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
        }
    }

private:
    ID3D12GraphicsCommandList *m_cmdList;
};

} // namespace DX12Engine::Renderer