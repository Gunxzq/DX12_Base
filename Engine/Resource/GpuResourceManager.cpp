#include "GpuResourceManager.h"
#include "Common/EngineAssert.h"
#include "Common/d3dx12.h"
#include <cassert>

namespace DX12Engine::Resource {

GpuResourceManager &GpuResourceManager::GetInstance() {
    static GpuResourceManager instance;
    return instance;
}

void GpuResourceManager::Initialize() {
    if (m_initialized)
        return;

    // 初始化 HandlePool，预分配一定容量
    GpuHandlePool::InitConfig config;
    config.maxTotalHandles = 8192;
    m_handlePool.Initialize(config);

    m_initialized = true;
}

void GpuResourceManager::Shutdown() {
    // 强制清理所有待释放资源（在退出时）
    // 注意：生产环境中应确保所有 Fence 都已完成再调用 Shutdown
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto &pr : m_pendingReleases) {
        ID3D12Resource *res = static_cast<ID3D12Resource *>(m_handlePool.GetDataPtr(pr.handle));
        if (res) {
            res->Release();
        }
        m_handlePool.FreeSlot(pr.handle);
    }
    m_pendingReleases.clear();
    m_handlePool.Shutdown();
    m_initialized = false;
}

GpuResourceHandle GpuResourceManager::CreateBuffer(ID3D12Device *device, size_t size, D3D12_HEAP_TYPE heapType,
                                                   D3D12_RESOURCE_STATES initialState) {
    if (!m_initialized || !device) {
        return GpuResourceHandle::Invalid();
    }

    // 1. 分配句柄
    GpuResourceHandle handle = m_handlePool.AllocateSlot(GpuResourceType::Buffer, 0);

    // 2. 创建 D3D12 资源
    CD3DX12_HEAP_PROPERTIES heapProps(heapType);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(size);

    ID3D12Resource *resource = nullptr;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                                 IID_PPV_ARGS(&resource));

    if (FAILED(hr) || !resource) {
        ENGINE_ASSERT_FMT("GpuResourceManager: Failed to create buffer. Size: %zu, HR: 0x%X", size, hr);
        m_handlePool.FreeSlot(handle);
        return GpuResourceHandle::Invalid();
    }

    // 3. 存储指针并标记为 Ready
    m_handlePool.SetDataPtr(handle, resource);
    m_handlePool.SetState(handle, GpuResourceState::Ready);

    // 4. 更新内存统计
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_totalMemoryUsage += size;
    }

    return handle;
}

GpuResourceHandle GpuResourceManager::CreateTexture2D(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                                                      D3D12_RESOURCE_STATES initialState) {
    if (!m_initialized || !device) {
        return GpuResourceHandle::Invalid();
    }

    // 验证维度
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return GpuResourceHandle::Invalid();
    }

    GpuResourceHandle handle = m_handlePool.AllocateSlot(GpuResourceType::Texture2D, 0xFF);

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

    ID3D12Resource *resource = nullptr;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                                                 IID_PPV_ARGS(&resource));

    if (FAILED(hr) || !resource) {
        m_handlePool.FreeSlot(handle);
        return GpuResourceHandle::Invalid();
    }

    m_handlePool.SetDataPtr(handle, resource);
    m_handlePool.SetState(handle, GpuResourceState::Ready);

    // 获取真实内存大小
    D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = device->GetResourceAllocationInfo(0, 1, &desc);
    size_t memSize = allocationInfo.SizeInBytes;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_totalMemoryUsage += memSize;
    }

    return handle;
}

GpuResourceHandle GpuResourceManager::CreateTexture2D(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                                                      const D3D12_CLEAR_VALUE &clearValue,
                                                      D3D12_RESOURCE_STATES initialState) {
    if (!m_initialized || !device) {
        return GpuResourceHandle::Invalid();
    }

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return GpuResourceHandle::Invalid();
    }

    GpuResourceHandle handle = m_handlePool.AllocateSlot(GpuResourceType::Texture2D, 0xFF);

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

    ID3D12Resource *resource = nullptr;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, &clearValue,
                                                 IID_PPV_ARGS(&resource));

    if (FAILED(hr) || !resource) {
        m_handlePool.FreeSlot(handle);
        return GpuResourceHandle::Invalid();
    }

    m_handlePool.SetDataPtr(handle, resource);
    m_handlePool.SetState(handle, GpuResourceState::Ready);

    D3D12_RESOURCE_ALLOCATION_INFO allocationInfo = device->GetResourceAllocationInfo(0, 1, &desc);
    size_t memSize = allocationInfo.SizeInBytes;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_totalMemoryUsage += memSize;
    }

    return handle;
}

ID3D12Resource *GpuResourceManager::GetResource(GpuResourceHandle handle) const {
    if (!m_handlePool.Validate(handle)) {
        return nullptr;
    }
    if (m_handlePool.GetState(handle) != GpuResourceState::Ready) {
        return nullptr;
    }
    return static_cast<ID3D12Resource *>(m_handlePool.GetDataPtr(handle));
}

void GpuResourceManager::Release(GpuResourceHandle handle, uint64_t completedFenceValue) {
    if (!m_handlePool.Validate(handle)) {
        return;
    }

    m_handlePool.SetState(handle, GpuResourceState::PendingRelease);

    PendingGpuRelease pr;
    pr.handle = handle;
    pr.fenceValue = completedFenceValue;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingReleases.push_back(pr);
    }
}

void GpuResourceManager::Update(uint64_t completedFenceValue) {
    std::vector<PendingGpuRelease> toRelease;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pendingReleases.begin();
        while (it != m_pendingReleases.end()) {
            if (completedFenceValue >= it->fenceValue) {
                toRelease.push_back(*it);
                it = m_pendingReleases.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto &pr : toRelease) {
        ID3D12Resource *resource = static_cast<ID3D12Resource *>(m_handlePool.GetDataPtr(pr.handle));
        if (resource) {
            // 获取资源描述以更新内存统计
            D3D12_RESOURCE_DESC desc = resource->GetDesc();
            size_t memSize = 0;
            if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
                memSize = desc.Width;
            } else {
                // 简化估算，实际可使用 GetResourceAllocationInfo
                memSize = desc.Width * desc.Height * 4;
            }

            resource->Release();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_totalMemoryUsage >= memSize) {
                    m_totalMemoryUsage -= memSize;
                }
            }
        }
        m_handlePool.FreeSlot(pr.handle);
    }
}

uint32_t GpuResourceManager::GetActiveCount() const { return m_handlePool.GetActiveCount(); }

size_t GpuResourceManager::GetTotalGpuMemoryUsage() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_totalMemoryUsage;
}

} // namespace DX12Engine::Resource