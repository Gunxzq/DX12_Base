#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace DX12Engine {
namespace Renderer {

// 堆类型
enum class HeapType : uint32_t {
    Upload,   // D3D12_HEAP_TYPE_UPLOAD (CPU 写入, GPU 读)
    Default,  // D3D12_HEAP_TYPE_DEFAULT (GPU 读写)
    Readback, // D3D12_HEAP_TYPE_READBACK (GPU 写, CPU 读)
};

// 资源标志
enum class ResourceFlags : uint32_t {
    None = 0,
    AllowUnorderedAccess = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    DenyShaderResource = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
    AllowRenderTarget = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
    AllowDepthStencil = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
};

enum class ResourceState : uint32_t {
    GenericRead = D3D12_RESOURCE_STATE_GENERIC_READ,
    UnorderedAccess = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    NonPixelShaderResource = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
    PixelShaderResource = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
    RenderTarget = D3D12_RESOURCE_STATE_RENDER_TARGET,
    DepthWrite = D3D12_RESOURCE_STATE_DEPTH_WRITE,
    DepthRead = D3D12_RESOURCE_STATE_DEPTH_READ,
    CopyDest = D3D12_RESOURCE_STATE_COPY_DEST,
    CopySource = D3D12_RESOURCE_STATE_COPY_SOURCE,
};

NLOHMANN_JSON_SERIALIZE_ENUM(HeapType, {
                                           {HeapType::Upload, "upload"},
                                           {HeapType::Default, "default"},
                                           {HeapType::Readback, "readback"},
                                       })

NLOHMANN_JSON_SERIALIZE_ENUM(ResourceFlags, {
                                                {ResourceFlags::None, "none"},
                                                {ResourceFlags::AllowUnorderedAccess, "allow_unordered_access"},
                                                {ResourceFlags::DenyShaderResource, "deny_shader_resource"},
                                                {ResourceFlags::AllowRenderTarget, "allow_render_target"},
                                                {ResourceFlags::AllowDepthStencil, "allow_depth_stencil"},
                                            })

NLOHMANN_JSON_SERIALIZE_ENUM(ResourceState, {
                                                {ResourceState::GenericRead, "generic_read"},
                                                {ResourceState::UnorderedAccess, "unordered_access"},
                                                {ResourceState::NonPixelShaderResource, "non_pixel_shader_resource"},
                                                {ResourceState::PixelShaderResource, "pixel_shader_resource"},
                                                {ResourceState::RenderTarget, "render_target"},
                                                {ResourceState::DepthWrite, "depth_write"},
                                                {ResourceState::DepthRead, "depth_read"},
                                                {ResourceState::CopyDest, "copy_dest"},
                                                {ResourceState::CopySource, "copy_source"},
                                            })

struct RingBufferConfig {
    std::string name;
    uint32_t initialSize = 16 * 1024 * 1024; // 16MB
    uint32_t alignment = 256;

    HeapType heapType = HeapType::Upload;
    ResourceFlags flags = ResourceFlags::None;
    ResourceState initialState = ResourceState::GenericRead;

    bool allowExpand = true;
    uint32_t maxSize = 256 * 1024 * 1024; // 256MB 上限

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(RingBufferConfig, name, initialSize, alignment, heapType, flags, initialState,
                                   allowExpand, maxSize)
};

struct FrameResourceConfig {
    std::vector<RingBufferConfig> ringBuffers;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(FrameResourceConfig, ringBuffers)
};

inline D3D12_HEAP_TYPE ToD3D12HeapType(HeapType type) {
    switch (type) {
    case HeapType::Upload:
        return D3D12_HEAP_TYPE_UPLOAD;
    case HeapType::Default:
        return D3D12_HEAP_TYPE_DEFAULT;
    case HeapType::Readback:
        return D3D12_HEAP_TYPE_READBACK;
    default:
        return D3D12_HEAP_TYPE_UPLOAD;
    }
}

inline D3D12_RESOURCE_FLAGS ToD3D12Flags(ResourceFlags flags) { return static_cast<D3D12_RESOURCE_FLAGS>(flags); }

inline D3D12_RESOURCE_STATES ToD3D12State(ResourceState state) { return static_cast<D3D12_RESOURCE_STATES>(state); }

} // namespace Renderer
} // namespace DX12Engine

