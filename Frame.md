对于大型商业引擎（Unreal、Unity、Frostbite等），**运行时扩容是必要且精心设计的特性**，而不是需要避免的操作。让我详细分析他们的实现方案：

## 一、为什么需要运行时扩容

### 1. 无法预知所有场景
- **开放世界**：资源负载动态变化巨大
- **用户生成内容**：无法预判模型复杂度
- **内存碎片**：即使总容量够，碎片化导致分配失败

### 2. 节省内存
```cpp
// 而不是每个缓冲区都分配 256MB
// 根据实际使用动态调整
```

## 二、大型引擎的扩容策略

### 策略 1：多级缓冲区池（Unreal 风格）

```cpp
class DynamicRingBufferPool {
    struct BufferSegment {
        ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS gpuAddress;
        uint32_t size;
        uint64_t fenceValue;
        bool isActive;
    };
    
    std::vector<std::unique_ptr<BufferSegment>> m_segments;
    uint32_t m_currentSegment = 0;
    uint32_t m_alignment = 256;
    
public:
    D3D12_GPU_VIRTUAL_ADDRESS Allocate(uint32_t size, uint64_t fence) {
        // 1. 尝试在当前 segment 分配
        auto addr = TryAllocateFromCurrent(size, fence);
        if (addr != 0) return addr;
        
        // 2. 尝试在其他已存在的 segment 分配（环形使用）
        for (uint32_t i = 0; i < m_segments.size(); i++) {
            if (i == m_currentSegment) continue;
            addr = TryAllocateFromSegment(i, size, fence);
            if (addr != 0) {
                m_currentSegment = i;
                return addr;
            }
        }
        
        // 3. 创建新 segment（扩容）
        return CreateNewSegment(size, fence);
    }
    
private:
    D3D12_GPU_VIRTUAL_ADDRESS CreateNewSegment(uint32_t size, uint64_t fence) {
        // 计算新大小：按需增长，有上限
        uint32_t newSize = CalculateNewSize(size);
        
        auto newSegment = std::make_unique<BufferSegment>();
        newSegment->size = newSize;
        newSegment->fenceValue = fence;
        newSegment->isActive = true;
        
        // 创建新缓冲区
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(newSize);
        m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&newSegment->resource));
        
        newSegment->resource->Map(0, nullptr, &m_mappedBase);
        newSegment->gpuAddress = newSegment->resource->GetGPUVirtualAddress();
        
        // 分配请求的大小
        auto result = newSegment->gpuAddress;
        m_segments.push_back(std::move(newSegment));
        m_currentSegment = m_segments.size() - 1;
        
        // 可选：延迟回收旧 segments
        ScheduleOldSegmentReclaim(fence);
        
        return result;
    }
    
    uint32_t CalculateNewSize(uint32_t requestedSize) {
        // 平滑增长策略
        uint32_t currentTotal = GetTotalSize();
        
        if (currentTotal == 0) {
            return std::max(16 * 1024 * 1024, requestedSize); // 初始 16MB
        }
        
        // 增长因子：1.5x 或 2x，取决于场景
        float growthFactor = (requestedSize > currentTotal * 0.8f) ? 2.0f : 1.5f;
        uint32_t newSize = std::max(static_cast<uint32_t>(currentTotal * growthFactor), requestedSize);
        
        // 硬上限：256MB per buffer
        return std::min(newSize, 256 * 1024 * 1024);
    }
};
```

### 策略 2：虚拟内存映射式（EA/Frostbite 风格）

```cpp
class VirtualRingBuffer {
    // 预保留虚拟地址空间，按需提交物理内存
    ComPtr<ID3D12Resource> m_reservedMemory;  // 预留 1GB 虚拟地址
    uint32_t m_committedSize = 0;              // 实际提交的物理内存
    
public:
    bool Initialize(ID3D12Device* device) {
        // 1. 预留虚拟地址空间（不提交物理内存）
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 1024 * 1024 * 1024ULL;  // 1GB 虚拟地址
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_UPLOAD);
        
        // 使用 D3D12_HEAP_FLAG_CREATE_NOT_ZEROED 和保留模式
        device->CreateCommittedResource(&heapProps, 
            D3D12_HEAP_FLAG_CREATE_NOT_ZEROED,  // 不初始化内存
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_reservedMemory));
        
        m_reservedMemory->Map(0, nullptr, &m_mappedBase);
        m_gpuBase = m_reservedMemory->GetGPUVirtualAddress();
        return true;
    }
    
    void Expand(uint32_t newSize) {
        if (newSize <= m_committedSize) return;
        
        // 在 DX12 中，需要特殊处理
        // 实际方案：创建更大的资源，复制数据
        // 或者使用多个资源组成的链表
        
        // 简化：使用堆上的 Placeable Resources
        D3D12_RESOURCE_DESC desc = m_reservedMemory->GetDesc();
        desc.Width = newSize;
        
        ComPtr<ID3D12Resource> newResource;
        m_device->CreateCommittedResource(/* ... */, &desc, /* ... */);
        
        // 复制旧数据
        m_commandQueue->CopyResource(newResource.Get(), m_reservedMemory.Get());
        
        // 等待完成
        WaitForGPU();
        
        // 替换
        m_reservedMemory = newResource;
        m_committedSize = newSize;
    }
};
```

