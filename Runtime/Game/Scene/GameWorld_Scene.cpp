#include "Boot/GameContext.h"
#include "Common/ThrowHelper.h"
#include "Common/d3dUtil.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "GameWorld.h"
#include "Math/BoundingVolume.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Pipeline/BillboardRenderer.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/Pipeline/SkyRenderer.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/Asset/LODMesh.h"
#include "Resource/AssetLoader/AssetLoader.h"
#include "Resource/AssetLoader/Loader/DDSLoader.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Geometry/GridGeometry.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Renderer/Material/MaterialManager.h"
#include "Resource/Texture/TextureManager.h"
#include <DirectXMath.h>
#include <algorithm>
#include <fstream>

using namespace DirectX;
using namespace DX12Engine;
using namespace DX12Engine::Boot;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Math;

// ========================================================================
// GameWorld — 场景物体创建
// ========================================================================

void GameWorld::CreateGroundPlane() {
    if (!m_registry || !m_renderer || !m_context)
        return;

    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateGrid(30.0f, 30.0f, 30, 30);

    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

    size_t vbSize = meshData.Vertices.size() * sizeof(GeometryGenerator::Vertex);
    auto vbHandle =
        gpuMgr.CreateBuffer(device, vbSize, L"Ground_VB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);
    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, meshData.Vertices.data(), vbSize);
        vbResource->Unmap(0, nullptr);
    }

    size_t ibSize = meshData.Indices32.size() * sizeof(uint32_t);
    auto ibHandle =
        gpuMgr.CreateBuffer(device, ibSize, L"Ground_IB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);
    if (ibResource) {
        void *ibMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &ibMapped);
        memcpy(ibMapped, meshData.Indices32.data(), ibSize);
        ibResource->Unmap(0, nullptr);
    }

    TriangleMesh planeMesh;
    planeMesh.vertexBufferHandle = vbHandle;
    planeMesh.indexBufferHandle = ibHandle;
    planeMesh.vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
    planeMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    planeMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    planeMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    planeMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    planeMesh.isGpuReady = true;

    BoundingAABB bounds;
    bounds.min = XMFLOAT3(-5.0f, 20.0f, -5.0f);
    bounds.max = XMFLOAT3(5.0f, 0.0f, 5.0f);

    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterGeometry<TriangleMesh>(planeMesh);
    if (!geoHandle.IsValid()) {
        OutputDebugStringW(L"[ERROR] RegisterGeometry for ground plane failed!\n");
        return;
    }

    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle};
    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    m_groundPlaneEntity = m_registry->CreateEntity();

    XMFLOAT3 position(0.0f, 30.0f, 0.0f);
    XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
    m_registry->AddComponent<TransformComponent>(m_groundPlaneEntity, position, rotation, scale);

    MeshComponent meshComp;
    meshComp.lodMeshHandle = lodHandle;
    meshComp.localBounds = bounds;
    meshComp.materialHandle = m_brickMaterialHandle;

    meshComp.receivesShadow = true;
    m_registry->AddComponent<MeshComponent>(m_groundPlaneEntity, std::move(meshComp));
    m_registry->AddComponent<OpaqueTag>(m_groundPlaneEntity);

    // m_registry->AddComponent<StaticComponent>(m_groundPlaneEntity);

    OutputDebugStringW(L"[GameWorld] Ground plane created at Y=60.0 (10x10)\n");
}

