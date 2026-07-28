#pragma once

#include <cstdint>
#include <string>

namespace DX12Engine::ECS {

// 关系类型
enum class RelationshipKind : uint8_t {
    Parent = 0,     ///< 变换父级：子实体跟随父实体的 Transform
    Socket = 1,     ///< 骨骼挂点：子实体挂在父实体的某块骨骼上
    Group  = 2,     ///< 逻辑分组：同属一个逻辑组（如同一波敌人）
    Follow = 3      ///< 跟随目标：声明该实体跟随另一个实体（如相机跟随角色）
};

// 关系组件——表达实体间的引用关系
// 引擎 CORE 只存储此组件，不解释、不维护树结构、不做级联删除
struct RelationshipComponent {
    uint64_t targetId = 0;         ///< 目标实体的 persistentId（0 = 无关系）
    RelationshipKind kind;         ///< 关系类型
};

// 骨骼挂点组件——仅 RelationshipKind::Socket 时使用
// 分离为独立组件避免变长 string 浪费在其他关系类型的内存中
struct SocketAttachmentComponent {
    std::string socketName;        ///< 骨骼挂点名称（如 "hand_r", "head", "neck"）
};

} // namespace DX12Engine::ECS
