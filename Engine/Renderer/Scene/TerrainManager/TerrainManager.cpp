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
// 常量缓冲区分配/释放
// ============================================================================

uint32_t TerrainManager::AllocateConstantBuffer(uint64_t fence) {
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = m_constantBuffer.Allocate(CONSTANT_ALIGNMENT, fence);
    if (gpuAddr == 0) {
        return UINT32_MAX; // 分配失败，返回无效标记
    }

    uint32_t offset = static_cast<uint32_t>(gpuAddr - m_constantBuffer.GetGPUAddress());

    // 记录分配（用于调试/追踪）
    m_activeAllocations.push_back({offset, fence});

    return offset;
}

void TerrainManager::UploadConstant(uint32_t offset, const TerrainConstants &constants, uint64_t fence) {
    if (!m_initialized || offset == UINT32_MAX) {
        return;
    }

    // 获取 CPU 地址并写入数据
    void *cpuAddress = m_constantBuffer.GetCPUAddress(offset);
    if (cpuAddress) {
        memcpy(cpuAddress, &constants, sizeof(TerrainConstants));
    } else {
        OutputDebugStringW(L"[ERROR] TerrainManager: Failed to get CPU address for constant buffer\n");
    }
}

D3D12_GPU_VIRTUAL_ADDRESS TerrainManager::GetConstantGPUAddress(uint32_t offset) const {
    if (!m_initialized || offset == UINT32_MAX) {
        return 0;
    }

    return m_constantBuffer.GetGPUAddress(offset);
}

void *TerrainManager::GetConstantCPUAddress(uint32_t offset) const {
    if (!m_initialized || offset == UINT32_MAX) {
        return nullptr;
    }

    return m_constantBuffer.GetCPUAddress(offset);
}

// ============================================================================
// 每帧开始/结束
// ============================================================================

void TerrainManager::BeginFrame(uint64_t fence) {
    if (!m_initialized) {
        return;
    }

    m_constantBuffer.Reclaim(fence);
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