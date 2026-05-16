
# GPU资源管理器

异步上传	添加 UploadBuffer 机制	中
资源别名	多个句柄指向同一资源（共享纹理）	低
资源池	小 Buffer 池化复用	低




你说得对。**在当前架构下，异步上传的复杂性被大幅简化了。**

---

## 一、为什么不需要复杂的异步上传

| 你的架构特性 | 影响 |
|:------------|:-----|
| **工作线程已存在** | `TaskFlow` 工作线程可以并行加载文件 |
| **`CopyQueue` 已创建** | `CommandManager` 已有三种队列 |
| **事件系统驱动** | 加载完成后发送 `ResourceLoadedEvent` |
| **`GpuResourceManager` 已集成** | 资源创建和释放已有框架 |

**你不需要实现一个独立的异步上传系统，只需要在现有框架上添加几个方法。**

---

## 二、简化实现

```cpp
// GpuResourceManager 添加
ResourceHandle GpuResourceManager::CreateBufferAsync(
    const void* data, size_t size, 
    ID3D12Device* device, 
    ID3D12CommandQueue* copyQueue) {
    
    // 1. 分配句柄
    ResourceHandle handle = m_handlePool.AllocateSlot(ResourceType::Mesh, 0);
    m_handlePool.SetState(handle, ResourceState::Loading);
    
    // 2. 创建 DefaultBuffer（GPU 显存）
    auto defaultBuffer = CreateDefaultBuffer(size);
    
    // 3. 创建 UploadBuffer（CPU 可写）
    auto uploadBuffer = CreateUploadBuffer(data, size);
    
    // 4. 录制 Copy 命令
    auto cmdList = m_copyListPool.AcquireHandle();
    cmdList->CopyBufferRegion(defaultBuffer, 0, uploadBuffer, 0, size);
    cmdList->Close();
    
    // 5. 提交并记录围栏值
    copyQueue->ExecuteCommandLists(1, cmdList);
    uint64_t fenceValue = m_fenceManager.Signal(COPY, copyQueue);
    
    // 6. 记录待处理上传
    m_pendingUploads.push_back({handle, fenceValue, defaultBuffer, uploadBuffer});
    
    return handle;
}

void GpuResourceManager::Update(uint64_t completedFence) {
    // 检查上传完成
    for (auto& upload : m_pendingUploads) {
        if (completedFence >= upload.fenceValue) {
            upload.uploadBuffer->Release();  // 释放上传缓冲
            m_handlePool.SetState(upload.handle, ResourceState::Ready);
            // 发送事件
            EventBus::Post(ResourceLoadedEvent{upload.handle});
        }
    }
}
```

---

## 三、事件系统的利用

```cpp
// 游戏层
REGISTER_SYSTEM(ResourceReadySystem, Update, Main)
    .WithMessage<ResourceLoadedEvent>()
    .Func([](ECS::Registry& reg, const MessageContext& ctx) {
        auto handle = ctx.GetResourceHandle();
        // 资源已就绪，可以创建 MeshComponent
    });
```

---

## 四、总结

| 复杂方案 | 你的简化方案 |
|:---------|:-------------|
| 独立的异步加载系统 | 利用现有工作线程 |
| 专门的加载队列 | 使用现有 `CopyQueue` |
| 手动同步管理 | 利用事件系统通知 |
| 需要新的同步机制 | 复用 `FenceManager` |

**你不需要重新发明轮子。`CommandManager` + `FenceManager` + 事件系统已经提供了异步上传所需的基础设施。**