void GameWorld::CreateTestCube() {
    if (!m_registry || !m_renderer || !m_context) {
        return;
    }

    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 0);

    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

    size_t vbSize = meshData.Vertices.size() * sizeof(GeometryGenerator::Vertex);
    auto vbHandle =
        gpuMgr.CreateBuffer(device, vbSize, L"Cube_VB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);

    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, meshData.Vertices.data(), vbSize);
        vbResource->Unmap(0, nullptr);
    }

    size_t ibSize = meshData.Indices32.size() * sizeof(uint32_t);
    auto ibHandle =
        gpuMgr.CreateBuffer(device, ibSize, L"Cube_IB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);

    if (ibResource) {
        void *ibMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &ibMapped);
        memcpy(ibMapped, meshData.Indices32.data(), ibSize);
        ibResource->Unmap(0, nullptr);
    }

    TriangleMesh triangleMesh;
    triangleMesh.vertexBufferHandle = vbHandle;
    triangleMesh.indexBufferHandle = ibHandle;
    triangleMesh.vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
    triangleMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    triangleMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    triangleMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    triangleMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    triangleMesh.isGpuReady = true;

    BoundingAABB bounds;
    bounds.min = XMFLOAT3(-0.5f, -0.5f, -0.5f);
    bounds.max = XMFLOAT3(0.5f, 0.5f, 0.5f);

    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterGeometry<TriangleMesh>(triangleMesh);

    if (!geoHandle.IsValid()) {
        OutputDebugStringW(L"[ERROR] RegisterGeometry failed!\n");
        return;
    }

    constexpr float PLANE_Y = 30.0f;

    struct CubePlacement {
        XMFLOAT3 position;
        XMFLOAT3 rotation;
        XMFLOAT3 scale;
    };

    const std::vector<CubePlacement> cubePlacements = {
        {{0.0f, PLANE_Y + 0.5f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{3.0f, PLANE_Y + 1.0f, 2.0f}, {0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}},
        {{-3.0f, PLANE_Y + 0.75f, 1.0f}, {0.0f, 0.0f, 0.0f}, {1.5f, 1.5f, 1.5f}},
        {{0.0f, PLANE_Y + 0.5f, -3.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{3.0f, PLANE_Y + 1.5f, -1.0f}, {0.0f, 0.0f, 0.0f}, {3.0f, 3.0f, 3.0f}},
        {{-3.0f, PLANE_Y + 1.0f, -2.0f}, {0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}},
    };

    m_cubeEntities.clear();
    m_cubeEntities.reserve(cubePlacements.size());

    for (size_t i = 0; i < cubePlacements.size(); ++i) {
        auto entity = m_registry->CreateEntity();

        const auto &placement = cubePlacements[i];
        m_registry->AddComponent<TransformComponent>(entity, placement.position, placement.rotation, placement.scale);

        LODMesh lodMesh;
        lodMesh.lodChain = {geoHandle};

        LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

        MeshComponent meshComp;
        meshComp.lodMeshHandle = lodHandle;
        meshComp.localBounds = bounds;
        meshComp.materialHandle = m_cubeMaterialHandle;
    
        m_registry->AddComponent<MeshComponent>(entity, std::move(meshComp));
        m_registry->AddComponent<OpaqueTag>(entity);

        if (i == 1) {
            m_registry->AddComponent<PickingComponent>(entity);
        }

        // m_registry->AddComponent<StaticComponent>(entity);

        m_cubeEntities.push_back(entity);
    }

    m_cubeEntity = m_cubeEntities.empty() ? INVALID_ENTITY : m_cubeEntities[0];

    // 反射探针测试立方体
    {
        m_reflectionCubeEntity = m_registry->CreateEntity();
        m_registry->AddComponent<TransformComponent>(m_reflectionCubeEntity, XMFLOAT3(10.0f, 32.0f, 3.0f),
                                                     XMFLOAT3(0.0f, 0.0f, 0.0f), XMFLOAT3(1.0f, 1.0f, 1.0f));

        LODMesh lodMesh;
        lodMesh.lodChain = {geoHandle};
        LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

        MeshComponent meshComp;
        meshComp.lodMeshHandle = lodHandle;
        meshComp.localBounds = bounds;
        meshComp.materialHandle = m_reflectionTestMaterialHandle;
    
        m_registry->AddComponent<MeshComponent>(m_reflectionCubeEntity, std::move(meshComp));
        m_registry->AddComponent<OpaqueTag>(m_reflectionCubeEntity);

        m_registry->AddComponent<ReflectionConsumerComponent>(
            m_reflectionCubeEntity, ReflectionConsumerComponent{.probeIndex = 0, .useDynamicFallback = false});
    }
}

void GameWorld::CreateStressTestScene() {
    if (!m_registry || !m_renderer || !m_context) {
        return;
    }

    auto &geoMgr = m_context->GeometryResourceManager;

    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 0);

    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

    size_t vbSize = meshData.Vertices.size() * sizeof(GeometryGenerator::Vertex);
    auto vbHandle =
        gpuMgr.CreateBuffer(device, vbSize, L"Stress_VB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);
    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, meshData.Vertices.data(), vbSize);
        vbResource->Unmap(0, nullptr);
    }

    size_t ibSize = meshData.Indices32.size() * sizeof(uint32_t);
    auto ibHandle =
        gpuMgr.CreateBuffer(device, ibSize, L"Stress_IB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);
    if (ibResource) {
        void *ibMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &ibMapped);
        memcpy(ibMapped, meshData.Indices32.data(), ibSize);
        ibResource->Unmap(0, nullptr);
    }

    TriangleMesh triangleMesh;
    triangleMesh.vertexBufferHandle = vbHandle;
    triangleMesh.indexBufferHandle = ibHandle;
    triangleMesh.vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
    triangleMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    triangleMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    triangleMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    triangleMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    triangleMesh.isGpuReady = true;

    BoundingAABB bounds;
    bounds.min = XMFLOAT3(-0.5f, -0.5f, -0.5f);
    bounds.max = XMFLOAT3(0.5f, 0.5f, 0.5f);

    GeometryHandle geoHandle = geoMgr->RegisterGeometry<TriangleMesh>(triangleMesh);
    if (!geoHandle.IsValid()) {
        OutputDebugStringW(L"[StressTest] Failed to register geometry\n");
        return;
    }

    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle};
    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    m_stressEntities.clear();

    constexpr int GRID_HALF = 30;
    constexpr float SPACING = 2.0f;
    constexpr float PLANE_Y = 30.0f;

    m_stressEntities.clear();
    m_stressEntities.reserve((GRID_HALF * 2) * (GRID_HALF * 2));

    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);

    for (int x = -GRID_HALF; x < GRID_HALF; ++x) {
        for (int z = -GRID_HALF; z < GRID_HALF; ++z) {
            auto entity = m_registry->CreateEntity();

            XMFLOAT3 position(x * SPACING, PLANE_Y + 0.5f, z * SPACING);
            m_registry->AddComponent<TransformComponent>(entity, position, XMFLOAT3(), scale);

            MeshComponent meshComp;
            meshComp.lodMeshHandle = lodHandle;
            meshComp.localBounds = bounds;
            meshComp.materialHandle = m_cubeMaterialHandle;
        
            meshComp.receivesShadow = true;
            m_registry->AddComponent<MeshComponent>(entity, std::move(meshComp));
            m_registry->AddComponent<OpaqueTag>(entity);

            m_stressEntities.push_back(entity);
        }
    }

    m_context->Logging->Info("[GameWorld] Stress test scene: {} cubes created", m_stressEntities.size());
}

