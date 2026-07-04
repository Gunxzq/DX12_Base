#include "Async/BackgroundExecutor.h"
#include "Async/ResourceTransitionTask.h"
#include "Async/TerrainLoadTask.h"
#include "Boot/GameContext.h"
#include "Common/ThrowHelper.h"
#include "Common/d3dUtil.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "Event/EventRegistry.h"
#include "Event/EventTypes.h"
#include "Event/MessageDispatcher.h"
#include "Framework/SystemRegistry.h"
#include "GameWorld.h"
#include "Math/HashTypes.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/FrameResources/Struct/FrameResourceTypes.h"
#include "Renderer/Pipeline/WaterRenderer.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/AssetDataManager.h"
#include "Resource/AssetLoader/AssetLoader.h"
#include "Resource/AssetLoader/Loader/DDSLoader.h"
#include "Resource/AssetLoader/Loader/M3dLoader.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Renderer/Material/MaterialManager.h"
#include "Resource/Manager/SkeletonManager.h"
#include "Renderer/Material/MaterialResource.h"
#include "Resource/Texture/TextureManager.h"
#include <DirectXMath.h>
#include <algorithm>
#include <string>

using namespace DirectX;
using namespace DX12Engine;
using namespace DX12Engine::Async;
using namespace DX12Engine::Boot;
using namespace DX12Engine::ECS;
using namespace DX12Engine::Renderer;
using namespace DX12Engine::Resource;
using namespace DX12Engine::Math;

// ========================================================================
// GameWorld — 资源加载
// ========================================================================

