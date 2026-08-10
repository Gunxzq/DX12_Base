#include "SpatialHashGrid.h"

#include "Logger/Logger.h" // 完整定义（动态扩展诊断日志 2026-08-09；ErrorReporter.h 仅前向声明会 C2027）
#include "Renderer/Core/CullingUtil.h" // FrustumCullAABB / FrustumCullSphere（统一剔除语义）
#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_set>

using namespace DX12Engine::Renderer;

// ========================================================================
// SpatialHashGrid 实现（2026-08-10 完整迁移自 OctreeSystem.cpp，逻辑保留）
//
// 原文件：Engine/Renderer/Core/OctreeSystem.cpp（614 行，旧系统保留作参考，稳定后移除）
// 变更：namespace Culling + Renderer:: 前缀（CulledSet/Frustum）+ 类名 SpatialHashGrid
// ========================================================================

namespace DX12Engine {
namespace Culling {

// ========================================================================
// 生命周期
// ========================================================================

void SpatialHashGrid::Initialize(const DirectX::XMFLOAT3 &worldCenter, float worldSize) {
    m_worldCenter = worldCenter;
    m_worldSize = worldSize;
    m_cellSize = 250.0f; // 格子边长（City 2726 对角 → 约 11 格/轴；原 100 → 远平面格子遍历爆炸）
    m_halfCells = (int)std::ceil(worldSize * 0.5f / m_cellSize);
    m_cells.clear();
    m_initialized = true;
    m_dirty = true;
}

void SpatialHashGrid::Shutdown() {
    m_cells.clear();
    m_initialized = false;
}

// ========================================================================
// 内部工具
// ========================================================================

int64_t SpatialHashGrid::CellKey(int cx, int cy, int cz) const {
    // 线性索引（偏移 m_halfCells 保证非负）：((cx+h)*span + cy+h) * span + cz+h
    int64_t span = m_halfCells * 2 + 1;
    return (((int64_t)(cx + m_halfCells)) * span + (cy + m_halfCells)) * span + (cz + m_halfCells);
}

void SpatialHashGrid::InsertEntry(const Renderer::CulledSet::Entry &e) {
    int minX, minY, minZ, maxX, maxY, maxZ;
    CellRangeForBounds(e.worldBounds, minX, minY, minZ, maxX, maxY, maxZ);
    for (int x = minX; x <= maxX; ++x)
        for (int y = minY; y <= maxY; ++y)
            for (int z = minZ; z <= maxZ; ++z)
                m_cells[CellKey(x, y, z)].push_back(e);
}

void SpatialHashGrid::CellRangeForBounds(const Math::BoundingAABB &b, int &minX, int &minY, int &minZ, int &maxX,
                                         int &maxY, int &maxZ) const {
    // NaN/Inf 防御：无效输入 → 空范围（防 (int)floor(NaN) UB → INT_MIN/INT_MAX 三重循环死循环）
    if (!std::isfinite(b.min.x) || !std::isfinite(b.max.x) || !std::isfinite(b.min.y) || !std::isfinite(b.max.y) ||
        !std::isfinite(b.min.z) || !std::isfinite(b.max.z)) {
        minX = minY = minZ = 0;
        maxX = maxY = maxZ = -1; // 空范围：for 循环不执行
        return;
    }
    minX = (int)std::floor((b.min.x - m_worldCenter.x) / m_cellSize);
    minY = (int)std::floor((b.min.y - m_worldCenter.y) / m_cellSize);
    minZ = (int)std::floor((b.min.z - m_worldCenter.z) / m_cellSize);
    maxX = (int)std::floor((b.max.x - m_worldCenter.x) / m_cellSize);
    maxY = (int)std::floor((b.max.y - m_worldCenter.y) / m_cellSize);
    maxZ = (int)std::floor((b.max.z - m_worldCenter.z) / m_cellSize);
    // 格子范围 clamp（2026-08-10 防御性修复）：worldBounds 有限但巨大（如 1e9，isfinite 通过）
    // 时 min/max 可达 ±4000000 → InsertEntry 三重循环天文数字死循环（Build 阶段 2 卡死根因）。
    // clamp 到合法格子范围 ±halfCells（越界实体保守 clamp 到边界格，查询同口径 clamp，
    // 实体级视锥剔除仍正确）——防死循环，语义保守不误删。
    minX = (std::max)(minX, -m_halfCells);
    maxX = (std::min)(maxX, m_halfCells);
    minY = (std::max)(minY, -m_halfCells);
    maxY = (std::min)(maxY, m_halfCells);
    minZ = (std::max)(minZ, -m_halfCells);
    maxZ = (std::min)(maxZ, m_halfCells);
}

// ========================================================================
// 构建与更新（双轨制）
// ========================================================================

void SpatialHashGrid::AddEntity(ECS::Entity entity, const Math::BoundingAABB &worldBounds, uint64_t sceneId,
                                float cullDistance, bool forceVisible) {
    if (!m_initialized)
        return;
    // 动态范围扩展（2026-08-09 修复绘制错乱根因）：实体超出当前 worldSize 覆盖（±worldSize/2）→
    // 倍增扩容 + 重哈希。worldCenter 同步移到实体分布中心（对齐 CellRangeForBounds 基准——
    // 相对 worldCenter 计算，2026-08-09 阻塞根因修复）
    const float half = m_worldSize * 0.5f;
    const float maxAbs =
        (std::max)({std::abs(worldBounds.min.x - m_worldCenter.x), std::abs(worldBounds.max.x - m_worldCenter.x),
                    std::abs(worldBounds.min.y - m_worldCenter.y), std::abs(worldBounds.max.y - m_worldCenter.y),
                    std::abs(worldBounds.min.z - m_worldCenter.z), std::abs(worldBounds.max.z - m_worldCenter.z)});
    if (maxAbs > half) {
        // 收集当前所有已入格实体（跨格去重 + forceVisible），扩容后重新入格
        std::vector<Renderer::CulledSet::Entry> all;
        std::unordered_set<uint64_t> seen;
        for (const auto &kv : m_cells)
            for (const auto &e : kv.second)
                if (seen.insert(static_cast<uint64_t>(e.entity)).second)
                    all.push_back(e);
        for (const auto &fe : m_forceVisibleEntities)
            if (seen.insert(static_cast<uint64_t>(fe.entity)).second)
                all.push_back(fe);
        // 中心化：按全部实体包围盒范围计算世界中心（含本实体——先并入 all 再算中心）
        all.push_back({entity, worldBounds, sceneId});
        seen.insert(static_cast<uint64_t>(entity));
        float cMinX = 1e30f, cMaxX = -1e30f, cMinY = 1e30f, cMaxY = -1e30f, cMinZ = 1e30f, cMaxZ = -1e30f;
        for (const auto &e : all) {
            cMinX = (std::min)(cMinX, e.worldBounds.min.x);
            cMaxX = (std::max)(cMaxX, e.worldBounds.max.x);
            cMinY = (std::min)(cMinY, e.worldBounds.min.y);
            cMaxY = (std::max)(cMaxY, e.worldBounds.max.y);
            cMinZ = (std::min)(cMinZ, e.worldBounds.min.z);
            cMaxZ = (std::max)(cMaxZ, e.worldBounds.max.z);
        }
        const float cx = (cMinX + cMaxX) * 0.5f;
        const float cy = (cMinY + cMaxY) * 0.5f;
        const float cz = (cMinZ + cMaxZ) * 0.5f;
        const float needHalf =
            (std::max)({(cMaxX - cMinX) * 0.5f, (cMaxY - cMinY) * 0.5f, (cMaxZ - cMinZ) * 0.5f}) * 1.2f;
        m_worldCenter = {cx, cy, cz};
        // 扩容封顶（防 worldSize 无限倍增 → halfCells 爆炸 → 格子遍历死循环）
        constexpr float kMaxWorldSize = 65536.0f; // 64km（City 2726 的 ~24 倍，留足余量）
        m_worldSize = (std::min)((std::max)(m_worldSize, needHalf * 2.0f), kMaxWorldSize);
        m_halfCells = (int)std::ceil(m_worldSize * 0.5f / m_cellSize);
        m_cells.clear();
        // [Diag] 动态扩展诊断（2026-08-09）：worldCenter/worldSize 变化打印（节流 60 次）
        {
            static uint32_t s_extendDiagFrame = 0;
            if ((++s_extendDiagFrame % 60) == 1) {
                Logger::Logger::GetInstance()->Info(
                    "[SpatialHashGrid][Diag] dynamic extend: center=({:.1f},{:.1f},{:.1f}) size={:.1f} halfCells={} "
                    "entities={}",
                    m_worldCenter.x, m_worldCenter.y, m_worldCenter.z, m_worldSize, m_halfCells,
                    static_cast<int>(all.size()));
            }
        }
        for (const auto &e : all)
            InsertEntry(e);
        // 重哈希已含历史实体 + 本实体（all 已并入）；补录本实体的 cullDistance / forceVisible
        m_cullDistances[entity] = cullDistance;
        if (forceVisible) {
            Renderer::CulledSet::Entry fe;
            fe.entity = entity;
            fe.worldBounds = worldBounds;
            fe.sceneId = sceneId;
            m_forceVisibleEntities.push_back(std::move(fe));
        }
        return;
    }
    m_cullDistances[entity] = cullDistance; // 粗筛层拒远用（@CullFar）
    // [Diag] AddEntity 采样（2026-08-09 错乱诊断）：节流 120 次
    {
        static uint32_t s_addDiagFrame = 0;
        if ((++s_addDiagFrame % 120) == 1) {
            Logger::Logger::GetInstance()->Info(
                "[SpatialHashGrid][Diag] AddEntity: entity={} bounds=({:.1f},{:.1f},{:.1f})~({:.1f},{:.1f},{:.1f}) "
                "worldCenter=({:.1f},{:.1f},{:.1f}) size={:.1f} halfCells={} cells={}",
                static_cast<uint64_t>(entity), worldBounds.min.x, worldBounds.min.y, worldBounds.min.z,
                worldBounds.max.x, worldBounds.max.y, worldBounds.max.z, m_worldCenter.x, m_worldCenter.y,
                m_worldCenter.z, m_worldSize, m_halfCells, static_cast<int>(m_cells.size()));
        }
    }
    if (forceVisible) {
        // 强制可见：绕过剔除系统（BlockComponent.forceVisible——开发者自定义强制内容，始终进入候选集）
        Renderer::CulledSet::Entry fe;
        fe.entity = entity;
        fe.worldBounds = worldBounds;
        fe.sceneId = sceneId;
        m_forceVisibleEntities.push_back(std::move(fe));
    }
    Renderer::CulledSet::Entry entry;
    entry.entity = entity;
    entry.worldBounds = worldBounds;
    entry.sceneId = sceneId;
    // 实体入其 worldBounds 覆盖的所有格子（保守，防误删——吸取八叉树节点 AABB 误剪教训）
    InsertEntry(entry);
}

void SpatialHashGrid::RemoveEntity(ECS::Entity entity) {
    m_cullDistances.erase(entity);
    for (auto &kv : m_cells) {
        auto &v = kv.second;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [entity](const Renderer::CulledSet::Entry &e) { return e.entity == entity; }),
                v.end());
    }
}

