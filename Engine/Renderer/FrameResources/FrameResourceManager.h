#pragma once
#include "FrameResourceConfig.h"
#include "Resource/Core/DescriptorHeapCollection.h" // HeapTag（临时 SRV 槽位 heapTag 参数，规则 17）
#include "Resource/Struct/Descriptor.h"
#include "RingBuffer.h"
#include "Struct/FrameResourceTypes.h"
#include <d3d12.h>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace DX12Engine {

namespace Resource {

class DescriptorHeapCollection;
}

namespace Renderer {
/**
 *
 * @brief 帧资源管理器
 *
 * 管理每帧临时资源（环形缓冲区 + 固定 PassCB）：
 * - 3 帧轮换，每帧独立资源
 * - 每种资源类型独立环形缓冲区
 * - 配合 CommandManager 的围栏进行 GPU 同步
 */
class FrameResourceManager {
public:
    static constexpr uint32_t FRAME_COUNT = 3;

    FrameResourceManager() = default;
    ~FrameResourceManager();

    FrameResourceManager(const FrameResourceManager &) = delete;
    FrameResourceManager &operator=(const FrameResourceManager &) = delete;

    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descriptorHeaps,
                    const FrameResourceConfig &config);
    void Shutdown();

    void BeginFrame(uint64_t completedFence, uint64_t nextFence);

    // ========================================================================
    // Pass Constants（固定位置，每帧覆盖）
    // ========================================================================

    PassConstants &GetPassConstants() { return m_passConstants; }
    D3D12_GPU_VIRTUAL_ADDRESS GetPassCBAddress() const;
    void UpdatePassConstants(); // 将 m_passConstants 拷贝到 GPU

    // ========================================================================
    // 环形缓冲区分配接口（每帧动态分配）
    // ========================================================================

    /**
     * @brief 按名称分配 RingBuffer 空间
     * @param name  配置中定义的名称（如 "Instance", "Skinning"）
     * @param data  上传数据（可为 nullptr 仅分配地址）
     * @param size  请求大小
     * @param alignment 显式对齐（0 = 用条目配置 alignment）——SRV StructuredBuffer 元素对齐修复：
     *                  GPUInstanceData=96B 必须按 96 对齐分配，否则 CreateSRV firstElement=byteOffset/96
     *                  向下取整错位 → CS 读 gInstances 错位全灭（频闪根因，2026-08-09）
     * @return GPU 虚拟地址，失败返回 0
     */
    D3D12_GPU_VIRTUAL_ADDRESS Allocate(const std::string &name, const void *data, uint32_t size,
                                       uint32_t alignment = 0);

    void *GetCPUAddress(uint32_t offset);

    /// 按名获取 RingBuffer 底层资源（供 SRV 创建：段偏移 = GPU地址 - 资源基址）
    ID3D12Resource *GetBufferResource(const std::string &name) const;

    // ========================================================================
    // 调试/监控
    // ========================================================================

    bool IsInitialized() const { return m_initialized; }
    uint64_t GetCurrentFence() const { return m_currentFence; }

private:
    /// 单条目（按 name 命名）多段缓冲池——对齐 Frame.md 策略 1（Unreal 风格）：
    /// 扩容时新建更大段，旧段保留并延迟回收（fence 完成后移除），杜绝扩容销毁 GPU 在用资源（悬垂/TDR）
    struct RingBufferEntry {
        std::string name;
        uint32_t alignment = 256;
        bool allowExpand = true;
        uint32_t maxSize = 256 * 1024 * 1024;
        D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_GENERIC_READ;

        struct Segment {
            RingBuffer buffer;
            uint64_t lastFence = 0; // 本段最后一次分配的 fence（回收判据）
        };

        std::vector<Segment> segments; // 段 0 = 配置初始段；扩容追加
        uint32_t currentSegment = 0;   // 当前分配段（新分配固定此段，SRV 段偏移依赖单段连续性）
    };

    std::vector<RingBufferEntry> m_ringBuffers;

    RingBufferEntry *FindEntry(const std::string &name);

    /// 单段分配尝试（失败返回 0，由 Allocate 决定是否新建段）
    D3D12_GPU_VIRTUAL_ADDRESS AllocateFrom(RingBuffer &buffer, const void *data, uint32_t size, uint64_t fence,
                                           uint32_t alignment);

    /// 新建段（扩容）：大小 = max(1.5x 当前总大小, size)，硬上限 256MB（Frame.md CalculateNewSize 同款）
    /// 返回新段指针（已追加到 entry.segments 并设为 currentSegment），失败返回 nullptr
    RingBuffer *CreateSegment(RingBufferEntry &entry, uint32_t size, uint64_t fence);

    void CreatePassCB(ID3D12Device *device);

    ID3D12Device *m_device = nullptr;
    Resource::DescriptorHeapCollection *m_descriptorHeaps = nullptr;

    // PassCB（独立，非环形）
    Microsoft::WRL::ComPtr<ID3D12Resource> m_passCBResource;
    void *m_passCBMapped = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_passCBAddress = 0;
    PassConstants m_passConstants;

    uint64_t m_currentFence = 0; // 当前帧的围栏值

    bool m_initialized = false;
};

} // namespace Renderer
} // namespace DX12Engine