void GameWorld::CreateTestCylinder() {
    if (!m_registry || !m_renderer || !m_context)
        return;

    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateCylinder(0.5f, 0.5f, 3.0f, 20, 8);

    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

    size_t vbSize = meshData.Vertices.size() * sizeof(GeometryGenerator::Vertex);
    auto vbHandle =
        gpuMgr.CreateBuffer(device, vbSize, L"Cylinder_VB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);
    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, meshData.Vertices.data(), vbSize);
        vbResource->Unmap(0, nullptr);
    }

    size_t ibSize = meshData.Indices32.size() * sizeof(uint32_t);
    auto ibHandle =
        gpuMgr.CreateBuffer(device, ibSize, L"Cylinder_IB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);
    if (ibResource) {
        void *ibMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &ibMapped);
        memcpy(ibMapped, meshData.Indices32.data(), ibSize);
        ibResource->Unmap(0, nullptr);
    }

    TriangleMesh triangleMesh;
    triangleMesh.vertexBufferHandle = vbHandle;
    triangleMesh.indexBufferHandle = ibHandle;
    triangleMesh.vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
    triangleMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    triangleMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    triangleMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    triangleMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    triangleMesh.isGpuReady = true;

    Math::BoundingAABB bounds;
    bounds.min = XMFLOAT3(-1.0f, -1.0f, -1.0f);
    bounds.max = XMFLOAT3(1.0f, 1.0f, 1.0f);

    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterGeometry<TriangleMesh>(triangleMesh);
    if (!geoHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] RegisterGeometry for cylinder failed!");
        return;
    }

    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle};
    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    auto entity = m_registry->CreateEntity();
    m_registry->AddComponent<TransformComponent>(entity, XMFLOAT3(4.0f, 32.0f, -3.0f), XMFLOAT3(0.0f, 0.0f, 0.0f),
                                                 XMFLOAT3(1.0f, 1.0f, 1.0f));
    MeshComponent meshComp;
    meshComp.lodMeshHandle = lodHandle;
    meshComp.localBounds = bounds;
    meshComp.materialHandle = m_brickMaterialHandle;

    meshComp.receivesShadow = true;
    m_registry->AddComponent<MeshComponent>(entity, std::move(meshComp));
    m_registry->AddComponent<OpaqueTag>(entity);
    m_context->Logging->Info("[GameWorld] Test cylinder created with brick material + normal map");
}

