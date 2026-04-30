// HandlePool.h
#pragma once
#include "System/Resource/ResourceHandle.h"
#include <atomic>
#include <cassert>
#include <mutex> // 确保包含 mutex
#include <vector>

namespace DX12Engine {
namespace System {
namespace Resource {

enum class ResourceState : uint8_t { Empty, Loading, Ready, Error, PendingRelease };

enum class ResourceType : uint8_t { Unknown, Mesh, Texture, Audio, Shader };

class HandlePool {
public:
    static constexpr uint32_t INITIAL_CAPACITY = 4096;

    HandlePool();
    ~HandlePool();

    // 注意：如果头文件中声明了 Initialize/Shutdown，cpp中必须实现
    // 如果只使用构造函数/析构函数，则不需要声明 Initialize/Shutdown
    // 根据提供的 cpp 代码，似乎没有 Initialize/Shutdown 的公共接口调用，
    // 但 ResourceManager.cpp 调用了 m_handlePool.Initialize() 和 Shutdown()。
    // 因此需要在头文件中添加声明，或者在 cpp 中实现它们。

    void Initialize(); // 添加声明以匹配 ResourceManager 的调用
    void Shutdown();   // 添加声明以匹配 ResourceManager 的调用

    ResourceHandle AllocateSlot(ResourceType type);
    void FreeSlot(ResourceHandle handle);

    void SetState(ResourceHandle handle, ResourceState state);
    ResourceState GetState(ResourceHandle handle) const;

    void SetDataPtr(ResourceHandle handle, void *ptr);
    void *GetDataPtr(ResourceHandle handle) const;

    bool Validate(ResourceHandle handle) const;

    uint32_t GetActiveCount() const;

private:
    mutable std::mutex m_mutex;

    std::vector<ResourceType> m_types;
    std::vector<std::atomic<ResourceState>> m_states;
    std::vector<uint32_t> m_generations;
    std::vector<std::atomic<void *>> m_dataPtrs;

    std::vector<uint32_t> m_freeIndices;

    static constexpr uint32_t INITIAL_CAPACITY = 1024;

    void ExpandCapacity();
};

} // namespace Resource
} // namespace System
} // namespace DX12Engine