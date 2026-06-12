#pragma once

#include "Renderer/FrameResources/RingBuffer.h"
#include "TerrainResourceType.h"
#include <cstdint>
#include <vector>

namespace DX12Engine::Renderer {

// 地形管理器 — 匹配 LightManager 的 Immediate 上传模式
class TerrainManager {
public:
    static TerrainManager &GetInstance();

    TerrainManager(const TerrainManager &) = delete;
    TerrainManager &operator=(const TerrainManager &) = delete;

    // 初始化/关闭
    void Initialize(ID3D12Device *device, uint32_t bufferSize = 64 * 1024);
    void Shutdown();

    void UpdateAndUpload(uint64_t fence);

    // 设置待上传的地形常量数据（由 Immediate 回调调用）
    void SetPendingConstants(std::vector<TerrainConstants> &&constants) { m_pendingConstants = std::move(constants); }

    // 标记地形常量为脏（需要重新上传），由外部在常量变化时调用
    void MarkDirty() { m_terrainDirty = true; }

    // 注册一个地形块的 GPU 地址（供 Render 阶段查询）
    void RegisterTerrainBlock(uint32_t blockIndex, D3D12_GPU_VIRTUAL_ADDRESS gpuAddress);

    // 查询地形块的 GPU 常量地址（按索引）
    D3D12_GPU_VIRTUAL_ADDRESS GetTerrainBlockAddress(uint32_t blockIndex) const;

    // 获取所有地形块常量的起始 GPU 地址（作为连续 StructuredBuffer 的基址）
    D3D12_GPU_VIRTUAL_ADDRESS GetTerrainCBBaseAddress() const { return m_terrainCBBaseAddress; }
    uint32_t GetTerrainBlockCount() const { return m_terrainBlockCount; }

    // 每帧开始/结束
    void BeginFrame(uint64_t fence);
    void EndFrame(uint64_t completedFence);

    // 调试/统计
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