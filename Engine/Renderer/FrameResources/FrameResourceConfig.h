#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace DX12Engine {
namespace Renderer {

struct RingBufferConfig {
    std::string name;
    uint32_t initialSize = 16 * 1024 * 1024; // 16MB
    uint32_t alignment = 256;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(RingBufferConfig, name, initialSize, alignment)
};

struct FrameResourceConfig {
    std::vector<RingBufferConfig> ringBuffers;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FrameResourceConfig, ringBuffers)
};

} // namespace Renderer
} // namespace DX12Engine
