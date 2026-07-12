#include "Asset/IO/AssetLoader.h"
#include "Asset/IO/Loader/DDSLoader.h"
#include "Background/BackgroundExecutor.h"
#include "Background/ResourceTransitionTask.h"
#include "Background/TerrainLoadTask.h"
#include "Boot/GameContext.h"
#include "Common/ThrowHelper.h"
#include "Common/d3dUtil.h"
#include "Core/SharedDataStore/SharedDataStore.h"
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
#include "Renderer/Material/MaterialManager.h"
#include "Renderer/Material/MaterialResource.h"
#include "Renderer/Pipeline/WaterRenderer.h"
#include "Renderer/RHI/D3D12DeviceContext.h"
#include "Resource/Core/DescriptorHeapCollection.h"
#include "Resource/Geometry/TriangleMesh.h"
#include "Resource/GpuResourceManager.h"
#include "Resource/Manager/GeometryResourceManager.h"
#include "Resource/Manager/SkeletonManager.h"
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

// LoadWaterTexture 已迁移：由 JSON 材质系统的纹理管理替代
// 函数体保留供参考