void SpatialHashGrid::Clear() {
    m_cells.clear();
    m_cullDistances.clear();
}

void SpatialHashGrid::Build(const std::vector<Renderer::CulledSet::Entry> &entities) {
    Clear();
    if (!m_initialized || entities.empty())
        return;
    // 双轨制（2026-08-10 文档定案，InstanceCullingSystem.md §二）：场景加载/重建 = 静态轨，
    // 内容确定 → 阶段 1 按全部实体包围盒一次算定 worldCenter/worldSize（覆盖 + 1.2 余量），
    // 阶段 2 批量 InsertEntry（全部实体在覆盖内，零扩容，O(N)）。
    float cMinX = 1e30f, cMaxX = -1e30f, cMinY = 1e30f, cMaxY = -1e30f, cMinZ = 1e30f, cMaxZ = -1e30f;
    uint32_t nonFiniteCount = 0; // [Diag] 非有限 worldBounds 统计（worldSize 推断污染源）
    float maxAbsBound = 0.0f;    // [Diag] 最大坐标绝对值（异常实体定位）
    for (const auto &e : entities) {
        // 防御性修复（2026-08-10）：跳过非有限 worldBounds——NaN/Inf 参与 min/max 会污染
        // 包围盒推断（cMinX..cMaxZ 变 NaN/Inf）→ worldCenter/worldSize 推断异常 → 入格错乱。
        if (!std::isfinite(e.worldBounds.min.x) || !std::isfinite(e.worldBounds.max.x) ||
            !std::isfinite(e.worldBounds.min.y) || !std::isfinite(e.worldBounds.max.y) ||
            !std::isfinite(e.worldBounds.min.z) || !std::isfinite(e.worldBounds.max.z)) {
            ++nonFiniteCount;
            continue;
        }
        cMinX = (std::min)(cMinX, e.worldBounds.min.x);
        cMaxX = (std::max)(cMaxX, e.worldBounds.max.x);
        cMinY = (std::min)(cMinY, e.worldBounds.min.y);
        cMaxY = (std::max)(cMaxY, e.worldBounds.max.y);
        cMinZ = (std::min)(cMinZ, e.worldBounds.min.z);
        cMaxZ = (std::max)(cMaxZ, e.worldBounds.max.z);
        maxAbsBound = (std::max)({maxAbsBound, std::abs(e.worldBounds.min.x), std::abs(e.worldBounds.max.x),
                                  std::abs(e.worldBounds.min.y), std::abs(e.worldBounds.max.y),
                                  std::abs(e.worldBounds.min.z), std::abs(e.worldBounds.max.z)});
    }
    const float cx = (cMinX + cMaxX) * 0.5f;
    const float cy = (cMinY + cMaxY) * 0.5f;
    const float cz = (cMinZ + cMaxZ) * 0.5f;
    const float needHalf = (std::max)({(cMaxX - cMinX) * 0.5f, (cMaxY - cMinY) * 0.5f, (cMaxZ - cMinZ) * 0.5f}) * 1.2f;
    m_worldCenter = {cx, cy, cz};
    m_worldSize = (std::min)((std::max)(m_worldSize, needHalf * 2.0f), 65536.0f); // 封顶：防 halfCells 爆炸
    m_halfCells = (int)std::ceil(m_worldSize * 0.5f / m_cellSize);
    // [Diag] worldSize 推断诊断（2026-08-10 阻塞排查）
    {
        static uint32_t s_buildDiagFrame = 0;
        if ((++s_buildDiagFrame % 60) == 1) {
            Logger::Logger::GetInstance()->Info(
                "[SpatialHashGrid][Diag] Build: entities={} nonFinite={} maxAbsBound={:.1f} "
                "bounds=({:.1f},{:.1f},{:.1f})~({:.1f},{:.1f},{:.1f}) center=({:.1f},{:.1f},{:.1f}) "
                "worldSize={:.1f} halfCells={} cellSize={:.1f}",
                static_cast<int>(entities.size()), nonFiniteCount, maxAbsBound, cMinX, cMinY, cMinZ, cMaxX, cMaxY,
                cMaxZ, m_worldCenter.x, m_worldCenter.y, m_worldCenter.z, m_worldSize, m_halfCells, m_cellSize);
        }
    }
    m_cells.clear();
    // [Diag] InsertEntry 进度（2026-08-10 阻塞定位）
    {
        static uint32_t s_insertDiagFrame = 0;
        const bool diag = ((++s_insertDiagFrame % 60) == 1);
        if (diag)
            Logger::Logger::GetInstance()->Info("[SpatialHashGrid][Diag] InsertEntry start: total={}",
                                                static_cast<int>(entities.size()));
        uint32_t idx = 0;
        for (const auto &e : entities) {
            InsertEntry(e);
            if (diag && ((idx++ & 0x7) == 0))
                Logger::Logger::GetInstance()->Info("[SpatialHashGrid][Diag] InsertEntry: {}/{} entity={}", idx,
                                                    static_cast<int>(entities.size()), static_cast<uint64_t>(e.entity));
        }
        if (diag)
            Logger::Logger::GetInstance()->Info("[SpatialHashGrid][Diag] InsertEntry done: total={}",
                                                static_cast<int>(entities.size()));
    }
    m_dirty = false; // 完整重建完成，清除脏标记
}

