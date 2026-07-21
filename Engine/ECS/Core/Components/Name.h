#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// ========================================================================
// NameComponent — 实体名称与持久化 ID
//
// 设计说明：
//   persistentId 是单调递增的 uint64_t，实体销毁后永不复用，
//   与 ECS 的 entt::entity（generational index，可被复用）不同。
//   这使得 persistentId 适合作为编辑器 UI 中的稳定标识符，
//   也适合作为序列化/反序列化时的持久化引用。
//
// 大型引擎参考：
//   - Unreal Engine: FUniqueObjectGuid (128-bit GUID)
//   - Unity: GetInstanceID() (session-local int, 不跨会话)
//   - Blender: session_uuid (128-bit UUID)
//   我们使用 64-bit 单调递增计数器，简单且够用。
// ========================================================================

namespace DX12Engine::ECS {

struct NameComponent {
    /// 持久化 ID（单调递增，永不复用）
    /// 0 为无效值，用于检测未初始化的组件
    uint64_t persistentId = 0;

    /// 实体显示名称
    std::string name;
};

// ========================================================================
// 全局 ID 分配器
// ========================================================================

/// 获取下一个可用的持久化 ID（线程安全）
inline uint64_t NextPersistentId() {
    static std::atomic<uint64_t> s_nextId{1};
    return s_nextId.fetch_add(1, std::memory_order_relaxed);
}

} // namespace DX12Engine::ECS