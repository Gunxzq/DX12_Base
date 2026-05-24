// CpuResourceTypes.h
#pragma once
#include <cstdint>

namespace DX12Engine {
namespace Resource {

// CPU 端资源状态
enum class CpuResourceState : uint8_t { Empty, Loading, Ready, Error, PendingRelease };
enum class CpuResourceType : uint8_t { Unknown, Mesh, Texture, Audio, Shader, UploadBuffer, ReadbackBuffer };

// GPU 端资源状态
enum class GpuResourceState : uint8_t { Empty, Ready, PendingRelease };
enum class GpuResourceType : uint8_t { Unknown, Buffer, Texture2D, Texture3D, TextureCube };

} // namespace Resource
} // namespace DX12Engine