void SpatialHashGrid::SetEntityCullData(const Renderer::CulledSet::Entry &entry, float cullDistance,
                                        bool forceVisible) {
    // 双轨制（2026-08-10）：Build 批量入格只承载空间位置（Entry = entity/bounds/sceneId），
    // cullDistance（@CullFar 拒远）与 forceVisible（绕过剔除）由 Build 后补录，语义与 AddEntity 一致。
    m_cullDistances[entry.entity] = cullDistance;
    if (forceVisible)
        m_forceVisibleEntities.push_back(entry); // 完整 Entry（含 worldBounds，查询 outSet.Add 使用）
}

// ========================================================================
// 查询
// ========================================================================

void SpatialHashGrid::QueryFrustum(const Renderer::Frustum &frustum, Renderer::CulledSet &outSet,
                                   const DirectX::XMFLOAT3 &cameraPos) const {
    outSet.Clear();
    if (!m_initialized || m_cells.empty())
        return;

    // 视锥角点 AABB（保守覆盖查询格子范围）
    const auto &corners = frustum.GetCorners();
    Math::BoundingAABB faabb;
    faabb.min = corners[0];
    faabb.max = corners[0];
    for (int i = 1; i < 8; ++i) {
        faabb.min.x = (std::min)(faabb.min.x, corners[i].x);
        faabb.min.y = (std::min)(faabb.min.y, corners[i].y);
        faabb.min.z = (std::min)(faabb.min.z, corners[i].z);
        faabb.max.x = (std::max)(faabb.max.x, corners[i].x);
        faabb.max.y = (std::max)(faabb.max.y, corners[i].y);
        faabb.max.z = (std::max)(faabb.max.z, corners[i].z);
    }

    int minX, minY, minZ, maxX, maxY, maxZ;
    CellRangeForBounds(faabb, minX, minY, minZ, maxX, maxY, maxZ);

    // [Diag] 查询命中采样（2026-08-09 错乱诊断）：节流 120 次
    {
        static uint32_t s_queryDiagFrame = 0;
        if ((++s_queryDiagFrame % 120) == 1) {
            Logger::Logger::GetInstance()->Info("[SpatialHashGrid][Diag] QueryFrustumChunk: cells={} forceVisible={} "
                                                "span=({:.1f}~{:.1f},{:.1f}~{:.1f}) "
                                                "worldCenter=({:.1f},{:.1f},{:.1f}) size={:.1f} halfCells={}",
                                                static_cast<int>(m_cells.size()),
                                                static_cast<int>(m_forceVisibleEntities.size()), faabb.min.x,
                                                faabb.max.x, faabb.min.z, faabb.max.z, m_worldCenter.x, m_worldCenter.y,
                                                m_worldCenter.z, m_worldSize, m_halfCells);
        }
    }

    // GTA 查询计数器（大型引擎 dedup）：查询级 stamp，合并阶段去重
    ++m_frustumStamp;
    if (m_frustumStamp == 0) { // uint32 回绕：清空标记避免误跳过
        m_queryStamps.clear();
        m_frustumStamp = 1;
    }

    // 并行剔除（大型引擎 CPU 剔除多线程）：x 分 4 块，块内局部 set 去重，合并阶段单线程 GTA 去重
    constexpr int kChunks = 4;
    std::vector<std::vector<Renderer::CulledSet::Entry>> chunkResults(kChunks);
    std::vector<std::atomic<uint32_t>> chunkHits(kChunks);
    const int span = maxX - minX + 1;

    // 锥形遍历预筛（根治 AABB 立方体覆盖——跳过视锥锥形体积外的角落格子）
    const DirectX::XMFLOAT3 nearC = frustum.GetNearCenter();
    const DirectX::XMFLOAT3 farC = frustum.GetFarCenter();
    DirectX::XMFLOAT3 axis = {farC.x - nearC.x, farC.y - nearC.y, farC.z - nearC.z};
    const float axisLen = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (axisLen > 1e-6f) {
        axis.x /= axisLen;
        axis.y /= axisLen;
        axis.z /= axisLen;
    }
    const float h = m_cellSize * 0.5f;
    const float nearDepth =
        (nearC.x - cameraPos.x) * axis.x + (nearC.y - cameraPos.y) * axis.y + (nearC.z - cameraPos.z) * axis.z;
    const float farDepth =
        (farC.x - cameraPos.x) * axis.x + (farC.y - cameraPos.y) * axis.y + (farC.z - cameraPos.z) * axis.z;

    // 块处理（串行执行分块——并行改由 Editor 注册 N 个并行剔除 system，依赖图并行）
    auto blockProcess = [&](int c) {
        const int xStart = minX + span * c / kChunks;
        const int xEnd = minX + span * (c + 1) / kChunks - 1;
        std::unordered_set<uint64_t> localSeen; // 块内实体去重（线程安全，不碰共享 m_queryStamps）
        auto &local = chunkResults[c];
        uint32_t hits = 0;
        for (int x = xStart; x <= xEnd; ++x)
            for (int y = minY; y <= maxY; ++y)
                for (int z = minZ; z <= maxZ; ++z) {
                    // 锥形预筛：格子中心深度/径向在视锥锥形体积外 → 跳过（省立方体角落遍历）
                    DirectX::XMFLOAT3 cc = {m_worldCenter.x + x * m_cellSize + h, m_worldCenter.y + y * m_cellSize + h,
                                            m_worldCenter.z + z * m_cellSize + h};
                    const float depth =
                        (cc.x - cameraPos.x) * axis.x + (cc.y - cameraPos.y) * axis.y + (cc.z - cameraPos.z) * axis.z;
                    if (depth + h < nearDepth || depth - h > farDepth)
                        continue; // 深度范围在视锥外
                    float secW = 0.0f, secH = 0.0f;
                    frustum.GetSectionSize(depth, secW, secH);
                    if (secW > 0.0f && secH > 0.0f) {
                        // 仅视锥参数有效时才启用锥形预筛（GetSectionSize 依赖 m_params.isValid，
                        // 编辑器视锥可能未设 params → 返回 0 → 若仍测试会全跳过）
                        const float px = cc.x - cameraPos.x - axis.x * depth;
                        const float py = cc.y - cameraPos.y - axis.y * depth;
                        const float pz = cc.z - cameraPos.z - axis.z * depth;
                        const float rad = std::sqrt(px * px + py * py + pz * pz);
                        const float rMax = (std::max)(secW, secH) * 0.5f + h; // 保守：截面外接球 + h
                        if (rad > rMax)
                            continue; // 锥形外（保守球测试，不误删）
                    }
                    // 格子级视锥剪枝：跳过视锥外格子（防远平面格子遍历爆炸）
                    Math::BoundingAABB cellBounds;
                    cellBounds.min = {m_worldCenter.x + x * m_cellSize, m_worldCenter.y + y * m_cellSize,
                                      m_worldCenter.z + z * m_cellSize};
                    cellBounds.max = {cellBounds.min.x + m_cellSize, cellBounds.min.y + m_cellSize,
                                      cellBounds.min.z + m_cellSize};
                    if (!FrustumCullAABB(cellBounds, frustum))
                        continue;
                    auto it = m_cells.find(CellKey(x, y, z));
                    if (it == m_cells.end())
                        continue;
                    ++hits; // 命中格子（有实体的可见格，性能观察：区块维度）
                    for (const auto &entry : it->second) {
                        uint64_t key = static_cast<uint64_t>(entry.entity);
                        if (!localSeen.insert(key).second)
                            continue; // 块内已处理（实体入多格）
                        // 粗筛层 cullDistance 拒远（@CullFar，对齐大型引擎先距离后视锥）
                        auto cdIt = m_cullDistances.find(entry.entity);
                        if (cdIt != m_cullDistances.end() && cdIt->second > 0.0f) {
                            const auto &bc = entry.worldBounds.GetCenter();
                            const float dx = bc.x - cameraPos.x;
                            const float dy = bc.y - cameraPos.y;
                            const float dz = bc.z - cameraPos.z;
                            if (dx * dx + dy * dy + dz * dz > cdIt->second * cdIt->second)
                                continue; // 离得足够远根本看不清 → 强制剔除
                        }
                        // 实体级视锥粗筛（统一剔除语义 2026-08-09：与 GPU CS 一致用外接球测试，
                        // FrustumCullSphere 内部 radius×1.15 保守不误剔——AABB 判外但外接球可判内会误剔地面）
                        {
                            const Math::BoundingSphere s{entry.worldBounds.GetCenter(), entry.worldBounds.GetRadius()};
                            if (FrustumCullSphere(s, frustum))
                                local.push_back(entry);
                        }
                    }
                }
        chunkHits[c].store(hits);
    };
    for (int c = 0; c < kChunks; ++c)
        blockProcess(c);

    // 合并（单线程）：跨块重复实体用 GTA 计数器去重
    m_lastCellsHit = 0;
    for (int c = 0; c < kChunks; ++c) {
        m_lastCellsHit += chunkHits[c].load();
        for (const auto &entry : chunkResults[c]) {
            uint32_t &stamp = m_queryStamps[entry.entity];
            if (stamp == m_frustumStamp)
                continue; // 跨块已处理（实体入多格）
            stamp = m_frustumStamp;
            outSet.Add(entry.entity, entry.worldBounds, entry.sceneId);
        }
    }
    // 强制可见实体（BlockComponent.forceVisible——绕过剔除系统，始终进入候选集）
    for (const auto &fe : m_forceVisibleEntities) {
        uint32_t &stamp = m_queryStamps[fe.entity];
        if (stamp == m_frustumStamp)
            continue;
        stamp = m_frustumStamp;
        outSet.Add(fe.entity, fe.worldBounds, fe.sceneId);
    }
}