void GameWorld::LoadSoldierCharacter() {
    if (!m_registry || !m_context)
        return;

    auto device = m_context->DeviceContext->GetDevice();
    auto &gpuMgr = GpuResourceManager::GetInstance();
    auto &descriptorHeaps = m_context->DescriptorHeaps;
    auto *materialMgr = m_context->MaterialMgr;
    auto *texMgr = m_context->TextureMgr;
    auto &geoMgr = m_context->GeometryResourceManager;
    auto *skeletonMgr = m_context->SkeletonMgr;

    // 1. 解析 .m3d
    Resource::M3dMeshData meshData;
    if (!Resource::M3dLoader::LoadFromFile("Content/Models/soldier.m3d", meshData)) {
        m_context->Logging->Error("[GameWorld] Failed to load soldier.m3d");
        return;
    }
    m_context->Logging->Info("[GameWorld] Soldier loaded: {} vertices, {} subsets", meshData.vertices.size(),
                             meshData.subsets.size());

    // 2. 纹理加载
    struct LoadedTexture {
        uint32_t srvSlot = UINT32_MAX;
        Resource::TextureHandle handle;
    };
    std::vector<LoadedTexture> loadedDiffuse(meshData.materials.size());
    std::vector<LoadedTexture> loadedNormal(meshData.materials.size());

    struct PendingTex {
        Resource::GpuResourceHandle gpuHandle;
        DDSTextureInfo ddsInfo;
    };
    std::vector<PendingTex> allUploads;

    for (uint32_t m = 0; m < (uint32_t)meshData.materials.size(); ++m) {
        auto &matRef = meshData.materials[m];
        auto loadOne = [&](const std::string &filename, LoadedTexture &out) {
            if (filename.empty())
                return;
            std::wstring wpath = L"Content/Textures/" + std::wstring(filename.begin(), filename.end());
            DDSTextureInfo ddsInfo;
            if (!AssetLoader::GetInstance().LoadTextureFromFile(wpath, ddsInfo)) {
                m_context->Logging->Warn("[Soldier] Failed to load: {}", filename);
                return;
            }
            auto gpuHandle =
                gpuMgr.CreateTexture2D(device, ddsInfo.desc, L"Soldier_Texture", D3D12_RESOURCE_STATE_COMMON);
            if (!gpuHandle.IsValid())
                return;
            uint32_t srvSlot = descriptorHeaps->Allocate(PartitionType::Texture);
            if (srvSlot == UINT32_MAX) {
                gpuMgr.Release(gpuHandle, 0);
                return;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = ddsInfo.desc.Format;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
            srvDesc.Texture2D.MostDetailedMip = 0;
            auto cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvSlot);
            device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);
            out.srvSlot = srvSlot;
            out.handle = texMgr->RegisterTexture(gpuHandle, srvSlot);
            allUploads.push_back({gpuHandle, std::move(ddsInfo)});
        };
        loadOne(matRef.DiffuseMapName, loadedDiffuse[m]);
        loadOne(matRef.NormalMapName, loadedNormal[m]);
    }

    if (!allUploads.empty()) {
        m_context->Logging->Info("[Soldier] Uploading {} textures to GPU...", allUploads.size());
        uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
        auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
        auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
        auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

        struct UploadBuf {
            GpuResourceHandle handle;
        };
        std::vector<UploadBuf> uploadBufs;
        uploadBufs.reserve(allUploads.size());

        for (auto &pt : allUploads) {
            UINT64 uploadSize = GetRequiredIntermediateSize(gpuMgr.GetResource(pt.gpuHandle), 0,
                                                            static_cast<UINT>(pt.ddsInfo.subresources.size()));
            GpuResourceHandle uploadBuf =
                gpuMgr.CreateBuffer(device, uploadSize, L"Soldier_Texture_Upload", D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!uploadBuf.IsValid()) {
                m_context->Logging->Error("[Soldier] Failed to create upload buffer");
                continue;
            }
            uploadBufs.push_back({uploadBuf});

            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                gpuMgr.GetResource(pt.gpuHandle), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd.Get()->ResourceBarrier(1, &barrier);

            UpdateSubresources(cmd.Get(), gpuMgr.GetResource(pt.gpuHandle), gpuMgr.GetResource(uploadBuf), 0, 0,
                               static_cast<UINT>(pt.ddsInfo.subresources.size()), pt.ddsInfo.subresources.data());

            auto barrier2 =
                CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(pt.gpuHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmd.Get()->ResourceBarrier(1, &barrier2);
        }

        cmd.Close();
        m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmd);
        m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

        uint64_t seq = m_context->GetNextSequence();
        for (auto &ub : uploadBufs) {
            gpuMgr.Release(ub.handle, seq);
        }
        m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
        m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);
        allUploads.clear();
    }

    // 3. 注册材质
    std::vector<Resource::MaterialHandle> matHandles(meshData.materials.size());
    for (uint32_t m = 0; m < (uint32_t)meshData.materials.size(); ++m) {
        auto &matRef = meshData.materials[m];
        Resource::MaterialData matData;
        matData.materialId = TYPE_HASH(("soldier_" + std::to_string(m)).c_str());
        matData.name = "soldier_mat_" + std::to_string(m);
        matData.baseColor = matRef.DiffuseAlbedo;
        matData.roughness = matRef.Roughness;
        matData.alpha = matRef.AlphaClip ? 0.0f : 1.0f;
        matData.baseColorTextureId = loadedDiffuse[m].srvSlot;
        matData.normalTextureId = loadedNormal[m].srvSlot;
        matData.rendererTypeHash = TYPE_HASH("OpaquePBR");
        matHandles[m] = materialMgr->RegisterMaterial(matData);
    }

    // 4. 上传几何体
    size_t fullVbSize = meshData.vertices.size() * sizeof(Resource::M3dVertex);
    size_t fullIbSize = meshData.indices.size() * sizeof(uint32_t);

    auto fullVbHandle = gpuMgr.CreateBuffer(device, fullVbSize, L"Soldier_VB", D3D12_HEAP_TYPE_UPLOAD,
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
    if (auto *res = gpuMgr.GetResource(fullVbHandle)) {
        void *mapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        res->Map(0, &readRange, &mapped);
        memcpy(mapped, meshData.vertices.data(), fullVbSize);
        res->Unmap(0, nullptr);
    }

    auto fullIbHandle = gpuMgr.CreateBuffer(device, fullIbSize, L"Soldier_IB", D3D12_HEAP_TYPE_UPLOAD,
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
    if (auto *res = gpuMgr.GetResource(fullIbHandle)) {
        void *mapped = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        res->Map(0, &readRange, &mapped);
        memcpy(mapped, meshData.indices.data(), fullIbSize);
        res->Unmap(0, nullptr);
    }

    Resource::TriangleMesh fullMesh;
    fullMesh.vertexBufferHandle = fullVbHandle;
    fullMesh.indexBufferHandle = fullIbHandle;
    fullMesh.vertexCount = static_cast<uint32_t>(meshData.vertices.size());
    fullMesh.indexCount = static_cast<uint32_t>(meshData.indices.size());
    fullMesh.vertexStride = sizeof(Resource::M3dVertex);
    fullMesh.indexFormat = DXGI_FORMAT_R32_UINT;
    fullMesh.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    fullMesh.isGpuReady = true;

    Resource::GeometryHandle fullGeoHandle = geoMgr->RegisterGeometry<Resource::TriangleMesh>(fullMesh);

    Math::BoundingAABB fullBounds;
    fullBounds.min = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    fullBounds.max = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (auto &v : meshData.vertices) {
        fullBounds.min.x = std::min(fullBounds.min.x, v.Pos.x);
        fullBounds.min.y = std::min(fullBounds.min.y, v.Pos.y);
        fullBounds.min.z = std::min(fullBounds.min.z, v.Pos.z);
        fullBounds.max.x = std::max(fullBounds.max.x, v.Pos.x);
        fullBounds.max.y = std::max(fullBounds.max.y, v.Pos.y);
        fullBounds.max.z = std::max(fullBounds.max.z, v.Pos.z);
    }

    // 5. 注册骨骼
    m_soldierSkeletonHandle = Resource::SkeletonHandle::Invalid();
    if (meshData.HasSkeleton() && skeletonMgr) {
        Resource::SkeletonData skelData;
        skelData.BoneHierarchy = std::move(meshData.boneHierarchy);
        skelData.BoneOffsets = std::move(meshData.boneOffsets);
        skelData.BoneNames = std::move(meshData.boneNames);
        skelData.Animations = std::move(meshData.animations);
        m_soldierSkeletonHandle = skeletonMgr->RegisterSkeleton(skelData);
        m_context->Logging->Info("[GameWorld] Skeleton registered: {} bones", skelData.BoneCount());
    }

    // 6. 创建 ECS 实体
    m_soldierEntities.clear();
    auto *lodSystem = m_context->LODSystem;

    Resource::LODMesh fullLodMesh;
    fullLodMesh.lodChain = {fullGeoHandle};
    Resource::LODMeshHandle fullLodHandle = lodSystem->RegisterLODMesh(fullLodMesh);

    for (uint32_t s = 0; s < (uint32_t)meshData.subsets.size(); ++s) {
        auto &sub = meshData.subsets[s];

        auto entity = m_registry->CreateEntity();
        m_registry->AddComponent<TransformComponent>(entity, XMFLOAT3(0.0f, 30.0f, 0.0f), XMFLOAT3(0.0f, 0.0f, 0.0f),
                                                     XMFLOAT3(0.05f, 0.05f, 0.05f));

        ECS::MeshComponent meshComp;
        meshComp.lodMeshHandle = fullLodHandle;
        meshComp.localBounds = fullBounds;
        meshComp.materialHandle = matHandles[s];

        meshComp.receivesShadow = true;
        meshComp.indexCount = sub.faceCount * 3;
        meshComp.startIndex = sub.faceStart * 3;
        meshComp.startVertex = 0;
        m_registry->AddComponent<ECS::MeshComponent>(entity, std::move(meshComp));

        if (m_soldierSkeletonHandle.IsValid()) {
            ECS::SkinnedComponent skinnedComp;
            skinnedComp.skeletonHandle = m_soldierSkeletonHandle;
            skinnedComp.currentClip = "Take1";
            skinnedComp.timePos = 0.0f;
            m_registry->AddComponent<ECS::SkinnedComponent>(entity, std::move(skinnedComp));
            m_registry->AddComponent<ECS::SkinnedTag>(entity);
        }

        m_soldierEntities.push_back(entity);
    }

    m_context->Logging->Info("[GameWorld] Soldier character created: {} entities ({} subsets, single mesh)",
                             m_soldierEntities.size(), meshData.subsets.size());
}

void GameWorld::LoadTestTexture() {
    DDSTextureInfo ddsInfo;
    std::wstring texturePath = L"Content/Textures/WoodCrate01.dds";

    if (!AssetLoader::GetInstance().LoadTextureFromFile(texturePath, ddsInfo)) {
        OutputDebugStringW(L"[ERROR] Failed to load test texture\n");
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();
    GpuResourceHandle gpuHandle =
        gpuMgr.CreateTexture2D(device, ddsInfo.desc, L"Test_Texture", D3D12_RESOURCE_STATE_COMMON);

    if (!gpuHandle.IsValid()) {
        OutputDebugStringW(L"[ERROR] Failed to create GPU texture\n");
        return;
    }

    auto &descriptorHeaps = m_context->DescriptorHeaps;
    uint32_t srvIndex = descriptorHeaps->Allocate(PartitionType::Texture);
    if (srvIndex == UINT32_MAX) {
        OutputDebugStringW(L"[ERROR] Failed to allocate SRV index\n");
        gpuMgr.Release(gpuHandle, 0);
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = ddsInfo.desc.Format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
    srvDesc.Texture2D.MostDetailedMip = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvIndex);
    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

    TextureManager *texMgr = m_context->TextureMgr;
    m_testTextureHandle = texMgr->RegisterTexture(gpuHandle, srvIndex);
    m_testTextureSrvSlot = srvIndex;

    uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
    auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
    auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
    auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

    std::vector<D3D12_SUBRESOURCE_DATA> subresources = ddsInfo.subresources;
    UINT64 requiredSize =
        GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0, static_cast<UINT>(subresources.size()));

    GpuResourceHandle uploadHandle = gpuMgr.CreateBuffer(device, requiredSize, L"Test_Texture_Upload",
                                                         D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

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

    m_context->Logging->Info("[GameWorld] Test texture loaded successfully");
}

void GameWorld::LoadBrickTextures() {
    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();
    auto &descriptorHeaps = m_context->DescriptorHeaps;

    auto loadAndUpload = [&](const std::wstring &path, uint32_t &outSrvSlot) -> bool {
        DDSTextureInfo ddsInfo;
        if (!AssetLoader::GetInstance().LoadTextureFromFile(path, ddsInfo)) {
            m_context->Logging->Warn("[GameWorld] Failed to load: {}", std::string(path.begin(), path.end()));
            return false;
        }

        GpuResourceHandle gpuHandle =
            gpuMgr.CreateTexture2D(device, ddsInfo.desc, L"Bricks_Texture", D3D12_RESOURCE_STATE_COMMON);
        if (!gpuHandle.IsValid())
            return false;

        uint32_t srvSlot = descriptorHeaps->Allocate(PartitionType::Texture);
        if (srvSlot == UINT32_MAX) {
            gpuMgr.Release(gpuHandle, 0);
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = ddsInfo.desc.Format;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvSlot);
        device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

        outSrvSlot = srvSlot;

        uint64_t fence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto allocH = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(fence);
        auto alloc = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH);
        auto cmdH = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(alloc);
        auto cmd = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);

        auto subresources = ddsInfo.subresources;
        UINT64 uploadSize =
            GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0, static_cast<UINT>(subresources.size()));
        GpuResourceHandle uploadBuf = gpuMgr.CreateBuffer(device, uploadSize, L"Bricks_Texture_Upload",
                                                          D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
        cmd.Get()->ResourceBarrier(1, &b1);
        UpdateSubresources(cmd.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadBuf), 0, 0,
                           static_cast<UINT>(subresources.size()), subresources.data());
        auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmd.Get()->ResourceBarrier(1, &b2);
        cmd.Close();
        m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmd);
        m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);
        uint64_t seq = m_context->GetNextSequence();
        gpuMgr.Release(uploadBuf, seq);
        m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdH);
        m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocH, seq);

        return true;
    };

    // 漫反射
    {
        uint32_t srvSlot = UINT32_MAX;
        if (loadAndUpload(L"Content/Textures/bricks2.dds", srvSlot)) {
            m_brickTextureSrvSlot = srvSlot;
            // 注册到 TextureManager
            // 简化处理：只记录槽位，实际采样不走 TextureManager
        }
    }

    // 法线贴图
    if (loadAndUpload(L"Content/Textures/bricks2_NRM.dds", m_brickNormalSrvSlot)) {
        m_context->Logging->Info("[SlotDBG] Brick normal map SRV slot={}", m_brickNormalSrvSlot);
    }

    // AO 贴图
    loadAndUpload(L"Content/Textures/bricks2_OCC.dds", m_brickOcclusionSrvSlot);

    // 金属度-粗糙度贴图
    loadAndUpload(L"Content/Textures/bricks2_SPEC.dds", m_brickMetallicRoughnessSrvSlot);

    // 高度/位移贴图
    loadAndUpload(L"Content/Textures/bricks2_DISP.dds", m_brickHeightSrvSlot);

    m_context->Logging->Info("[GameWorld] Brick textures loaded");
}