### 策略 3：分块环形缓冲区（Call of Duty 风格）

```cpp
class ChunkedRingBuffer {
    static constexpr uint32_t CHUNK_SIZE = 4 * 1024 * 1024;  // 4MB 每块
    
    struct Chunk {
        ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS gpuStart;
        D3D12_GPU_VIRTUAL_ADDRESS gpuEnd;
        uint32_t size;
        uint64_t lastUsedFence;
        bool inUse;
    };
    
    std::vector<Chunk> m_chunks;
    uint32_t m_headChunk = 0;
    uint32_t m_headOffset = 0;
    uint32_t m_tailChunk = 0;
    uint32_t m_tailOffset = 0;
    
public:
    D3D12_GPU_VIRTUAL_ADDRESS Allocate(uint32_t size, uint64_t fence) {
        // 1. 在当前 chunk 尝试分配
        auto& currentChunk = m_chunks[m_headChunk];
        if (m_headOffset + size <= currentChunk.size) {
            auto result = currentChunk.gpuStart + m_headOffset;
            
            // 对齐处理
            m_headOffset = Align(m_headOffset + size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            return result;
        }
        
        // 2. 移动到下一个 chunk
        m_headChunk = (m_headChunk + 1) % m_chunks.size();
        m_headOffset = 0;
        
        if (m_headChunk == m_tailChunk) {
            // 环形冲突，需要扩容
            return ExpandAndAllocate(size, fence);
        }
        
        return Allocate(size, fence);  // 重试
    }
    
private:
    D3D12_GPU_VIRTUAL_ADDRESS ExpandAndAllocate(uint32_t size, uint64_t fence) {
        // 添加新 chunk
        Chunk newChunk;
        newChunk.size = CHUNK_SIZE;
        CreateChunk(newChunk);
        
        // 插入到当前 head 位置
        m_chunks.insert(m_chunks.begin() + m_headChunk, std::move(newChunk));
        m_headChunk++;  // 调整索引
        
        // 重试分配
        return Allocate(size, fence);
    }
};
```

## 四、关键设计原则

### 1. 平滑扩容，避免卡顿
```cpp
class AdaptiveRingBuffer {
    void PredictiveExpand() {
        // 监控使用趋势
        float usageRate = (float)m_allocatedSize / m_size;
        float growthRate = (m_allocatedSize - m_lastFrameAllocated) / (float)m_size;
        
        if (usageRate > 0.85f && growthRate > 0.1f) {
            // 预扩容，避免运行时分配失败
            uint32_t newSize = m_size * 1.2f;  // 20% 增长
            ScheduleAsyncExpand(newSize);
        }
    }
};
```

### 2. 延迟回收 + 双缓冲
```cpp
void FrameResourceManager::ExpandRingBuffer(RingBuffer& buffer, uint32_t newSize) {
    // 创建新缓冲区
    RingBuffer newBuffer;
    newBuffer.Initialize(m_device, newSize);
    
    // 保留旧缓冲区到回收队列
    m_pendingExpandBuffers.push({
        .oldBuffer = std::move(buffer),
        .fence = m_currentFence + FRAME_COUNT  // N 帧后回收
    });
    
    // 替换为新缓冲区
    buffer = std::move(newBuffer);
}

void FrameResourceManager::ReclaimExpandedBuffers(uint64_t completedFence) {
    while (!m_pendingExpandBuffers.empty() && 
           m_pendingExpandBuffers.front().fence <= completedFence) {
        m_pendingExpandBuffers.pop();  // 旧缓冲区自动析构
    }
}
```

### 3. 分级扩容策略
```cpp
enum class ExpandLevel {
    None,
    Small,   // +25%
    Medium,  // +50%
    Large,   // +100%
    Max      // 达到上限，采取fallback策略
};

ExpandLevel DetermineExpandLevel(uint32_t requested, uint32_t current) {
    float ratio = (float)requested / current;
    if (ratio < 0.2f) return ExpandLevel::Small;
    if (ratio < 0.5f) return ExpandLevel::Medium;
    if (ratio < 1.0f) return ExpandLevel::Large;
    return ExpandLevel::Max;
}
```

## 五、实际建议

对于您的引擎，考虑到复杂度，建议采用**渐进式方案**：

```cpp
// 1. 短期：增大初始值 + 监控
const uint32_t INITIAL_SIZE = 64 * 1024 * 1024;  // 64MB
// 添加性能计数器，观察实际使用峰值

// 2. 中期：实现延迟回收式扩容
// 3. 长期：如果需要，实现 chunked 或 pooled 方案
```

大型引擎的扩容策略核心是：**永远不丢失数据、平滑过渡、预测性扩容避免卡顿**。