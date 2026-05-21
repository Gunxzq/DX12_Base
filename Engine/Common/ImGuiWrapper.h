#pragma once

// 必须包含 Common.h 之前定义这些宏
#define IMGUI_IMPL_DX12_USE_SR_DESCRIPTOR_INDEXING
#define ImTextureID ID3D12Resource *

#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"