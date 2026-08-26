#include "FrameResourceManager.h"
#include "FrameResourceConfig.h"
#include "Resource/Core/DescriptorHeapCollection.h"

#include "Common/Common.h"

using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;

namespace DX12Engine::Renderer {

FrameResourceManager::~FrameResourceManager() { Shutdown(); }

void FrameResourceManager::CreatePassCB(ID3D12Device *device) {
    UINT cbSize = (sizeof(PassConstants) + 255) & ~255; // 256 字节对齐

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

    HRESULT hr =
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                        nullptr, IID_PPV_ARGS(&m_passCBResource));

    if (FAILED(hr)) {
        return;
    }

    hr = m_passCBResource->Map(0, nullptr, &m_passCBMapped);
    if (FAILED(hr)) {
        m_passCBResource.Reset();
        return;
    }
    m_passCBResource->SetName(L"PassCB");

    m_passCBAddress = m_passCBResource->GetGPUVirtualAddress();
}

FrameResourceManager::RingBufferEntry *FrameResourceManager::FindEntry(const std::string &name) {
    for (auto &entry : m_ringBuffers) {
        if (entry.name == name)
            return &entry;
    }
    return nullptr;
}

D3D12_GPU_VIRTUAL_ADDRESS FrameResourceManager::AllocateFrom(RingBuffer &buffer, const void *data, uint32_t size,
                                                             uint64_t fence, uint32_t alignment) {
    if (data) {
        return buffer.AllocateUpload(data, size, fence, alignment);
    }
    return buffer.Allocate(size, fence, alignment);
}

RingBuffer *FrameResourceManager::CreateSegment(RingBufferEntry &entry, uint32_t size, uint64_t fence) {

    if (!entry.allowExpand) {
        return nullptr; // 不允许扩容
    }

    // 增长因子：1.5x 当前总大小（非翻倍——翻倍指数膨胀到 1GB 是本 bug 根因，见 Frame.md 策略 1）
    uint32_t totalSize = 0;

    for (const auto &seg : entry.segments)
        totalSize += seg.buffer.GetSize();

    uint32_t newSize = std::max(static_cast<uint32_t>(totalSize * 1.5f), size);

    if (newSize > entry.maxSize)
        newSize = entry.maxSize; // 硬上限

    if (newSize <= totalSize && totalSize >= entry.maxSize) {
        // 已达硬上限，扩容失败（VS 控制台日志 = OutputDebugStringW，含上传大小）
        wchar_t failMsg[256];
        swprintf_s(
            failMsg,
            L"[ERROR] FrameResourceManager: '%hs' ringbuffer 扩容失败: 已达 %u MB 硬上限 (upload %u B, total %u MB)\n",
            entry.name.c_str(), entry.maxSize / (1024 * 1024), size, totalSize / (1024 * 1024));
        OutputDebugStringW(failMsg);
        return nullptr; // 已达上限，明确失败而非无限增长
    }

    RingBufferEntry::Segment newSeg;
    std::wstring wname(entry.name.begin(), entry.name.end());
    wname += L"_seg";

    if (!newSeg.buffer.Initialize(m_device, newSize, wname, entry.heapType, entry.flags, entry.initialState)) {
        // 新段创建失败（VS 控制台日志 = OutputDebugStringW，含上传大小）
        wchar_t failMsg[256];
        swprintf_s(
            failMsg,
            L"[ERROR] FrameResourceManager: '%hs' 新段创建失败: buffer.Initialize(%u MB) 返回 false (upload %u B)\n",
            entry.name.c_str(), newSize / (1024 * 1024), size);
        OutputDebugStringW(failMsg);
        return nullptr;
    }

    // 旧段保留在 segments（不销毁——旧段 GPU 地址可能仍被上一帧引用，销毁即悬垂/TDR）。
    // 旧段回收由 BeginFrame 按 fence 延迟执行
    entry.segments.push_back(std::move(newSeg));
    entry.currentSegment = static_cast<uint32_t>(entry.segments.size() - 1);
    entry.segments[entry.currentSegment].lastFence = fence;

    return &entry.segments[entry.currentSegment].buffer;
}

void FrameResourceManager::Initialize(ID3D12Device *device, DescriptorHeapCollection *descriptorHeaps,
                                      const FrameResourceConfig &config) {
    if (m_initialized) {
        Shutdown();
    }

    m_device = device;

    // 创建 PassCB
    CreatePassCB(device);

    // 按配置创建环形缓冲区（段 0 = 配置初始段）
    for (const auto &rbCfg : config.ringBuffers) {
        auto &entry = m_ringBuffers.emplace_back();
        entry.name = rbCfg.name;
        entry.alignment = rbCfg.alignment;
        entry.allowExpand = rbCfg.allowExpand;
        entry.maxSize = rbCfg.maxSize;
        entry.heapType = ToD3D12HeapType(rbCfg.heapType);
        entry.flags = ToD3D12Flags(rbCfg.flags);
        entry.initialState = ToD3D12State(rbCfg.initialState);

        std::wstring wname(rbCfg.name.begin(), rbCfg.name.end());
        RingBufferEntry::Segment seg;
        seg.buffer.Initialize(device, rbCfg.initialSize, wname, entry.heapType, entry.flags, entry.initialState);
        entry.segments.push_back(std::move(seg));
        entry.currentSegment = 0;
    }

    m_passConstants = {};
    UpdatePassConstants();

    m_initialized = true;
}

void FrameResourceManager::Shutdown() {
    if (!m_initialized) {
        return;
    }

    for (auto &entry : m_ringBuffers) {
        for (auto &seg : entry.segments)
            seg.buffer.Shutdown();
    }
    m_ringBuffers.clear();

    if (m_passCBResource) {
        if (m_passCBMapped) {
            m_passCBResource->Unmap(0, nullptr);
            m_passCBMapped = nullptr;
        }
        m_passCBResource.Reset();
    }

    m_device = nullptr;
    m_passCBAddress = 0;
    m_initialized = false;
}

