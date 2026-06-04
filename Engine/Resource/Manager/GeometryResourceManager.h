#pragma once

#include "Math/BoundingVolume.h"
#include "Resource/Geometry/GridGeometry.h"
#include "Resource/Geometry/PatchMesh.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/Struct/GeometryHandle.h"
#include <cstdint>
#include <unordered_map>
#include <variant>
#include <vector>

namespace DX12Engine {

namespace Math {
using BoundingVolumeVariant = Math::BoundingVolumeVariant;
}

namespace Resource {
// 定义所有支持的几何体类型
using GeometryVariant = std::variant<TriangleMesh, PatchMesh, GridGeometry>;

// ============================================================================
// 几何体资源管理器
// ============================================================================

class GeometryResourceManager {
public:
    GeometryResourceManager() = default;
    ~GeometryResourceManager() = default;

    // 禁止拷贝
    GeometryResourceManager(const GeometryResourceManager &) = delete;
    GeometryResourceManager &operator=(const GeometryResourceManager &) = delete;

    // ========================================================================
    // 初始化/关闭
    // ========================================================================
    void Initialize(uint32_t initialCapacity = 1024);
    void Shutdown();

    // ========================================================================
    // 统一注册接口（模板自动推导）
    // ========================================================================
    template <typename T> GeometryHandle RegisterGeometry(const T &geometry) {
        return RegisterGeometryVariant(geometry);
    }

    // ========================================================================
    // 统一查询接口（类型安全）
    // ========================================================================
    template <typename T> const T *GetGeometry(GeometryHandle handle) const {
        const auto *variant = GetGeometryVariant(handle);
        if (!variant)
            return nullptr;
        return std::get_if<T>(variant);
    }

    template <typename T> T *GetGeometry(GeometryHandle handle) {
        auto *variant = GetGeometryVariant(handle);
        if (!variant)
            return nullptr;
        return std::get_if<T>(variant);
    }

    // ========================================================================
    // 类型信息查询
    // ========================================================================
    size_t GetGeometryTypeIndex(GeometryHandle handle) const;
    const char *GetGeometryTypeName(GeometryHandle handle) const;

    // ========================================================================
    // 通用查询
    // ========================================================================
    bool IsValid(GeometryHandle handle) const;
    const Math::BoundingVolumeVariant *GetBounds(GeometryHandle handle) const;

    // ========================================================================
    // 几何体释放
    // ========================================================================
    void Release(GeometryHandle handle, uint64_t fenceValue);
    void Reclaim(uint64_t completedFence);

    // ========================================================================
    // 调试/统计
    // ========================================================================
    uint32_t GetActiveCount() const;
    uint32_t GetCapacity() const;
    uint32_t GetPendingReleaseCount() const;

private:
    // 几何体条目
    struct Entry {
        GeometryVariant geometry; // 几何体数据
        uint32_t generation = 0;  // 世代号（用于句柄验证）
        bool inUse = false;       // 是否使用中
    };

    // 待释放条目
    struct PendingRelease {
        uint32_t index;      // 条目索引
        uint32_t generation; // 世代号（验证用）
        uint64_t fenceValue; // GPU 围栏值
    };

private:
    // 分配新条目
    uint32_t AllocateEntry();

    // 释放条目（内部，立即释放）
    void FreeEntry(uint32_t index);
    GeometryHandle RegisterGeometryVariant(const GeometryVariant &geometry);
    const GeometryVariant *GetGeometryVariant(GeometryHandle handle) const;
    GeometryVariant *GetGeometryVariant(GeometryHandle handle);

private:
    std::vector<Entry> m_entries;                  // 几何体条目数组
    std::vector<uint32_t> m_freeList;              // 空闲索引列表
    std::vector<PendingRelease> m_pendingReleases; // 待释放队列
    uint32_t m_nextGeneration = 1;                 // 下一代世代号
    uint32_t m_capacity = 0;                       // 当前容量
    bool m_initialized = false;                    // 是否已初始化

    // 容量参数
    static constexpr uint32_t INITIAL_CAPACITY = 1024;
    static constexpr uint32_t MAX_CAPACITY = 65536;
};

} // namespace Resource

} // namespace DX12Engine