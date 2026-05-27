#pragma once

#include <cstdint>
#include <string_view>

using TypeHash = uint64_t;

constexpr uint64_t HashString(std::string_view str) {
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    for (char c : str) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
}

#define TYPE_HASH(str) (HashString(str))