void GameWorld::CreateMaterials() {
    auto *materialMgr = m_context->MaterialMgr;

    MaterialData cubeMaterial;
    cubeMaterial.materialId = TYPE_HASH("cube_material");
    cubeMaterial.name = "cube_material";
    cubeMaterial.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    cubeMaterial.metallic = 0.0f;
    cubeMaterial.roughness = 0.5f;
    cubeMaterial.ambient = 0.5f;
    cubeMaterial.alpha = 1.0f;
    cubeMaterial.baseColorTextureId = m_testTextureSrvSlot != UINT32_MAX ? m_testTextureSrvSlot : 0;
    cubeMaterial.rendererTypeHash = TYPE_HASH("OpaquePBR");
    m_cubeMaterialHandle = materialMgr->RegisterMaterial(cubeMaterial);

    // 砖块材质
    MaterialData brickMaterial;
    brickMaterial.materialId = TYPE_HASH("brick_material");
    brickMaterial.name = "brick_material";
    brickMaterial.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    brickMaterial.metallic = 0.0f;
    brickMaterial.roughness = 0.4f;
    brickMaterial.ambient = 0.5f;
    brickMaterial.alpha = 1.0f;
    brickMaterial.normalIntensity = 1.0f;
    brickMaterial.baseColorTextureId = m_brickTextureSrvSlot != UINT32_MAX ? m_brickTextureSrvSlot : 0;
    brickMaterial.normalTextureId = m_brickNormalSrvSlot != UINT32_MAX ? m_brickNormalSrvSlot : 0;
    brickMaterial.metallicRoughnessTextureId =
        m_brickMetallicRoughnessSrvSlot != UINT32_MAX ? m_brickMetallicRoughnessSrvSlot : 0;
    brickMaterial.occlusionTextureId = m_brickOcclusionSrvSlot != UINT32_MAX ? m_brickOcclusionSrvSlot : 0;
    brickMaterial.heightTextureId = m_brickHeightSrvSlot != UINT32_MAX ? m_brickHeightSrvSlot : 0;
    brickMaterial.rendererTypeHash = TYPE_HASH("OpaquePBR");
    m_brickMaterialHandle = materialMgr->RegisterMaterial(brickMaterial);

    // 地形材质
    MaterialData terrainMaterial;
    terrainMaterial.materialId = TYPE_HASH("terrain_material");
    terrainMaterial.name = "terrain_material";
    terrainMaterial.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    terrainMaterial.metallic = 0.0f;
    terrainMaterial.roughness = 0.8f;
    terrainMaterial.ambient = 0.8f;
    terrainMaterial.alpha = 1.0f;
    terrainMaterial.rendererTypeHash = TYPE_HASH("OpaquePBR");
    m_terrainMaterialHandle = materialMgr->RegisterMaterial(terrainMaterial);

    // 水材质
    MaterialData waterMaterial;
    waterMaterial.materialId = TYPE_HASH("water_material");
    waterMaterial.name = "water_material";
    waterMaterial.baseColor = XMFLOAT4(0.2f, 0.4f, 0.6f, 0.8f);
    waterMaterial.metallic = 0.0f;
    waterMaterial.roughness = 0.2f;
    waterMaterial.ambient = 0.5f;
    waterMaterial.alpha = 0.8f;
    waterMaterial.rendererTypeHash = TYPE_HASH("TransparentPBR");
    m_waterMaterialHandle = materialMgr->RegisterMaterial(waterMaterial);

    // 反射探针测试材质
    MaterialData reflectionTestMaterial;
    reflectionTestMaterial.materialId = TYPE_HASH("reflection_test_material");
    reflectionTestMaterial.name = "reflection_test_material";
    reflectionTestMaterial.baseColor = XMFLOAT4(0.9f, 0.9f, 1.0f, 1.0f);
    reflectionTestMaterial.metallic = 1.0f;
    reflectionTestMaterial.roughness = 0.2f;
    reflectionTestMaterial.ambient = 0.1f;
    reflectionTestMaterial.alpha = 1.0f;
    reflectionTestMaterial.baseColorTextureId = m_whiteTextureSrvSlot != UINT32_MAX ? m_whiteTextureSrvSlot : 0;
    reflectionTestMaterial.rendererTypeHash = TYPE_HASH("OpaquePBR");
    m_reflectionTestMaterialHandle = materialMgr->RegisterMaterial(reflectionTestMaterial);

    // 公告牌材质
    MaterialData billboardMaterial;
    billboardMaterial.materialId = TYPE_HASH("billboard_material");
    billboardMaterial.name = "billboard_material";
    billboardMaterial.baseColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    billboardMaterial.metallic = 0.0f;
    billboardMaterial.roughness = 0.8f;
    billboardMaterial.ambient = 1.0f;
    billboardMaterial.alpha = 1.0f;
    billboardMaterial.rendererTypeHash = TYPE_HASH("OpaquePBR");
    m_billboardMaterialHandle = materialMgr->RegisterMaterial(billboardMaterial);

    // 创建材质数组 GPU Buffer
    auto materialList = materialMgr->GetGPUMaterialList();
    if (materialList.empty()) {
        m_context->Logging->Error("[GameWorld] Material list is empty, cannot create material buffer");
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();

    std::vector<MaterialConstants> gpuData;
    gpuData.reserve(materialList.size());
    for (auto &[idx, constants] : materialList) {
        gpuData.push_back(constants);
    }

    size_t bufferSize = gpuData.size() * sizeof(MaterialConstants);

    GpuResourceHandle bufferHandle = gpuMgr.CreateBuffer(device, bufferSize, L"Material_Buffer",
                                                         D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    if (!bufferHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to create material GPU buffer");
        return;
    }

    GpuResourceHandle uploadHandle = gpuMgr.CreateBuffer(device, bufferSize, L"Material_Buffer_Upload",
                                                         D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!uploadHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to create material upload buffer");
        gpuMgr.Release(bufferHandle, 0);
        return;
    }

    ID3D12Resource *uploadResource = gpuMgr.GetResource(uploadHandle);
    void *mappedData = nullptr;
    uploadResource->Map(0, nullptr, &mappedData);
    memcpy(mappedData, gpuData.data(), bufferSize);
    uploadResource->Unmap(0, nullptr);

    uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
    auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
    auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
    auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

    auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(bufferHandle), D3D12_RESOURCE_STATE_COMMON,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList.Get()->ResourceBarrier(1, &barrier1);
    cmdList.Get()->CopyResource(gpuMgr.GetResource(bufferHandle), uploadResource);
    auto barrier2 =
        CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(bufferHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList.Get()->ResourceBarrier(1, &barrier2);
    cmdList.Close();

    m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
    m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

    uint64_t sequence = m_context->GetNextSequence();
    gpuMgr.Release(uploadHandle, sequence);
    m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
    m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, sequence);

    auto &descriptorHeaps = m_context->DescriptorHeaps;
    uint32_t srvIndex = descriptorHeaps->Allocate(PartitionType::Buffer);
    if (srvIndex == UINT32_MAX) {
        m_context->Logging->Error("[GameWorld] Failed to allocate SRV for material buffer");
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = static_cast<UINT>(gpuData.size());
    srvDesc.Buffer.StructureByteStride = sizeof(MaterialConstants);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Buffer, srvIndex);
    device->CreateShaderResourceView(gpuMgr.GetResource(bufferHandle), &srvDesc, cpuHandle);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = descriptorHeaps->GetPartitionGpuHandle(PartitionType::Buffer, srvIndex);
    materialMgr->SetMaterialBufferSRV(gpuHandle);
    m_materialBufferHandle = bufferHandle;

    m_context->Logging->Info("[GameWorld] Material buffer created: {} materials", gpuData.size());
}

void GameWorld::LoadWaterTexture() {
    DDSTextureInfo ddsInfo;
    std::wstring texturePath = L"Content/Textures/water1.dds";

    if (!AssetLoader::GetInstance().LoadTextureFromFile(texturePath, ddsInfo)) {
        m_context->Logging->Error("[GameWorld] Failed to load water texture");
        m_waterTextureHandle = m_testTextureHandle;
        return;
    }

    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();
    GpuResourceHandle gpuHandle =
        gpuMgr.CreateTexture2D(device, ddsInfo.desc, L"Water_Texture", D3D12_RESOURCE_STATE_COMMON);

    if (!gpuHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to create water GPU texture");
        m_waterTextureHandle = m_testTextureHandle;
        return;
    }

    auto &descriptorHeaps = m_context->DescriptorHeaps;
    uint32_t srvIndex = descriptorHeaps->Allocate(PartitionType::Texture);
    if (srvIndex == UINT32_MAX) {
        m_context->Logging->Error("[GameWorld] Failed to allocate SRV for water texture");
        gpuMgr.Release(gpuHandle, 0);
        m_waterTextureHandle = m_testTextureHandle;
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = ddsInfo.desc.Format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = ddsInfo.desc.MipLevels;
    srvDesc.Texture2D.MostDetailedMip = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvIndex);
    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

    TextureManager *texMgr = m_context->TextureMgr;
    m_waterTextureHandle = texMgr->RegisterTexture(gpuHandle, srvIndex);
    m_waterTextureSrvSlot = srvIndex;
    m_waterRenderer->SetWaterTextureSRV(texMgr->GetSRV(m_waterTextureHandle));

    // 上传纹理
    uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
    auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
    auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
    auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

    std::vector<D3D12_SUBRESOURCE_DATA> subresources = ddsInfo.subresources;
    UINT64 requiredSize =
        GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0, static_cast<UINT>(subresources.size()));

    GpuResourceHandle uploadHandle = gpuMgr.CreateBuffer(device, requiredSize, L"Water_Texture_Upload",
                                                         D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

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

    m_context->Logging->Info("[GameWorld] Water texture loaded successfully");
}

void GameWorld::LoadBillboardTextures() {
    auto &gpuMgr = GpuResourceManager::GetInstance();
    ID3D12Device *device = m_context->DeviceContext->GetDevice();
    auto &descriptorHeaps = m_context->DescriptorHeaps;
    TextureManager *texMgr = m_context->TextureMgr;

    DDSTextureInfo ddsInfo;
    if (!AssetLoader::GetInstance().LoadTextureFromFile(L"Content/Textures/treearray.dds", ddsInfo)) {
        m_context->Logging->Error("[GameWorld] Failed to load billboard texture treearray.dds");
        return;
    }

    D3D12_RESOURCE_DESC &desc = ddsInfo.desc;
    uint32_t arraySize = static_cast<uint32_t>(desc.DepthOrArraySize);
    m_billboardTotalSlices = arraySize;

    GpuResourceHandle gpuHandle =
        gpuMgr.CreateTexture2D(device, desc, L"Billboard_TextureArray", D3D12_RESOURCE_STATE_COMMON);
    if (!gpuHandle.IsValid()) {
        m_context->Logging->Error("[GameWorld] Failed to create billboard Texture2DArray");
        return;
    }

    uint32_t srvIndex = descriptorHeaps->Allocate(PartitionType::Texture);
    if (srvIndex == UINT32_MAX) {
        m_context->Logging->Error("[GameWorld] Failed to allocate SRV slot for billboard Texture2DArray");
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = desc.MipLevels;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = arraySize;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeaps->GetPartitionCpuHandle(PartitionType::Texture, srvIndex);
    device->CreateShaderResourceView(gpuMgr.GetResource(gpuHandle), &srvDesc, cpuHandle);

    TextureHandle texHandle = texMgr->RegisterTexture(gpuHandle, srvIndex);

    // 上传纹理
    uint64_t completedFence = m_context->GetFenceValue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto allocatorHandle = m_context->GetAllocatorHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(completedFence);
    auto allocator = m_context->GetAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle);
    auto cmdListHandle = m_context->AcquireCommandListHandle<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocator);
    auto cmdList = m_context->GetCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);

    auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COMMON,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList.Get()->ResourceBarrier(1, &barrier1);

    UINT64 requiredSize =
        GetRequiredIntermediateSize(gpuMgr.GetResource(gpuHandle), 0, static_cast<UINT>(ddsInfo.subresources.size()));
    GpuResourceHandle uploadHandle = gpuMgr.CreateBuffer(device, requiredSize, L"Billboard_Upload",
                                                         D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

    UpdateSubresources(cmdList.Get(), gpuMgr.GetResource(gpuHandle), gpuMgr.GetResource(uploadHandle), 0, 0,
                       static_cast<UINT>(ddsInfo.subresources.size()), ddsInfo.subresources.data());

    auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(gpuMgr.GetResource(gpuHandle), D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList.Get()->ResourceBarrier(1, &barrier2);
    cmdList.Close();
    m_context->DeviceContext->GetCommandManager().Submit(D3D12_COMMAND_LIST_TYPE_DIRECT, cmdList);
    m_context->DeviceContext->GetCommandManager().Flush(D3D12_COMMAND_LIST_TYPE_DIRECT);

    uint64_t releaseSequence = m_context->GetNextSequence();
    gpuMgr.Release(uploadHandle, releaseSequence);
    m_context->ReleaseCommandList<D3D12_COMMAND_LIST_TYPE_DIRECT>(cmdListHandle);
    m_context->ReleaseAllocator<D3D12_COMMAND_LIST_TYPE_DIRECT>(allocatorHandle, releaseSequence);

    for (int i = 0; i < 4; ++i) {
        m_billboardTextureHandles[i] = texHandle;
    }
    for (int i = 0; i < 4; ++i) {
        m_billboardSliceOffsets[i] = 0;
    }

    m_context->Logging->Info("[GameWorld] Billboard Texture2DArray created: {}x{}, {} slices, SRV index {}", desc.Width,
                             desc.Height, arraySize, srvIndex);
}

void GameWorld::LoadTerrainAsync() {
    if (!m_context || !m_registry || !m_backgroundExecutor) {
        m_context->Logging->Error("[LoadTerrainAsync] Invalid state");
        return;
    }

    static std::atomic<uint32_t> s_nextRequestId{1};
    uint32_t requestId = s_nextRequestId++;
    m_terrainRequestId = requestId;

    m_context->Logging->Info("[LoadTerrainAsync] Starting async terrain loading (request={})...", requestId);

    m_terrainReadyState = std::make_shared<Async::TerrainReadyState>();

    Async::TerrainLoadTaskFactory::Input input;
    input.device = m_context->DeviceContext->GetDevice();
    input.cmdMgr = &m_context->DeviceContext->GetCommandManager();
    input.descriptorHeaps = m_context->DescriptorHeaps;
    input.readyState = m_terrainReadyState;
    input.backgroundExecutor = m_backgroundExecutor.get();

    Async::TerrainLoadDataPtr terrainData;
    auto loadTask = Async::TerrainLoadTaskFactory::Create(requestId, L"Content/Terrain/H_Source_heightmap.png", 256.0f,
                                                          256.0f, 20.0f, 256, terrainData, input);
    m_terrainLoadData = terrainData;

    m_backgroundExecutor->Submit(std::move(loadTask));

    m_context->Logging->Info("[LoadTerrainAsync] Task submitted to BackgroundExecutor (pending={}, total={})",
                             m_backgroundExecutor->GetPendingCount(), m_backgroundExecutor->GetTotalSubmitted());
}
