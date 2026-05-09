#include "System/Framework/SystemRegistry.h"

namespace DX12Engine::Scheduler {

// ========================================================================
// SystemRegistry 实现
// ========================================================================

SystemId SystemRegistry::s_nextId = 1;
std::unordered_map<SystemId, SystemInfo> SystemRegistry::s_systems;
std::unordered_map<std::string, SystemId> SystemRegistry::s_nameToId;
std::unordered_map<MessageTypeHash, std::vector<SystemId>> SystemRegistry::s_messageToSystems;

SystemId SystemRegistry::Register(SystemInfo info) {
    SystemId id = s_nextId++;
    info.id = id;

    // 注册到各索引表
    s_systems[id] = info;
    s_nameToId[info.name] = id;

    // 建立消息到System的映射
    for (auto msgType : info.interestedMessages) {
        s_messageToSystems[msgType].push_back(id);
    }

    return id;
}

const SystemInfo *SystemRegistry::GetSystem(SystemId id) {
    auto it = s_systems.find(id);
    if (it != s_systems.end()) {
        return &it->second;
    }
    return nullptr;
}

const SystemInfo *SystemRegistry::GetSystemByName(const std::string &name) {
    auto it = s_nameToId.find(name);
    if (it != s_nameToId.end()) {
        return GetSystem(it->second);
    }
    return nullptr;
}

std::vector<SystemId> SystemRegistry::GetInterestedSystems(MessageTypeHash messageType) {
    auto it = s_messageToSystems.find(messageType);
    if (it != s_messageToSystems.end()) {
        return it->second;
    }
    return {};
}

const std::unordered_map<SystemId, SystemInfo> &SystemRegistry::GetAllSystems() { return s_systems; }

void SystemRegistry::Clear() {
    s_systems.clear();
    s_nameToId.clear();
    s_messageToSystems.clear();
    s_nextId = 1;
}

} // namespace DX12Engine::Scheduler