void SpatialHashGrid::QueryFrustumChunk(const Renderer::Frustum &frustum, Renderer::CulledSet &outSet,
                                        const DirectX::XMFLOAT3 &cameraPos, uint32_t chunkIndex,
                                        uint32_t chunkCount) const {
    outSet.Clear();
    if (!m_initialized || m_cells.empty() || chunkCount == 0 || chunkIndex >= chunkCount)
        return;

    // 视锥角点 AABB（保守覆盖查询格子范围）
    const auto &corners = frustum.GetCorners();
    Math::BoundingAABB faabb;
    faabb.min = corners[0];
    faabb.max = corners[0];
    for (int i = 1; i < 8; ++i) {
        faabb.min.x = (std::min)(faabb.min.x, corners[i].x);
        faabb.min.y = (std::min)(faabb.min.y, corners[i].y);
        faabb.min.z = (std::min)(faabb.min.z, corners[i].z);
        faabb.max.x = (std::max)(faabb.max.x, corners[i].x);
        faabb.max.y = (std::max)(faabb.max.y, corners[i].y);
        faabb.max.z = (std::max)(faabb.max.z, corners[i].z);
    }
    int minX, minY, minZ, maxX, maxY, maxZ;
    CellRangeForBounds(faabb, minX, minY, minZ, maxX, maxY, maxZ);

    // 锥形遍历预筛参数（视锥轴 + 深度/径向保守测试）
    const DirectX::XMFLOAT3 nearC = frustum.GetNearCenter();
    const DirectX::XMFLOAT3 farC = frustum.GetFarCenter();
    DirectX::XMFLOAT3 axis = {farC.x - nearC.x, farC.y - nearC.y, farC.z - nearC.z};
    const float axisLen = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (axisLen > 1e-6f) {
        axis.x /= axisLen;
        axis.y /= axisLen;
        axis.z /= axisLen;
    }
    const float h = m_cellSize * 0.5f;
    const float nearDepth =
        (nearC.x - cameraPos.x) * axis.x + (nearC.y - cameraPos.y) * axis.y + (nearC.z - cameraPos.z) * axis.z;
    const float farDepth =
        (farC.x - cameraPos.x) * axis.x + (farC.y - cameraPos.y) * axis.y + (farC.z - cameraPos.z) * axis.z;

    // 本块格子范围（x 分块）
    const int span = maxX - minX + 1;
    const int xStart = minX + span * static_cast<int>(chunkIndex) / static_cast<int>(chunkCount);
    const int xEnd = minX + span * static_cast<int>(chunkIndex + 1) / static_cast<int>(chunkCount) - 1;

    std::unordered_set<uint64_t> localSeen; // 块内实体去重（并行 system 各自独立，无共享写）
    for (int x = xStart; x <= xEnd; ++x)
        for (int y = minY; y <= maxY; ++y)
            for (int z = minZ; z <= maxZ; ++z) {
                // 锥形预筛：格子中心深度/径向在视锥锥形体积外 → 跳过
                DirectX::XMFLOAT3 cc = {m_worldCenter.x + x * m_cellSize + h, m_worldCenter.y + y * m_cellSize + h,
                                        m_worldCenter.z + z * m_cellSize + h};
                const float depth =
                    (cc.x - cameraPos.x) * axis.x + (cc.y - cameraPos.y) * axis.y + (cc.z - cameraPos.z) * axis.z;
                if (depth + h < nearDepth || depth - h > farDepth)
                    continue;
                float secW = 0.0f, secH = 0.0f;
                frustum.GetSectionSize(depth, secW, secH);
                if (secW > 0.0f && secH > 0.0f) {
                    const float px = cc.x - cameraPos.x - axis.x * depth;
                    const float py = cc.y - cameraPos.y - axis.y * depth;
                    const float pz = cc.z - cameraPos.z - axis.z * depth;
                    const float rad = std::sqrt(px * px + py * py + pz * pz);
                    const float rMax = (std::max)(secW, secH) * 0.5f + h;
                    if (rad > rMax)
                        continue;
                }
                // 格子级视锥剪枝
                Math::BoundingAABB cellBounds;
                cellBounds.min = {m_worldCenter.x + x * m_cellSize, m_worldCenter.y + y * m_cellSize,
                                  m_worldCenter.z + z * m_cellSize};
                cellBounds.max = {cellBounds.min.x + m_cellSize, cellBounds.min.y + m_cellSize,
                                  cellBounds.min.z + m_cellSize};
                if (!FrustumCullAABB(cellBounds, frustum))
                    continue;
                auto it = m_cells.find(CellKey(x, y, z));
                if (it == m_cells.end())
                    continue;
                for (const auto &entry : it->second) {
                    uint64_t key = static_cast<uint64_t>(entry.entity);
                    if (!localSeen.insert(key).second)
                        continue; // 块内已处理（实体入多格）
                    // 粗筛层 cullDistance 拒远（@CullFar）
                    auto cdIt = m_cullDistances.find(entry.entity);
                    if (cdIt != m_cullDistances.end() && cdIt->second > 0.0f) {
                        const auto &bc = entry.worldBounds.GetCenter();
                        const float dx = bc.x - cameraPos.x;
                        const float dy = bc.y - cameraPos.y;
                        const float dz = bc.z - cameraPos.z;
                        if (dx * dx + dy * dy + dz * dz > cdIt->second * cdIt->second)
                            continue; // 离得足够远根本看不清 → 强制剔除
                    }
                    // 实体级视锥粗筛（统一剔除语义 2026-08-09：与 GPU CS 一致用外接球测试）
                    {
                        const Math::BoundingSphere s{entry.worldBounds.GetCenter(), entry.worldBounds.GetRadius()};
                        if (FrustumCullSphere(s, frustum))
                            outSet.Add(entry.entity, entry.worldBounds, entry.sceneId);
                    }
                }
            }
    // 强制可见实体（BlockComponent.forceVisible——绕过剔除系统，仅 chunk 0 附加避免重复，合并去重兜底）
    if (chunkIndex == 0) {
        for (const auto &fe : m_forceVisibleEntities)
            outSet.Add(fe.entity, fe.worldBounds, fe.sceneId);
    }
}

