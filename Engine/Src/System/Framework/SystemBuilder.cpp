#include "System/Framework/SystemBuilder.h"

namespace DX12Engine::Scheduler {

// ========================================================================
// SystemBuilder 实现
// ========================================================================

SystemBuilder::SystemBuilder(const std::string &name, TaskPhase phase, ThreadType thread) : m_info{} {
    m_info.name = name;
    m_info.phase = phase;
    m_info.threadType = thread;
    m_info.priority = TaskPriority::Normal;
}

SystemBuilder &SystemBuilder::Func(SystemFunc func) {
    m_info.func = std::move(func);
    return *this;
}

SystemBuilder &SystemBuilder::DependsOn(const std::string &systemName) {
    // 延迟查找：在 Build 时解析
    auto *other = SystemRegistry::GetSystemByName(systemName);
    if (other) {
        m_info.dependencies.push_back(other->id);
    }
    return *this;
}

SystemBuilder &SystemBuilder::Priority(TaskPriority priority) {
    m_info.priority = priority;
    return *this;
}

SystemId SystemBuilder::Build() { return SystemRegistry::Register(m_info); }

} // namespace DX12Engine::Scheduler