void GameWorld::CreateTestTorus() {
    if (!m_registry || !m_renderer || !m_context)
        return;

    constexpr float R = 2.0f;
    constexpr float r = 0.8f;
    constexpr UINT slices = 48;
    constexpr UINT stacks = 24;
    constexpr float PI = 3.14159265358979f;

    std::vector<GeometryGenerator::Vertex> vertices;
    std::vector<uint32_t> indices;

    for (UINT i = 0; i <= slices; ++i) {
        float theta = (float)i / slices * 2.0f * PI;
        float cosTheta = cosf(theta);
        float sinTheta = sinf(theta);

        for (UINT j = 0; j <= stacks; ++j) {
            float phi = (float)j / stacks * 2.0f * PI;
            float cosPhi = cosf(phi);
            float sinPhi = sinf(phi);

            float px = (R + r * cosPhi) * cosTheta;
            float py = r * sinPhi;
            float pz = (R + r * cosPhi) * sinTheta;

            float nx = cosPhi * cosTheta;
            float ny = sinPhi;
            float nz = cosPhi * sinTheta;

            float tx = -sinTheta;
            float ty = 0.0f;
            float tz = cosTheta;

            float u = (float)i / slices;
            float v = (float)j / stacks;

            vertices.emplace_back(px, py, pz, nx, ny, nz, tx, ty, tz, u, v);
        }
    }

    for (UINT i = 0; i < slices; ++i) {
        for (UINT j = 0; j < stacks; ++j) {
            UINT i0 = i * (stacks + 1) + j;
            UINT i1 = i0 + 1;
            UINT i2 = (i + 1) * (stacks + 1) + j;
            UINT i3 = i2 + 1;

            indices.push_back(i0);
            indices.push_back(i1);
            indices.push_back(i2);

            indices.push_back(i1);
            indices.push_back(i3);
            indices.push_back(i2);
        }
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

    size_t vbSize = vertices.size() * sizeof(GeometryGenerator::Vertex);
    auto vbHandle =
        gpuMgr.CreateBuffer(device, vbSize, L"Torus_VB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);
    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, vertices.data(), vbSize);
        vbResource->Unmap(0, nullptr);
    }

    size_t ibSize = indices.size() * sizeof(uint32_t);
    auto ibHandle =
        gpuMgr.CreateBuffer(device, ibSize, L"Torus_IB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);
    if (ibResource) {
        void *ibMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &ibMapped);
        memcpy(ibMapped, indices.data(), ibSize);
        ibResource->Unmap(0, nullptr);
    }

    TriangleMesh triangleMesh;
    triangleMesh.vertexBufferHandle = vbHandle;
    triangleMesh.indexBufferHandle = ibHandle;
    triangleMesh.vertexCount = static_cast<uint32_t>(vertices.size());
    triangleMesh.indexCount = static_cast<uint32_t>(indices.size());
    triangleMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    triangleMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    triangleMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    triangleMesh.isGpuReady = true;

    float extent = R + r;
    Math::BoundingAABB bounds;
    bounds.min = XMFLOAT3(-extent, -r, -extent);
    bounds.max = XMFLOAT3(extent, r, extent);

    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterGeometry<TriangleMesh>(triangleMesh);
    if (!geoHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] RegisterGeometry for torus failed!");
        return;
    }

    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle};
    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    auto entity = m_registry->CreateEntity();
    m_registry->AddComponent<TransformComponent>(entity, XMFLOAT3(0.0f, 32.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, 0.0f),
                                                 XMFLOAT3(1.0f, 1.0f, 1.0f));
    MeshComponent meshComp;
    meshComp.lodMeshHandle = lodHandle;
    meshComp.localBounds = bounds;
    meshComp.materialHandle = m_brickMaterialHandle;

    meshComp.receivesShadow = true;
    m_registry->AddComponent<MeshComponent>(entity, std::move(meshComp));
    m_registry->AddComponent<OpaqueTag>(entity);

    m_context->Logging->Info("[GameWorld] Test torus created: {} vertices, {} triangles (SSAO test)", vertices.size(),
                             indices.size() / 3);
}

