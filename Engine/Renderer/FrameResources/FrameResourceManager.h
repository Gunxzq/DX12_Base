#pragma once
#include "Resource/Struct/Descriptor.h"
#include "RingBuffer.h"
#include "Struct/FrameResourceTypes.h"
#include <array>
#include <d3d12.h>
#include <memory>
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

    void Initialize(ID3D12Device *device, Resource::DescriptorHeapCollection *descriptorHeaps);
    void Shutdown();

    void BeginFrame(uint64_t completedFence, uint64_t nextFence);

    // ========================================================================
    // Pass Constants（固定位置，每帧覆盖）
    // ========================================================================

    PassConstants &GetPassConstants() { return m_passConstants; }
    D3D12_GPU_VIRTUAL_ADDRESS GetPassCBAddress() const;
    void UpdatePassConstants(); // 将 m_passConstants 拷贝到 GPU

    // ========================================================================
    // 环形缓冲区分配接口
    // ========================================================================

    D3D12_GPU_VIRTUAL_ADDRESS AllocateObjectCB(const void *data, uint32_t size);
    D3D12_GPU_VIRTUAL_ADDRESS AllocateSkinning(const void *data, uint32_t size);
    D3D12_GPU_VIRTUAL_ADDRESS AllocateInstance(const void *data, uint32_t size);
    D3D12_GPU_VIRTUAL_ADDRESS AllocateLight(const void *data, uint32_t size);
    D3D12_GPU_VIRTUAL_ADDRESS AllocateMaterialCB(const void *data, uint32_t size);

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

private:
    RingBuffer m_objectCB;
    RingBuffer m_skinning;
    RingBuffer m_instance;
    RingBuffer m_light;
    RingBuffer m_materialCB;

    D3D12_GPU_VIRTUAL_ADDRESS AllocateWithRetry(RingBuffer &buffer, const void *data, uint32_t size, uint64_t fence);

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