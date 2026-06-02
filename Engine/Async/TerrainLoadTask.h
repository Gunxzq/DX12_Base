#pragma once

#include "AsyncLoadTask.h"
#include "Event/EventRegistry.h"
#include "Logger/Logger.h"
#include "Math/BoundingVolume.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/AssetLoader/AssetLoader.h"
#include "Resource/AssetLoader/Loader/TerrainLoader.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Manager/MaterialManager.h"
#include "Resource/Texture/TextureManager.h"
#include <DirectXMath.h>
#include <memory>

namespace DX12Engine::Async {

// 使用 EventRegistry 中定义的枚举值作为事件哈希
constexpr uint32_t TERRAIN_LOADED_EVENT_HASH   = static_cast<uint32_t>(Event::EventType::TerrainLoadedEvent);
constexpr uint32_t TERRAIN_LOAD_FAILED_EVENT_HASH = static_cast<uint32_t>(Event::EventType::TerrainLoadFailedEvent);

// 地形加载数据（CPU 端），使用 shared_ptr 在线程间共享
struct TerrainLoadData {
    std::vector<GeometryGenerator::Vertex> vertices;
    std::vector<uint32_t> indices;
    float width;
    float depth;
    float maxHeight;
    Math::BoundingAABB bounds;
};

using TerrainLoadDataPtr = std::shared_ptr<TerrainLoadData>;

/**
 * @brief 地形加载任务工厂 — 创建 Worker 线程执行的地形网格生成任务
 *
 * 该任务在 Worker 线程执行：
 * 1. 从 PNG 加载高度图
 * 2. 生成顶点/索引数据
 * 3. 将结果写入 shared_ptr<TerrainLoadData>，供后续 GPU 创建任务读取
 */
class TerrainLoadTaskFactory {
public:
    static Scheduler::Task Create(uint32_t requestId, const std::wstring &heightmapPath, float width, float depth,
                                  float maxHeight, uint32_t segments, TerrainLoadDataPtr &outData) {
        Scheduler::Task task;
        task.name = "TerrainLoadTask";
        task.phase = Scheduler::TaskPhase::Update;
        task.thread = Scheduler::ThreadType::Worker;
        task.priority = static_cast<uint32_t>(Scheduler::TaskPriority::Background);

        auto data = std::make_shared<TerrainLoadData>();
        outData = data;

        task.execute = [requestId, heightmapPath, width, depth, maxHeight, segments, data]() {
            auto *logger = Logger::Logger::GetInstance();
            logger->Info("[TerrainLoadTask] Worker thread started (request={})", requestId);

            // 1. 加载高度图并生成网格数据
            Resource::TerrainMeshData meshData;
            if (!Resource::AssetLoader::GetInstance().LoadTerrainFromFile(heightmapPath, width, depth, maxHeight, segments,
                                                                          meshData)) {
                logger->Error("[TerrainLoadTask] Failed to load heightmap from file (request={})", requestId);
                uint64_t payload = static_cast<uint64_t>(requestId) << 32;
                Event::MessageDispatcher::GetInstance()->PostEvent(TERRAIN_LOAD_FAILED_EVENT_HASH, 0, payload,
                                                                   Event::EventPriority::P4_Background);
                return;
            }

            logger->Info("[TerrainLoadTask] Heightmap loaded: {} vertices, {} indices (request={})",
                         meshData.vertices.size(), meshData.indices.size(), requestId);

            // 拷贝数据到 shared_ptr
            data->vertices = std::move(meshData.vertices);
            data->indices = std::move(meshData.indices);
            data->width = meshData.width;
            data->depth = meshData.depth;
            data->maxHeight = meshData.maxHeight;

            // 计算包围盒
            data->bounds.min = XMFLOAT3(-meshData.width * 0.5f, 0.0f, -meshData.depth * 0.5f);
            data->bounds.max = XMFLOAT3(meshData.width * 0.5f, meshData.maxHeight, meshData.depth * 0.5f);

            // 2. 发送完成事件（payload 编码 requestId）
            uint64_t payload = static_cast<uint64_t>(requestId) << 32;
            bool posted = Event::MessageDispatcher::GetInstance()->PostEvent(
                TERRAIN_LOADED_EVENT_HASH, 0, payload, Event::EventPriority::P4_Background);
            logger->Info("[TerrainLoadTask] PostEvent TerrainLoaded: posted={} (request={})",
                         posted, requestId);
        };

        return task;
    }
};

} // namespace DX12Engine::Async
