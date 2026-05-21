#pragma once

// 1. 先包含 DX12 头文件，让 ID3D12Resource 类型可见
#include <d3d12.h>
#include <dxgi.h>

// 4. 包含 ImGui 头文件
#include "ThirdParty/imgui/backends/imgui_impl_dx12.h"
#include "ThirdParty/imgui/backends/imgui_impl_win32.h"
#include "ThirdParty/imgui/imgui.h"