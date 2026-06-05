#pragma once

#include "Renderer/FrameResources/RingBuffer.h"
#include "TerrainResourceType.h"
#include <cstdint>
#include <vector>

namespace DX12Engine::Renderer {

// ============================================================================
// 地形常量缓冲区分配器 - 只负责为地形块分配/释放常量缓冲区空间
// ============================================================================
class TerrainManager {
public:
    static TerrainManager &GetInstance();

    TerrainManager(const TerrainManager &) = delete;
    TerrainManager &operator=(const TerrainManager &) = delete;

    // ========================================================================
    // 初始化/关闭
    // ========================================================================
    void Initialize(ID3D12Device *device, uint32_t bufferSize = 64 * 1024);
    void Shutdown();

    // ========================================================================
    // 常量缓冲区分配/释放
    // ========================================================================

    // 分配常量缓冲区空间，返回偏移量（用于后续上传和访问）
    uint32_t AllocateConstantBuffer(uint64_t fence);

    // 上传常量数据到指定偏移
    void UploadConstant(uint32_t offset, const TerrainConstants &constants, uint64_t fence);

    // 获取 GPU 地址（基于当前帧的 RingBuffer 基址 + 偏移）
    D3D12_GPU_VIRTUAL_ADDRESS GetConstantGPUAddress(uint32_t offset) const;

    // 获取 CPU 地址（用于调试或直接写入）
    void *GetConstantCPUAddress(uint32_t offset) const;

    // ========================================================================
    // 每帧开始/结束
    // ========================================================================
    void BeginFrame(uint64_t fence);
    void EndFrame(uint64_t completedFence);

    // ========================================================================
    // 调试/统计
    // ========================================================================
    uint32_t GetActiveAllocationCount() const { return static_cast<uint32_t>(m_activeAllocations.size()); }
    uint32_t GetBufferSize() const { return m_bufferSize; }
    uint32_t GetUsedSize() const { return m_constantBuffer.GetUsedSize(); }

private:
    TerrainManager() = default;
    ~TerrainManager() = default;

    struct Allocation {
        uint32_t offset;
        uint64_t fence;
    };

    // 重新分配缓冲区（扩容）
    bool ReallocateBuffer(uint32_t requiredSize);

private:
    ID3D12Device *m_device = nullptr;
    RingBuffer m_constantBuffer;
    bool m_initialized = false;
    uint32_t m_bufferSize = 0;

    // 活跃分配记录（用于延迟回收）
    std::vector<Allocation> m_activeAllocations;

    // 常量缓冲区对齐
    static constexpr uint32_t CONSTANT_ALIGNMENT = 256;
};

} // namespace DX12Engine::Renderer