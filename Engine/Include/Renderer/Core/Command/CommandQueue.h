#pragma once
#include "CommandList/CommandList.h"
#include "Common/d3dUtil.h"
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine::Renderer {

class CommandQueue {
public:
    explicit CommandQueue(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type);

    // 提交单个命令列表
    void Execute(CommandList &cmdList);

    // 批量提交
    void ExecuteBatch(const std::vector<CommandList> &cmdLists);

    // GPU 端等待围栏
    void Wait(ID3D12Fence *fence, UINT64 value);

    ID3D12CommandQueue *Get() const { return m_queue.Get(); }
    D3D12_COMMAND_LIST_TYPE GetType() const { return m_type; }

private:
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    D3D12_COMMAND_LIST_TYPE m_type;
};

} // namespace DX12Engine::Renderer