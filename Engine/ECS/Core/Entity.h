#pragma once
#include <entt/entt.hpp>

namespace DX12Engine {

namespace ECS {

using Entity = entt::entity;
inline constexpr Entity INVALID_ENTITY = entt::null;

} // namespace ECS

} // namespace DX12Engine