void SpatialHashGrid::QueryBounds(const Math::BoundingAABB &bounds, std::vector<ECS::Entity> &outEntities) const {
    outEntities.clear();
    if (!m_initialized || m_cells.empty())
        return;

    int minX, minY, minZ, maxX, maxY, maxZ;
    CellRangeForBounds(bounds, minX, minY, minZ, maxX, maxY, maxZ);

    // GTA 查询计数器（独立于视锥查询：避免同帧 QueryFrustum 已处理的实体被本查询误跳过）
    ++m_boundsStamp;
    if (m_boundsStamp == 0) {
        m_boundsStamps.clear();
        m_boundsStamp = 1;
    }
    for (int x = minX; x <= maxX; ++x)
        for (int y = minY; y <= maxY; ++y)
            for (int z = minZ; z <= maxZ; ++z) {
                auto it = m_cells.find(CellKey(x, y, z));
                if (it == m_cells.end())
                    continue;
                for (const auto &entry : it->second) {
                    uint32_t &stamp = m_boundsStamps[entry.entity];
                    if (stamp == m_boundsStamp)
                        continue; // 本查询已处理（实体入多格）
                    stamp = m_boundsStamp;
                    outEntities.push_back(entry.entity);
                }
            }
}

void SpatialHashGrid::QueryRay(const FRay &ray, std::vector<ECS::Entity> &outEntities) const {
    // 简化：射线 AABB（原点 ± 方向×远距）覆盖格子（空间粗筛，不精确测试——与原八叉树语义一致）
    outEntities.clear();
    if (!m_initialized || m_cells.empty())
        return;
    const float rayLen = 10000.0f;
    Math::BoundingAABB rayBounds;
    float ex = ray.Origin.X + ray.Direction.X * rayLen;
    float ey = ray.Origin.Y + ray.Direction.Y * rayLen;
    float ez = ray.Origin.Z + ray.Direction.Z * rayLen;
    rayBounds.min = {(std::min)(ray.Origin.X, ex), (std::min)(ray.Origin.Y, ey), (std::min)(ray.Origin.Z, ez)};
    rayBounds.max = {(std::max)(ray.Origin.X, ex), (std::max)(ray.Origin.Y, ey), (std::max)(ray.Origin.Z, ez)};
    QueryBounds(rayBounds, outEntities);
}

} // namespace Culling
} // namespace DX12Engine
