#pragma once

/**
 * @file SystemRegistry.h
 * @brief System 注册表
 *
 * L4层在此注册System，建立消息到System的映射
 */

#include "SystemTypes.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace DX12Engine::Scheduler {

/**
 * @brief System注册表
 *
 * L4层在初始化时注册所有System，建立消息到System的映射
 *
 * 使用示例：
 * @code
 * // L4层注册System
 * SystemRegistry::Register({
 *     .name = "PlayerMoveSystem",
 *     .func = [](Registry& r, const MessageContext& ctx) {
 *         // 处理玩家移动逻辑
 *     },
 *     .phase = TaskPhase::Update,
 *     .interestedMessages = { PlayerInputEvent::StaticTypeHash }
 * });
 * @endcode
 */
class SystemRegistry {
public:
    /// 注册一个System
    static SystemId Register(SystemInfo info);

    /// 根据ID获取System信息
    static const SystemInfo *GetSystem(SystemId id);

    /// 根据名称获取System信息
    static const SystemInfo *GetSystemByName(const std::string &name);

    /// 获取对某消息感兴趣的所有System
    static std::vector<SystemId> GetInterestedSystems(MessageTypeHash messageType);

    /// 获取所有已注册的System
    static const std::unordered_map<SystemId, SystemInfo> &GetAllSystems();

    /// 清空所有注册
    static void Clear();

private:
    static SystemId s_nextId;
    static std::unordered_map<SystemId, SystemInfo> s_systems;
    static std::unordered_map<std::string, SystemId> s_nameToId;
    static std::unordered_map<MessageTypeHash, std::vector<SystemId>> s_messageToSystems;
};

} // namespace DX12Engine::Scheduler