void FrameResourceManager::BeginFrame(uint64_t completedFence, uint64_t nextFence) {
    if (!m_initialized)
        return;

    for (auto &entry : m_ringBuffers) {
        // 1. 每段 Reclaim（环形释放已完成帧空间）
        for (auto &seg : entry.segments)
            seg.buffer.Reclaim(completedFence);

        // 2. 旧段延迟回收：非当前段且 fence 已完成 → 移除（对齐 Frame.md ScheduleOldSegmentReclaim）。
        //    当前段保留（本帧分配用）；新段创建后旧段留在列表，等 GPU 用完（lastFence <= completedFence）才释放，
        //    杜绝扩容销毁 GPU 在用资源（悬垂/TDR）
        if (entry.segments.size() > 1) {
            for (size_t i = 0; i < entry.segments.size();) {
                if (i == entry.currentSegment) {
                    ++i;
                    continue;
                }
                if (entry.segments[i].lastFence <= completedFence) {
                    entry.segments[i].buffer.Shutdown();
                    entry.segments.erase(entry.segments.begin() + i);
                    if (entry.currentSegment > i)
                        --entry.currentSegment;
                } else {
                    ++i;
                }
            }
        }
    }

    m_currentFence = nextFence;
}

D3D12_GPU_VIRTUAL_ADDRESS FrameResourceManager::GetPassCBAddress() const { return m_passCBAddress; }

void FrameResourceManager::UpdatePassConstants() {
    if (!m_initialized || !m_passCBMapped) {
        return;
    }

    memcpy(m_passCBMapped, &m_passConstants, sizeof(PassConstants));
}

D3D12_GPU_VIRTUAL_ADDRESS FrameResourceManager::Allocate(const std::string &name, const void *data, uint32_t size,
                                                         uint32_t alignment) {

    if (!m_initialized)
        return 0;

    RingBufferEntry *entry = FindEntry(name);
    if (!entry)
        return 0;

    // 显式 alignment 优先（SRV 元素对齐修复：GPUInstanceData=96B 需 96 对齐），否则用条目配置
    const uint32_t align = alignment ? alignment : entry->alignment;

    // 优先当前段（SRV 段偏移依赖单段连续性）；失败 → 新建更大段（旧段保留延迟回收）
    RingBuffer &cur = entry->segments[entry->currentSegment].buffer;

    if (entry->heapType == D3D12_HEAP_TYPE_UPLOAD || entry->heapType == D3D12_HEAP_TYPE_READBACK) {
        D3D12_GPU_VIRTUAL_ADDRESS addr = AllocateFrom(cur, data, size, m_currentFence, align);
        if (addr != 0) {
            entry->segments[entry->currentSegment].lastFence = m_currentFence;
            return addr;
        }
    } else {
        // DEFAULT 堆：不支持 CPU 上传，data 必须为 nullptr
        if (data != nullptr) {
            return 0;
        }
        // 只分配地址，不写入数据
        D3D12_GPU_VIRTUAL_ADDRESS addr = cur.Allocate(size, m_currentFence, align);
        if (addr != 0) {
            entry->segments[entry->currentSegment].lastFence = m_currentFence;
            return addr;
        }
    }

    // 当前段满 → 扩容新建段（1.5x 增长 + 256MB 上限，Frame.md 策略 1）
    RingBuffer *newSeg = CreateSegment(*entry, size, m_currentFence);
    if (!newSeg)
        return 0; // 扩容失败（原因已由 CreateSegment 输出 VS 控制台日志）

    if (entry->heapType == D3D12_HEAP_TYPE_UPLOAD || entry->heapType == D3D12_HEAP_TYPE_READBACK) {
        return AllocateFrom(*newSeg, data, size, m_currentFence, align);
    } else {
        if (data != nullptr) {
            return 0;
        }

        D3D12_GPU_VIRTUAL_ADDRESS newAddr = newSeg->Allocate(size, m_currentFence, align);

        if (newAddr == 0) {
            // 防御性日志：新段大小 >= size，正常不应发生；若发生说明对齐开销超出预算
            wchar_t failMsg[256];
            swprintf_s(failMsg, L"[ERROR] FrameResourceManager: '%hs' 扩容后分配仍失败 (upload %u B, new seg %u MB)\n",
                       entry->name.c_str(), size, newSeg->GetSize() / (1024 * 1024));
            OutputDebugStringW(failMsg);
        }

        if (newAddr != 0) {
            entry->segments[entry->currentSegment].lastFence = m_currentFence;
        }

        return newAddr;
    }
}

void *FrameResourceManager::GetCPUAddress(uint32_t offset) {
    if (!m_initialized) {
        return nullptr;
    }

    // 默认返回第一个 RingBuffer 的 CPU 地址（通常为 ObjectCB）
    if (!m_ringBuffers.empty() && !m_ringBuffers[0].segments.empty()) {
        return m_ringBuffers[0].segments[0].buffer.GetCPUAddress(offset);
    }
    return nullptr;
}

ID3D12Resource *FrameResourceManager::GetBufferResource(const std::string &name) const {
    for (const auto &entry : m_ringBuffers) {
        if (entry.name == name) {
            if (entry.segments.empty())
                return nullptr;
            // 当前段资源（Allocate 固定分配在当前段，SRV 段偏移 = GPU地址 - 当前段基址）
            return entry.segments[entry.currentSegment].buffer.GetResource();
        }
    }
    return nullptr;
}

} // namespace DX12Engine::Renderer
