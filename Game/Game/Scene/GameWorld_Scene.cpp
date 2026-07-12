#include "Asset/Definitions/Mesh/DxMeshFormat.h"
#include "Asset/IO/AssetLoader.h"
#include "Asset/IO/Loader/DDSLoader.h"
#include "Asset/IO/Loader/DxMeshLoader.h"
#include "Boot/GameContext.h"
#include "Common/ThrowHelper.h"
#include "Common/d3dUtil.h"
#include "ECS/Core/Components.h"
#include "ECS/Core/Registry.h"
#include "GameWorld.h"
#include "Math/BoundingVolume.h"
#include "Renderer/Core/LODMesh.h"
#include "Renderer/Core/LODSystem.h"
#include "Renderer/FrameResources/FrameResourceManager.h"
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/Pipeline/BillboardRenderer.h"
#include "Renderer/Pipeline/OpaqueRenderer.h"
#include "Renderer/Pipeline/SkyRenderer.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Renderer/Scene/LightManager/LightManager.h"
#include "Renderer/Scene/SkyboxManager.h"
#include "Renderer/Utils/GeometryGenerator.h"
#include "Resource/AssetManager/AssetManager.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Geometry/GridGeometry.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Texture/TextureManager.h"
#include <DirectXMath.h>
#include <algorithm>
#include <filesystem>
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
