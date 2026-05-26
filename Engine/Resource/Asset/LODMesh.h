// Resource/Asset/LODMesh.h
#pragma once
#include "Resource/Struct/GeometryHandle.h"
#include <cstdint>
#include <vector>

namespace DX12Engine::Resource {

struct LODMesh {
    std::vector<GeometryHandle> lodChain; // LOD0, LOD1, LOD2...

    GeometryHandle GetLODByIndex(uint32_t index) const {
        if (index < lodChain.size()) {
            return lodChain[index];
        }
        return GeometryHandle::Invalid();
    }

    GeometryHandle GetHighestLOD() const { return lodChain.empty() ? GeometryHandle::Invalid() : lodChain.front(); }

    GeometryHandle GetLowestLOD() const { return lodChain.empty() ? GeometryHandle::Invalid() : lodChain.back(); }

    size_t GetLODCount() const { return lodChain.size(); }
    bool IsValid() const { return !lodChain.empty(); }
};

} // namespace DX12Engine::Resource