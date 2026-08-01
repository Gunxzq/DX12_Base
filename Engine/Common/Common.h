// ========== Common.h ==========
#pragma once

// ---------- 平台特定代码 ----------
#include "WindowsPlatform.h"

// ---------- 断言系统 ----------
#include "EngineAssert.h"

// ---------- 平台目标版本 ----------
#include <SDKDDKVer.h>

// ---------- DirectX 包装 ----------
#include "d3dUtil.h"

// ---------- 其他 ----------
#include "ThrowHelper.h"

// ========== 预编译头稳定层 ==========

// STL 稳定层
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// DirectX 稳定层
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <d3d12.h>

// 第三方稳定层
#include <nlohmann/json_fwd.hpp>
