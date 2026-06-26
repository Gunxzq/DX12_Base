#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <d3d12.h>

namespace DX12Engine::Renderer {

struct ProbeCaptureCB {
    DirectX::XMFLOAT4X4 faceViewProj[6]; // 6 个面的 VP 矩阵
    DirectX::XMFLOAT3 probePosition;     // 探针位置
    float pad;
};

static_assert(sizeof(ProbeCaptureCB) % 16 == 0, "ProbeCaptureCB size mismatch");

struct ProbeCaptureInfo {
    DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
    float captureRange = 50.0f;
    uint32_t probeIndex = UINT32_MAX; // 反射探针索引，用于在 Render 阶段查找对应的 Cubemap

    uint32_t rtvBaseSlot = UINT32_MAX; // Cubemap RTV 基槽（GS 方案只用一个 RTV）
    uint32_t dsvSlot = UINT32_MAX;     // 共享深度 DSV 槽
    uint32_t resolution = 0;
    ID3D12Resource *cubemapResource = nullptr; // Cubemap 纹理资源指针（用于 barrier 转换）
    D3D12_GPU_VIRTUAL_ADDRESS captureCBAddress = 0;
};

} // namespace DX12Engine::Renderer
