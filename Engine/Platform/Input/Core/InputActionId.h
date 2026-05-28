#pragma once
#include "Math/HashTypes.h"

#include "Common/Common.h"

namespace DX12Engine {
namespace Input {

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
#define DEFINE_ACTION(name) inline constexpr ActionId ActionId_##name = TYPE_HASH(#name)

} // namespace Input
} // namespace DX12Engine