namespace nlohmann {

// HeapType
template <> struct adl_serializer<DX12Engine::Renderer::HeapType> {
    static void from_json(const json &j, DX12Engine::Renderer::HeapType &type) {
        if (j.is_string()) {
            std::string s = j.get<std::string>();
            if (s == "upload")
                type = DX12Engine::Renderer::HeapType::Upload;
            else if (s == "default")
                type = DX12Engine::Renderer::HeapType::Default;
            else if (s == "readback")
                type = DX12Engine::Renderer::HeapType::Readback;
            else
                type = DX12Engine::Renderer::HeapType::Upload; // 默认
        } else if (j.is_number()) {
            // 兼容数字（可选）
            uint32_t v = j.get<uint32_t>();
            if (v == 1)
                type = DX12Engine::Renderer::HeapType::Default;
            else if (v == 2)
                type = DX12Engine::Renderer::HeapType::Readback;
            else
                type = DX12Engine::Renderer::HeapType::Upload;
        } else {
            type = DX12Engine::Renderer::HeapType::Upload;
        }
    }

    static void to_json(json &j, const DX12Engine::Renderer::HeapType &type) {
        switch (type) {
        case DX12Engine::Renderer::HeapType::Upload:
            j = "upload";
            break;
        case DX12Engine::Renderer::HeapType::Default:
            j = "default";
            break;
        case DX12Engine::Renderer::HeapType::Readback:
            j = "readback";
            break;
        default:
            j = "upload";
            break;
        }
    }
};

// ResourceFlags
template <> struct adl_serializer<DX12Engine::Renderer::ResourceFlags> {
    static void from_json(const json &j, DX12Engine::Renderer::ResourceFlags &flags) {
        if (j.is_string()) {
            std::string s = j.get<std::string>();
            if (s == "allow_unordered_access")
                flags = DX12Engine::Renderer::ResourceFlags::AllowUnorderedAccess;
            else if (s == "deny_shader_resource")
                flags = DX12Engine::Renderer::ResourceFlags::DenyShaderResource;
            else if (s == "allow_render_target")
                flags = DX12Engine::Renderer::ResourceFlags::AllowRenderTarget;
            else if (s == "allow_depth_stencil")
                flags = DX12Engine::Renderer::ResourceFlags::AllowDepthStencil;
            else if (s == "none")
                flags = DX12Engine::Renderer::ResourceFlags::None;
            else
                flags = DX12Engine::Renderer::ResourceFlags::None;
        } else if (j.is_number()) {
            uint32_t v = j.get<uint32_t>();
            // 数字直接映射到 D3D12 标志值
            flags = static_cast<DX12Engine::Renderer::ResourceFlags>(v);
        } else {
            flags = DX12Engine::Renderer::ResourceFlags::None;
        }
    }

    static void to_json(json &j, const DX12Engine::Renderer::ResourceFlags &flags) {
        switch (flags) {
        case DX12Engine::Renderer::ResourceFlags::None:
            j = "none";
            break;
        case DX12Engine::Renderer::ResourceFlags::AllowUnorderedAccess:
            j = "allow_unordered_access";
            break;
        case DX12Engine::Renderer::ResourceFlags::DenyShaderResource:
            j = "deny_shader_resource";
            break;
        case DX12Engine::Renderer::ResourceFlags::AllowRenderTarget:
            j = "allow_render_target";
            break;
        case DX12Engine::Renderer::ResourceFlags::AllowDepthStencil:
            j = "allow_depth_stencil";
            break;
        default:
            j = "none";
            break;
        }
    }
};

// ResourceState
template <> struct adl_serializer<DX12Engine::Renderer::ResourceState> {
    static void from_json(const json &j, DX12Engine::Renderer::ResourceState &state) {
        if (j.is_string()) {
            std::string s = j.get<std::string>();
            if (s == "generic_read")
                state = DX12Engine::Renderer::ResourceState::GenericRead;
            else if (s == "unordered_access")
                state = DX12Engine::Renderer::ResourceState::UnorderedAccess;
            else if (s == "non_pixel_shader_resource")
                state = DX12Engine::Renderer::ResourceState::NonPixelShaderResource;
            else if (s == "pixel_shader_resource")
                state = DX12Engine::Renderer::ResourceState::PixelShaderResource;
            else if (s == "render_target")
                state = DX12Engine::Renderer::ResourceState::RenderTarget;
            else if (s == "depth_write")
                state = DX12Engine::Renderer::ResourceState::DepthWrite;
            else if (s == "depth_read")
                state = DX12Engine::Renderer::ResourceState::DepthRead;
            else if (s == "copy_dest")
                state = DX12Engine::Renderer::ResourceState::CopyDest;
            else if (s == "copy_source")
                state = DX12Engine::Renderer::ResourceState::CopySource;
            else
                state = DX12Engine::Renderer::ResourceState::GenericRead;
        } else if (j.is_number()) {
            uint32_t v = j.get<uint32_t>();
            state = static_cast<DX12Engine::Renderer::ResourceState>(v);
        } else {
            state = DX12Engine::Renderer::ResourceState::GenericRead;
        }
    }

    static void to_json(json &j, const DX12Engine::Renderer::ResourceState &state) {
        switch (state) {
        case DX12Engine::Renderer::ResourceState::GenericRead:
            j = "generic_read";
            break;
        case DX12Engine::Renderer::ResourceState::UnorderedAccess:
            j = "unordered_access";
            break;
        case DX12Engine::Renderer::ResourceState::NonPixelShaderResource:
            j = "non_pixel_shader_resource";
            break;
        case DX12Engine::Renderer::ResourceState::PixelShaderResource:
            j = "pixel_shader_resource";
            break;
        case DX12Engine::Renderer::ResourceState::RenderTarget:
            j = "render_target";
            break;
        case DX12Engine::Renderer::ResourceState::DepthWrite:
            j = "depth_write";
            break;
        case DX12Engine::Renderer::ResourceState::DepthRead:
            j = "depth_read";
            break;
        case DX12Engine::Renderer::ResourceState::CopyDest:
            j = "copy_dest";
            break;
        case DX12Engine::Renderer::ResourceState::CopySource:
            j = "copy_source";
            break;
        default:
            j = "generic_read";
            break;
        }
    }
};

} // namespace nlohmann