void GameWorld::TestM3dLoader() {
    if (!m_context)
        return;

    const char *testFiles[] = {"Content/Models/soldier.m3d", nullptr};

    std::ofstream outFile("M3dVerify.txt");
    if (!outFile) {
        m_context->Logging->Warn("[M3dTest] Cannot create M3dVerify.txt, writing to debug output only");
    }

    auto log = [&](const std::string &msg) {
        m_context->Logging->Info("[M3dTest] {}", msg);
        if (outFile) {
            outFile << msg << std::endl;
        }
    };

    int loadedCount = 0;
    for (int i = 0; testFiles[i] != nullptr; ++i) {
        std::string filepath = testFiles[i];
        m_context->Logging->Info("[M3dTest] Attempting to load: {}", filepath);

        Resource::M3dMeshData meshData;
        if (!Resource::M3dLoader::LoadFromFile(filepath, meshData)) {
            log(filepath + " — FAILED to load (file not found or invalid format)");
            continue;
        }

        log("=== " + filepath + " ===");
        log("  vertices:  " + std::to_string(meshData.vertices.size()));
        log("  indices:   " + std::to_string(meshData.indices.size()));
        log("  subsets:   " + std::to_string(meshData.subsets.size()));
        log("  materials: " + std::to_string(meshData.materials.size()));
        log("  bones:     " + std::to_string(meshData.boneHierarchy.size()));
        log("  animations:" + std::to_string(meshData.animations.size()));

        for (size_t s = 0; s < meshData.subsets.size(); ++s) {
            auto &sub = meshData.subsets[s];
            log("  subset[" + std::to_string(s) + "]: " + std::to_string(sub.faceCount) + " faces, vertexOffset=" +
                std::to_string(sub.vertexStart) + ", indexStart=" + std::to_string(sub.faceStart * 3));
        }

        for (size_t m = 0; m < meshData.materials.size(); ++m) {
            auto &mat = meshData.materials[m];
            log("  material[" + std::to_string(m) + "]: Diffuse=" + mat.DiffuseMapName +
                " Normal=" + mat.NormalMapName + " AlphaClip=" + (mat.AlphaClip ? "true" : "false"));
        }

        size_t a = 0;
        for (auto &[name, anim] : meshData.animations) {
            log("  animation[" + std::to_string(a) + "]: " + name +
                " bones=" + std::to_string(anim.BoneAnimations.size()) +
                " start=" + std::to_string(anim.GetClipStartTime()) + " end=" + std::to_string(anim.GetClipEndTime()));
            ++a;
        }

        ++loadedCount;
    }

    log("Loaded " + std::to_string(loadedCount) + " files");
    if (loadedCount == 0) {
        m_context->Logging->Warn("[M3dTest] No files loaded — verify Content/Models/ directory");
    }
}

