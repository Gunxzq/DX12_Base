#pragma once

#include "Renderer/FrameResources/RingBuffer.h"
#include "TerrainResourceType.h"
#include <cstdint>
#include <vector>

namespace DX12Engine::Renderer {

// ============================================================================
// 地形管理器 — 匹配 LightManager 的 Immediate 上传模式
//
// 问题：原实现中，TerrainRenderItemBuilder::BuildTyped() 在 PreRender 阶段
// 分配 RingBuffer 并上传常量，但 Render 阶段读取的是上一帧 PreRender 构建的
// 队列。BeginFrame(nextFence) 调用 Reclaim() 后，上一帧的 RingBuffer 空间
// 被回收，GPU 读取到被覆盖的数据 → 曲面细分因子错乱 → 形变。
//
// 解决方案（LightManager 模式）：
//   1. ImmediateCallback 中调用 UpdateAndUpload()，分配+上传所有地形常量
//   2. Render 阶段使用当帧分配的 GPU 地址，不会被覆盖
//   3. 提供 GPU 地址查找接口，BuildRenderQueue 不再做 RingBuffer 分配
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
    // 旧接口（保留兼容，但推荐使用 UpdateAndUpload）
    // ========================================================================

    uint32_t AllocateConstantBuffer(uint64_t fence);
    void UploadConstant(uint32_t offset, const TerrainConstants &constants, uint64_t fence);
    D3D12_GPU_VIRTUAL_ADDRESS GetConstantGPUAddress(uint32_t offset) const;
    void *GetConstantCPUAddress(uint32_t offset) const;

    // ========================================================================
    // LightManager 模式：每帧 Immediate 回调中一次性上传所有地形常量
    // ========================================================================

    // LightManager 模式：每帧 Immediate 回调中检查是否需要上传
    // 只有常量变化时才重新分配 RingBuffer（匹配 LightManager 的 dirty 标记模式）
    void UpdateAndUpload(uint64_t fence);

    // 设置待上传的地形常量数据（由 Immediate 回调调用）
    void SetPendingConstants(std::vector<TerrainConstants> &&constants) {
        m_pendingConstants = std::move(constants);
    }

    // 标记地形常量为脏（需要重新上传），由外部在常量变化时调用
    void MarkDirty() { m_terrainDirty = true; }

    // 注册一个地形块的 GPU 地址（供 Render 阶段查询）
    void RegisterTerrainBlock(uint32_t blockIndex, D3D12_GPU_VIRTUAL_ADDRESS gpuAddress);

    // 查询地形块的 GPU 常量地址（按索引）
    D3D12_GPU_VIRTUAL_ADDRESS GetTerrainBlockAddress(uint32_t blockIndex) const;

    // 获取所有地形块常量的起始 GPU 地址（作为连续 StructuredBuffer 的基址）
    D3D12_GPU_VIRTUAL_ADDRESS GetTerrainCBBaseAddress() const { return m_terrainCBBaseAddress; }
    uint32_t GetTerrainBlockCount() const { return m_terrainBlockCount; }

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

    // LightManager 模式：批量上传的常量数据缓存
    std::vector<TerrainConstants> m_pendingConstants;

    // 批量上传结果
    D3D12_GPU_VIRTUAL_ADDRESS m_terrainCBBaseAddress = 0;
    uint32_t m_terrainBlockCount = 0;

    // Dirty 标记：只在常量变化时才重新分配 RingBuffer
    bool m_terrainDirty = true; // 初始为脏，首帧必须上传

    // 常量缓冲区对齐
    static constexpr uint32_t CONSTANT_ALIGNMENT = 256;
};

} // namespace DX12Engine::Renderer