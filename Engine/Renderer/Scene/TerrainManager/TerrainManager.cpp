#include "TerrainManager.h"
#include "Common/Common.h"
#include <algorithm>

using namespace DX12Engine::Renderer;

namespace DX12Engine::Renderer {

// ============================================================================
// 单例实现
// ============================================================================

TerrainManager &TerrainManager::GetInstance() {
    static TerrainManager instance;
    return instance;
}

// ============================================================================
// 初始化/关闭
// ============================================================================

void TerrainManager::Initialize(ID3D12Device *device, uint32_t bufferSize) {
    if (m_initialized) {
        Shutdown();
    }

    if (!device) {
        OutputDebugStringW(L"[ERROR] TerrainManager: Device is null\n");
        return;
    }

    m_device = device;
    m_bufferSize = bufferSize;

    // 初始化常量缓冲区 RingBuffer
    m_constantBuffer.Initialize(device, bufferSize);

    m_activeAllocations.clear();

    m_initialized = true;

    char buf[256];
    sprintf_s(buf, "[INFO] TerrainManager initialized with buffer size %u bytes\n", bufferSize);
    OutputDebugStringA(buf);
}

void TerrainManager::Shutdown() {
    if (!m_initialized) {
        return;
    }

    m_constantBuffer.Shutdown();
    m_activeAllocations.clear();
    m_device = nullptr;
    m_initialized = false;
    m_bufferSize = 0;

    OutputDebugStringW(L"[INFO] TerrainManager shutdown\n");
}

// ============================================================================
// LightManager 模式：每帧 Immediate 回调中一次性上传所有地形常量
//
// 注意：CBV 要求 256 字节对齐，因此每个 TerrainConstants 占用 CONSTANT_ALIGNMENT
// 字节。上传时填充 stride=256 的数组，查询时用 blockIndex * CONSTANT_ALIGNMENT。
// ============================================================================

void TerrainManager::UpdateAndUpload(uint64_t fence) {
    if (!m_initialized || m_pendingConstants.empty()) {
        return;
    }

    // 匹配 LightManager 模式：只在常量变化时才重新分配 RingBuffer
    // 地形常量（World 矩阵、曲面细分参数等）通常是低频更新的
    if (!m_terrainDirty) {
        // 未变化：保留上一帧的 GPU 地址，不重新分配
        // 但需要确保块数量与 pendingConstants 一致
        // （如果块数量变了，强制标记为脏）
        if (m_terrainBlockCount != static_cast<uint32_t>(m_pendingConstants.size())) {
            m_terrainDirty = true;
        } else {
            return; // 无变化，跳过上传
        }
    }

    uint32_t blockCount = static_cast<uint32_t>(m_pendingConstants.size());

    // 构建 256 字节对齐的连续数组（CBV 对齐要求）
    uint32_t strideSize = CONSTANT_ALIGNMENT; // 256
    uint32_t dataSize = blockCount * strideSize;
    std::vector<uint8_t> alignedData(dataSize, 0);

    for (uint32_t i = 0; i < blockCount; ++i) {
        memcpy(alignedData.data() + i * strideSize, &m_pendingConstants[i], sizeof(TerrainConstants));
    }

    // 使用 AllocateUpload 一次性分配 + 拷贝（LightManager 模式）
    m_terrainCBBaseAddress = m_constantBuffer.AllocateUpload(alignedData.data(), dataSize, fence);

    if (m_terrainCBBaseAddress == 0) {
        // RingBuffer 空间不足，尝试扩容
        if (ReallocateBuffer(dataSize * 2)) {
            m_terrainCBBaseAddress = m_constantBuffer.AllocateUpload(alignedData.data(), dataSize, fence);
        }
    }

    m_terrainBlockCount = blockCount;
    m_terrainDirty = false;

    // 匹配 LightManager：Reclaim 使用与 AllocateUpload 相同的 fence
    m_constantBuffer.Reclaim(fence);
}

void TerrainManager::RegisterTerrainBlock(uint32_t blockIndex, D3D12_GPU_VIRTUAL_ADDRESS gpuAddress) {
    // 暂不实现按索引查询（当前架构中 Render 阶段直接使用 TerrainRenderItem 中的 objectCBAddress）
    (void)blockIndex;
    (void)gpuAddress;
}

D3D12_GPU_VIRTUAL_ADDRESS TerrainManager::GetTerrainBlockAddress(uint32_t blockIndex) const {
    if (blockIndex >= m_terrainBlockCount || m_terrainCBBaseAddress == 0) {
        return 0;
    }
    return m_terrainCBBaseAddress + blockIndex * CONSTANT_ALIGNMENT;
}

// ============================================================================
// 每帧开始/结束
// ============================================================================

void TerrainManager::BeginFrame(uint64_t fence) {
    if (!m_initialized) {
        return;
    }

    // 清空上帧的 pending 常量数据，准备接收新数据
    m_pendingConstants.clear();
}

void TerrainManager::EndFrame(uint64_t completedFence) {
    if (!m_initialized) {
        return;
    }

    // 清理已完成帧的分配记录
    auto it = m_activeAllocations.begin();
    while (it != m_activeAllocations.end()) {
        if (completedFence >= it->fence) {
            it = m_activeAllocations.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================================
// 内部方法
// ============================================================================

bool TerrainManager::ReallocateBuffer(uint32_t requiredSize) {
    if (!m_initialized || !m_device) {
        return false;
    }

    uint32_t newSize = m_bufferSize;
    while (newSize < requiredSize && newSize < 16 * 1024 * 1024) { // 最大 16MB
        newSize *= 2;
    }

    if (newSize == m_bufferSize) {
        return false;
    }

    // 保存当前活跃分配的偏移（需要在扩容后重新分配）
    std::vector<uint32_t> activeOffsets;
    for (const auto &alloc : m_activeAllocations) {
        activeOffsets.push_back(alloc.offset);
    }

    // 重新初始化 RingBuffer
    m_constantBuffer.Shutdown();
    m_constantBuffer.Initialize(m_device, newSize);

    // 注意：扩容后原有的偏移量失效，需要重新分配
    // 调用者需要处理重新上传数据
    m_activeAllocations.clear();

    char buf[256];
    sprintf_s(buf, "[INFO] TerrainManager reallocated buffer to %u bytes\n", newSize);
    OutputDebugStringA(buf);

    m_bufferSize = newSize;
    return true;
}

} // namespace DX12Engine::Renderer