void GameWorld::CreateSkybox() {
    m_skyRenderer = std::make_unique<SkyRenderer>();
    m_skyRenderer->SetDeviceContext(m_context->DeviceContext);
    m_skyRenderer->SetGeometryResourceManager(m_context->GeometryResourceManager);
    m_skyRenderer->Initialize();

    DDSTextureInfo ddsInfo;
    std::wstring texturePath = L"Content/Textures/snowcube1024.dds";

    if (!AssetLoader::GetInstance().LoadTextureFromFile(texturePath, ddsInfo)) {
        m_context->Logging->Error("[GameWorld] Failed to load skybox texture");
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();

    GpuResourceHandle gpuHandle =
        gpuMgr.CreateTexture2D(device, ddsInfo.desc, L"Skybox_Texture", D3D12_RESOURCE_STATE_COMMON);
    if (!gpuHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to create skybox GPU texture");
        return;
    }

    auto &descriptorHeaps = m_context->DescriptorHeaps;
    uint32_t srvIndex = descriptorHeaps->Allocate(PartitionType::Texture);
    if (srvIndex == UINT32_MAX) {
        m_context->Logging->Error("[GameWorld] Failed to allocate SRV for skybox");
        gpuMgr.Release(gpuHandle, 0);
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = ddsInfo.desc.Format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MipLevels = ddsInfo.desc.MipLevels;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvIndex);
    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

    TextureManager *texMgr = m_context->TextureMgr;
    m_skyboxTextureHandle = texMgr->RegisterTexture(gpuHandle, srvIndex);

    uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
    auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
    auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
    auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

    std::vector<D3D12_SUBRESOURCE_DATA> subresources = ddsInfo.subresources;

    UINT64 requiredSize =
        GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0, static_cast<UINT>(subresources.size()));

    GpuResourceHandle uploadHandle = gpuMgr.CreateBuffer(device, requiredSize, L"Skybox_Upload", D3D12_HEAP_TYPE_UPLOAD,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ);

    auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList.Get()->ResourceBarrier(1, &barrier1);

    UpdateSubresources(cmdList.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadHandle), 0, 0,
                       static_cast<UINT>(subresources.size()), subresources.data());

    auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList.Get()->ResourceBarrier(1, &barrier2);

    cmdList.Close();
    m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
    m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

    uint64_t sequence = m_context->GetNextSequence();
    gpuMgr.Release(uploadHandle, sequence);
    m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
    m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);

    m_context->Logging->Info("[GameWorld] Skybox texture loaded successfully");

    GeometryGenerator geoGen;
    auto skyMeshData = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 0);

    size_t vbSize = skyMeshData.Vertices.size() * sizeof(GeometryGenerator::Vertex);
    auto vbHandle =
        gpuMgr.CreateBuffer(device, vbSize, L"Skybox_VB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);
    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, skyMeshData.Vertices.data(), vbSize);
        vbResource->Unmap(0, nullptr);
    }

    size_t ibSize = skyMeshData.Indices32.size() * sizeof(uint32_t);
    auto ibHandle =
        gpuMgr.CreateBuffer(device, ibSize, L"Skybox_IB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);
    if (ibResource) {
        void *ibMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &ibMapped);
        memcpy(ibMapped, skyMeshData.Indices32.data(), ibSize);
        ibResource->Unmap(0, nullptr);
    }

    TriangleMesh skyTriangleMesh;
    skyTriangleMesh.vertexBufferHandle = vbHandle;
    skyTriangleMesh.indexBufferHandle = ibHandle;
    skyTriangleMesh.vertexCount = static_cast<uint32_t>(skyMeshData.Vertices.size());
    skyTriangleMesh.indexCount = static_cast<uint32_t>(skyMeshData.Indices32.size());
    skyTriangleMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    skyTriangleMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    skyTriangleMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    skyTriangleMesh.isGpuReady = true;

    m_skyboxGeometryHandle = m_context->GeometryResourceManager->RegisterGeometry<TriangleMesh>(skyTriangleMesh);

    if (!m_skyboxGeometryHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to register skybox geometry");
        return;
    }

    ObjectConstants skyObjCB;
    DirectX::XMStoreFloat4x4(&skyObjCB.World, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&skyObjCB.WorldInvTranspose, DirectX::XMMatrixIdentity());
    DirectX::XMStoreFloat4x4(&skyObjCB.PrevWorld, DirectX::XMMatrixIdentity());
    skyObjCB.MaterialIndex = 0;
    skyObjCB.ReceiveShadow = 0;

    m_skyboxObjectCBAddress = m_context->FrameResourceManager->AllocateObjectCB(&skyObjCB, sizeof(ObjectConstants));

    m_context->Logging->Info("[GameWorld] Skybox created successfully");

    RegisterSkyboxSystem();
}

