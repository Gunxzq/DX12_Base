#pragma once

#include <cstdint>

namespace DX12Engine::Renderer {
class D3D12DeviceContext;
class CommandList; // 前向声明
} // namespace DX12Engine::Renderer

namespace DX12Engine::ECS {
class Registry; // 前向声明
}

namespace DX12Engine::Resource {
struct GeometryHandle;
struct MaterialHandle;
} // namespace DX12Engine::Resource

namespace DX12Engine::Renderer {

class D3D12DeviceContext;

// IRenderer 接口
// 定义了所有渲染模块（如 SceneRenderer, UIRenderer）必须遵守的契约
class IRenderer {
public:
    // 虚析构函数：确保通过基类指针删除派生类对象时能正确释放资源
    virtual ~IRenderer() = default;

    // ----------------------------------------------------------------------
    // 1. 生命周期与依赖注入
    // ----------------------------------------------------------------------

    virtual void SetDeviceContext(D3D12DeviceContext *context) = 0;
    virtual void Initialize() = 0;
    virtual void OnResize(uint32_t width, uint32_t height) = 0;
    virtual void BeginFrame(CommandList &cmdList, D3D12_GPU_VIRTUAL_ADDRESS passConstantsAddress,
                            D3D12_GPU_VIRTUAL_ADDRESS lightCBAddress) = 0;
    virtual void EndFrame() = 0;

    // ----------------------------------------------------------------------
    // 2. 帧循环逻辑
    // ----------------------------------------------------------------------

    virtual void Update(float deltaTime) = 0;

    virtual void DrawMesh(CommandList &cmdList, Resource::GeometryHandle geometryHandle,
                          const DirectX::XMMATRIX &worldMatrix, D3D12_GPU_VIRTUAL_ADDRESS objectCBAddress,
                          D3D12_GPU_VIRTUAL_ADDRESS matCBAddress) = 0;
};

} // namespace DX12Engine::Renderer