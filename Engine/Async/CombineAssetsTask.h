// Engine/Async/CombineAssetsTask.h
#pragma once

#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "Resource/Struct/GeometryHandle.h"
#include "Resource/Struct/MaterialHandle.h"
#include "Resource/Struct/TextureHandle.h"
#include "Scheduler/Task.h"
#include "TerrainLoadTask.h"
#include <DirectXMath.h>
#include <memory>

namespace DX12Engine::Async {

/**
 * @brief 组合任务工厂 — 等待所有资源就绪后，在主线程创建 ECS 实体
 *
 * 使用方式：
 * 1. 创建 TerrainLoadTask（Worker 线程，生成 CPU 网格数据）
 * 2. 创建 TerrainGPUCreateTask（Render 线程，创建 VB/IB）
 * 3. 材质/纹理加载完成（各自独立的加载任务）
 * 4. 创建 CombineAssetsTask（Main 线程，依赖以上所有任务）
 *
 * 依赖关系：
 *   terrainLoad ──→ terrainGPUCreate ──┐
 *   materialLoad ──────────────────────┼──→ combineTask (Main 线程)
 *   textureLoad ───────────────────────┘
 */
class CombineAssetsTaskFactory {
public:
    struct CombineInput {
        Resource::GeometryHandle geometryHandle;
        Resource::MaterialHandle materialHandle;
        Resource::TextureHandle textureHandle;
        bool hasGeometry = false;
        bool hasMaterial = false;
        bool hasTexture = false;
    };

    using CombineInputPtr = std::shared_ptr<CombineInput>;

    static CombineInputPtr CreateInput() { return std::make_shared<CombineInput>(); }

    /**
     * @brief 创建组合任务 — 在主线程执行，依赖 GPU 创建和材质/纹理加载完成后创建 ECS 实体
     *
     * @param requestId    请求 ID
     * @param registry     ECS 注册表引用
     * @param input        共享输入数据（各子任务完成后会设置对应 handle）
     * @param outEntity    输出实体 ID
     * @param terrainMeshData 地形网格数据引用（用于计算包围盒）
     * @return Scheduler::Task
     */
    static Scheduler::Task Create(uint32_t requestId, ECS::Registry *registry, CombineInputPtr input,
                                  ECS::Entity *outEntity, const TerrainLoadDataPtr &terrainData) {
        Scheduler::Task task;
        task.name = "CombineAssetsTask";
        task.phase = Scheduler::TaskPhase::Update;
        task.thread = Scheduler::ThreadType::Main;
        task.priority = static_cast<uint32_t>(Scheduler::TaskPriority::Normal);

        task.execute = [requestId, registry, input, outEntity, terrainData]() {
            // 验证所有句柄有效
            if (!input->hasGeometry || !input->hasMaterial || !input->hasTexture) {
                // 资源未就绪，不创建实体
                return;
            }

            // 创建实体
            auto entity = registry->CreateEntity();
            *outEntity = entity;

            // Transform 组件
            XMFLOAT3 position(0.0f, -10.0f, 0.0f);
            XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
            XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
            registry->AddComponent<ECS::TransformComponent>(entity, position, rotation, scale);

            // 计算包围盒
            Math::BoundingAABB bounds;
            if (terrainData) {
                bounds.min = XMFLOAT3(-terrainData->width * 0.5f, 0.0f, -terrainData->depth * 0.5f);
                bounds.max =
                    XMFLOAT3(terrainData->width * 0.5f, terrainData->maxHeight, terrainData->depth * 0.5f);
            }

            // Mesh 组件
            ECS::MeshComponent meshComp;
            meshComp.lodMeshHandle = {}; // 将在注册 LOD 后设置
            meshComp.localBounds = bounds;
            meshComp.materialHandle = input->materialHandle;
            meshComp.textureHandle = input->textureHandle;

            registry->AddComponent<ECS::MeshComponent>(entity, std::move(meshComp));
        };

        return task;
    }
};

} // namespace DX12Engine::Async