void GameWorld::CreateWater() {
    if (!m_registry || !m_renderer || !m_context)
        return;

    GeometryGenerator geoGen;
    auto meshData = geoGen.CreateGrid(256.0f, 256.0f, 64, 64);

    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto device = m_context->DeviceContext->GetDevice();

    size_t vbSize = meshData.Vertices.size() * sizeof(GeometryGenerator::Vertex);
    auto vbHandle =
        gpuMgr.CreateBuffer(device, vbSize, L"Water_MeshVB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *vbResource = gpuMgr.GetResource(vbHandle);
    if (vbResource) {
        void *vbMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        vbResource->Map(0, &readRange, &vbMapped);
        memcpy(vbMapped, meshData.Vertices.data(), vbSize);
        vbResource->Unmap(0, nullptr);
    }

    size_t ibSize = meshData.Indices32.size() * sizeof(uint32_t);
    auto ibHandle =
        gpuMgr.CreateBuffer(device, ibSize, L"Water_MeshIB", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ibResource = gpuMgr.GetResource(ibHandle);
    if (ibResource) {
        void *ibMapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        ibResource->Map(0, &readRange, &ibMapped);
        memcpy(ibMapped, meshData.Indices32.data(), ibSize);
        ibResource->Unmap(0, nullptr);
    }

    GridGeometry waterMesh;
    waterMesh.vertexBufferHandle = vbHandle;
    waterMesh.indexBufferHandle = ibHandle;
    waterMesh.vertexCount = static_cast<uint32_t>(meshData.Vertices.size());
    waterMesh.indexCount = static_cast<uint32_t>(meshData.Indices32.size());
    waterMesh.vertexStride = sizeof(GeometryGenerator::Vertex);
    waterMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    waterMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    waterMesh.isGpuReady = true;
    waterMesh.widthSegments = 64;
    waterMesh.depthSegments = 64;

    BoundingAABB bounds;
    bounds.min = XMFLOAT3(-128.0f, 0.0f, -128.0f);
    bounds.max = XMFLOAT3(128.0f, 0.1f, 128.0f);

    auto &geoMgr = m_context->GeometryResourceManager;
    GeometryHandle geoHandle = geoMgr->RegisterGeometry<GridGeometry>(waterMesh);

    LODMesh lodMesh;
    lodMesh.lodChain = {geoHandle};
    LODMeshHandle lodHandle = m_context->LODSystem->RegisterLODMesh(lodMesh);

    m_waterEntity = m_registry->CreateEntity();

    XMFLOAT3 position(0.0f, 10.0f, 0.0f);
    XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
    XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
    m_registry->AddComponent<TransformComponent>(m_waterEntity, position, rotation, scale);

    MeshComponent meshComp;
    meshComp.lodMeshHandle = lodHandle;
    meshComp.localBounds = bounds;
    meshComp.materialHandle = m_waterMaterialHandle;
    m_registry->AddComponent<MeshComponent>(m_waterEntity, std::move(meshComp));
    m_registry->AddComponent<TransparentTag>(m_waterEntity);

    // [GBuffer 调试] 注释 forward 渲染
    RegisterWaterRenderSystem();
}

void GameWorld::CreateBillboardTrees() {
    if (!m_registry || !m_context) {
        return;
    }

    m_billboardRenderer = std::make_unique<BillboardRenderer>();
    m_billboardRenderer->SetDeviceContext(m_context->DeviceContext);
    m_billboardRenderer->Initialize();

    m_billboardBuilder = std::make_unique<BillboardRenderItemBuilder>(m_context->FrameResourceManager,
                                                                      m_context->TextureMgr, m_context->MaterialMgr);

    constexpr float PLANE_Y = 30.0f;
    constexpr int BILLBOARD_GRID_HALF = 5;
    constexpr float BILLBOARD_SPACING = 2.0f;

    m_billboardEntities.clear();
    m_billboardEntities.reserve((BILLBOARD_GRID_HALF * 2) * (BILLBOARD_GRID_HALF * 2));

    for (int x = -BILLBOARD_GRID_HALF; x < BILLBOARD_GRID_HALF; ++x) {
        for (int z = -BILLBOARD_GRID_HALF; z < BILLBOARD_GRID_HALF; ++z) {
            if (!m_billboardTextureHandles[0].IsValid()) {
                continue;
            }

            auto entity = m_registry->CreateEntity();

            float bx = x * BILLBOARD_SPACING;
            float bz = z * BILLBOARD_SPACING;
            float bw = 2.0f + (x * z) % 3 * 0.4f;
            float bh = 4.0f + (x + z) % 5 * 0.5f;

            XMFLOAT3 position(bx, PLANE_Y + bh * 0.5f, bz);
            XMFLOAT3 rotation(0.0f, 0.0f, 0.0f);
            XMFLOAT3 scale(1.0f, 1.0f, 1.0f);
            m_registry->AddComponent<TransformComponent>(entity, position, rotation, scale);

            BillboardComponent billboardComp;
            billboardComp.textureHandle = m_billboardTextureHandles[0];
            billboardComp.materialHandle = m_billboardMaterialHandle;
            billboardComp.width = bw;
            billboardComp.height = bh;
            billboardComp.mode = BillboardMode::AxisY;
            billboardComp.minDistance = 0.5f;
            billboardComp.maxDistance = 500.0f;
            billboardComp.switchDistance = 50.0f;

            int idx = (x - BILLBOARD_GRID_HALF) * (BILLBOARD_GRID_HALF * 2) + (z - BILLBOARD_GRID_HALF);
            billboardComp.textureArrayIndex = idx % m_billboardTotalSlices;

            m_registry->AddComponent<BillboardComponent>(entity, std::move(billboardComp));

            m_billboardEntities.push_back(entity);
        }
    }

    m_context->Logging->Info("[GameWorld] {} billboard trees created ({} texture slices)", m_billboardEntities.size(),
                             m_billboardTotalSlices);

    // [点光源阴影阶段] 临时注释公告牌
    // RegisterBillboardRenderSystem();
}
