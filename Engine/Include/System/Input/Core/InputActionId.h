#pragma once
#include <cstdint>
#include <string_view>

namespace DX12Engine {
namespace Input {

/**
 * @brief 编译期字符串哈希函数 (FNV-1a 64-bit)
 * 将人类可读的动作名称（如 "Jump"）转换为唯一的 uint64_t ID。
 */
constexpr uint64_t HashString(std::string_view str) {
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    for (char c : str) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
}

/**
 * @brief 动作 ID 类型定义
 */
using ActionId = uint64_t;

/**
 * @brief 辅助宏：定义动作常量
 *
 * 用法：DEFINE_ACTION(Jump);
 * 展开后：inline constexpr ActionId ActionId_Jump = HashString("Jump");
 */
#define DEFINE_ACTION(name) inline constexpr ActionId ActionId_##name = DX12Engine::Input::HashString(#name)

} // namespace Input
} // namespace DX12Engine