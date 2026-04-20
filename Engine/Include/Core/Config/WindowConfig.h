#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace DX12Engine {
namespace Core {

// ========================================================================
// 窗口配置
// ========================================================================

struct WindowConfig {
    std::wstring title = L"DX12 Engine";
    uint32_t width = 1280;
    uint32_t height = 720;
    std::string mode = "windowed";
    bool resizable = true;
    bool maximizable = true;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(WindowConfig, title, width, height, mode, resizable, maximizable)
};

} // namespace Core
} // namespace DX12Engine
