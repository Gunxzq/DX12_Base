#pragma once
#include <cstdint>
#include <vector>

namespace DX12Engine::Renderer {

// ============================================================================
// LOD 全局配置
// ============================================================================

struct LODConfig {
    // 距离阈值（米），按顺序从高到低
    std::vector<float> distanceThresholds; // 例如：{10.0f, 30.0f, 60.0f} 表示：
                                           //   距离 <= 10.0f → LOD0
                                           //   距离 <= 30.0f → LOD1
                                           //   距离 <= 60.0f → LOD2
                                           //   距离 > 60.0f  → LOD3（最后一级）

    LODConfig() = default;
    explicit LODConfig(const std::vector<float> &thresholds) : distanceThresholds(thresholds) {}

    // 根据距离获取 LOD 等级索引
    uint32_t GetLODIndex(float distance) const {
        for (size_t i = 0; i < distanceThresholds.size(); ++i) {
            if (distance <= distanceThresholds[i])
                return static_cast<uint32_t>(i);
        }
        return static_cast<uint32_t>(distanceThresholds.size());
    }

    uint32_t GetLODCount() const { return static_cast<uint32_t>(distanceThresholds.size()) + 1; }

    bool IsValid() const { return !distanceThresholds.empty(); }
};

} // namespace DX12Engine::Renderer