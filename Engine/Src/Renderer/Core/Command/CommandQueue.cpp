#include "Renderer/Core/Command/CommandQueue.h"

namespace DX12Engine::Renderer {

CommandQueue::CommandQueue(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type) : m_type(type) {
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_queue)));
}

void CommandQueue::Execute(CommandList &cmdList) {
    ID3D12CommandList *lists[] = {cmdList.Get()};
    m_queue->ExecuteCommandLists(1, lists);
}

void CommandQueue::ExecuteBatch(const std::vector<CommandList> &cmdLists) {
    if (cmdLists.empty())
        return;

    std::vector<ID3D12CommandList *> rawLists;
    rawLists.reserve(cmdLists.size());
    for (const auto &cl : cmdLists) {
        rawLists.push_back(cl.Get());
    }
    m_queue->ExecuteCommandLists(static_cast<UINT>(rawLists.size()), rawLists.data());
}

} // namespace DX12Engine::Renderer