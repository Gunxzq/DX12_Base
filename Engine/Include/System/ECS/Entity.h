#pragma once
#include <entt/entt.hpp>

namespace DX12::ECS {

using Entity = entt::entity;
inline constexpr Entity INVALID_ENTITY = entt::null;

} // namespace DX12::ECS