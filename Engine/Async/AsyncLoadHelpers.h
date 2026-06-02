#pragma once

#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/MaterialHandle.h"
#include "Resource/Struct/TextureHandle.h"

namespace DX12Engine::Async {

/**
 * @brief 从 payload 解码 requestId
 */
inline uint32_t DecodeRequestId(uint64_t payload) {
    return static_cast<uint32_t>((payload >> 42) & 0x3FFFFF); // 22 bits
}

/**
 * @brief 从 payload 解码句柄索引
 */
inline uint32_t DecodeHandleIndex(uint64_t payload) { return static_cast<uint32_t>(payload & 0xFFFFFFFF); }

/**
 * @brief 从 payload 解码句柄世代号
 */
inline uint32_t DecodeHandleGeneration(uint64_t payload) {
    return static_cast<uint32_t>((payload >> 32) & 0x3FF); // 10 bits
}

/**
 * @brief 从 payload 解码几何体句柄
 */
inline Resource::GeometryHandle DecodeGeometryHandle(uint64_t payload) {
    Resource::GeometryHandle handle;
    handle.index = static_cast<uint32_t>(payload & 0xFFFFFFFF);
    handle.generation = static_cast<uint32_t>((payload >> 32) & 0x3FF);
    return handle;
}

/**
 * @brief 从 payload 解码材质句柄
 */
inline Resource::MaterialHandle DecodeMaterialHandle(uint64_t payload) {
    Resource::MaterialHandle handle;
    handle.index = static_cast<uint32_t>(payload & 0xFFFFFFFF);
    handle.generation = static_cast<uint32_t>((payload >> 32) & 0x3FF);
    return handle;
}

/**
 * @brief 从 payload 解码纹理句柄
 */
inline Resource::TextureHandle DecodeTextureHandle(uint64_t payload) {
    Resource::TextureHandle handle;
    handle.index = static_cast<uint32_t>(payload & 0xFFFFFFFF);
    handle.generation = static_cast<uint32_t>((payload >> 32) & 0x3FF);
    return handle;
}

} // namespace DX12Engine::Async