#pragma once
#include "FrameResourceConfig.h"
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
     * @return GPU 虚拟地址，失败返回 0
     */
    D3D12_GPU_VIRTUAL_ADDRESS Allocate(const std::string &name, const void *data, uint32_t size);

    void *GetCPUAddress(uint32_t offset);

    // ========================================================================
    // 临时描述符（从 DescriptorHeapCollection 分配）
    // ========================================================================

    uint32_t AllocateTemporarySrvSlot();
    void FreeTemporarySrvSlot(uint32_t slot, uint64_t fence);

    // ========================================================================
    // 调试/监控
    // ========================================================================

    bool IsInitialized() const { return m_initialized; }
    uint64_t GetCurrentFence() const { return m_currentFence; }

private:
    struct RingBufferEntry {
        std::string name;
        RingBuffer buffer;
        uint32_t alignment = 256;
    };

    std::vector<RingBufferEntry> m_ringBuffers;

    RingBuffer *FindBuffer(const std::string &name);

    D3D12_GPU_VIRTUAL_ADDRESS AllocateWithRetry(RingBuffer &buffer, const void *data, uint32_t size, uint64_t fence,
                                                  uint32_t alignment = 256);

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