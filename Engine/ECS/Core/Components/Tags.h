#pragma once

namespace DX12Engine::ECS {

// 不透明实体标记
struct OpaqueTag { int _dummy = 0; };

// 透明实体标记
struct TransparentTag { int _dummy = 0; };

// 蒙皮标记
struct SkinnedTag { int _dummy = 0; };

} // namespace DX12Engine::ECS
