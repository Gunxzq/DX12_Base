// Resource/Struct/LODMeshHandle.h
#pragma once
#include <cstdint>
#include <functional> // 添加

namespace DX12Engine::Resource {

struct LODMeshHandle {
    uint32_t index : 22;
    uint32_t generation : 10;

    static constexpr LODMeshHandle Invalid() { return {0x3FFFFF, 0}; }

    bool IsValid() const { return index != 0x3FFFFF; }

    operator uint32_t() const { return *reinterpret_cast<const uint32_t *>(this); }

    static LODMeshHandle FromUint32(uint32_t val) {
        LODMeshHandle h;
        *reinterpret_cast<uint32_t *>(&h) = val;
        return h;
    }

    // 相等性比较
    bool operator==(const LODMeshHandle &other) const { return index == other.index && generation == other.generation; }
};

} // namespace DX12Engine::Resource

// 为 LODMeshHandle 提供哈希特化
namespace std {
template <> struct hash<DX12Engine::Resource::LODMeshHandle> {
    size_t operator()(const DX12Engine::Resource::LODMeshHandle &handle) const {
        return static_cast<size_t>(static_cast<uint32_t>(handle));
    }
};
} // namespace std