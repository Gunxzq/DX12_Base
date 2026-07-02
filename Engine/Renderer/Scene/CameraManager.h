#pragma once

#include "Camera.h"
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace DX12Engine {
namespace Renderer {

class D3D12DeviceContext;

struct PredictedCameraData {
    DirectX::XMFLOAT3 Position = {0, 0, 0};
    DirectX::XMFLOAT3 Forward = {0, 0, 1};
    DirectX::XMFLOAT3 Up = {0, 1, 0};
    XMMATRIX InverseViewProj;
    float FOV = 0.0f;
    float AspectRatio = 0.0f;
    float NearPlane = 0.0f;
    float FarPlane = 0.0f;       // 渲染远平面（紧）
    float CullFarPlane = 0.0f;   // 剔除远平面（宽）
};

class CameraManager {

public:
    static CameraManager &GetInstance();

    CameraManager() = default;
    ~CameraManager() = default;

    CameraManager(const CameraManager &) = delete;
    CameraManager &operator=(const CameraManager &) = delete;

    void Initialize(uint32_t initialWidth, uint32_t initialHeight);
    void Shutdown();

    // ========================================================================
    // 预测相机
    // ========================================================================
    PredictedCameraData GetPredictedCameraData(float dt, float predictionFactor = 1.3f) const;

    // =========================================================================
    // 主相机管理 (Main Camera)
    // =========================================================================

    Camera &GetMainCamera();
    const Camera &GetMainCamera() const;
    void UpdateMainCamera();

    // =========================================================================
    // 辅助相机管理 (Auxiliary Cameras)
    // =========================================================================
    bool CreateAuxiliaryCamera(const std::string &name, const Camera &templateData = Camera());
    bool DestroyAuxiliaryCamera(const std::string &name);
    Camera *GetAuxiliaryCamera(const std::string &name);
    const Camera *GetAuxiliaryCamera(const std::string &name) const;

    // =========================================================================
    // 全局状态同步
    // =========================================================================
    void OnResize(uint32_t width, uint32_t height);

private:
    void CalculateMatrices(Camera &camera);

    Camera m_mainCamera;
    std::unordered_map<std::string, Camera> m_auxiliaryCameras;
};

} // namespace Renderer
} // namespace DX12Engine