#include "CameraManager.h"
#include "Boot/GameContext.h"
#include "Scheduler/FrameDriver.h"
#include <DirectXMath.h>
#include <stdexcept>

using namespace DirectX;

namespace DX12Engine {
namespace Renderer {

// ========================================================================
// 单例实现
// ========================================================================

CameraManager &CameraManager::GetInstance() {
    static CameraManager instance;
    return instance;
}

// ========================================================================
// 初始化与关闭
// ========================================================================

void CameraManager::Initialize(uint32_t initialWidth, uint32_t initialHeight) {
    if (initialHeight == 0) {
        initialHeight = 1; // 避免除以零
    }

    // 初始化主相机默认参数
    m_mainCamera.Type = ProjectionType::Perspective;
    m_mainCamera.FOV = XMConvertToRadians(60.0f);
    m_mainCamera.AspectRatio = static_cast<float>(initialWidth) / static_cast<float>(initialHeight);
    m_mainCamera.NearPlane = 0.1f;
    m_mainCamera.FarPlane = 1000.0f;
    m_mainCamera.Position = XMFLOAT3(0.0f, 0.0f, -5.0f);

    // 初始计算一次矩阵
    CalculateMatrices(m_mainCamera);                               // 计算当前帧矩阵
    m_mainCamera.PrevViewProjMatrix = m_mainCamera.ViewProjMatrix; // 第一帧时与当前帧相同
}

void CameraManager::Shutdown() {
    m_auxiliaryCameras.clear();
    m_deviceContext = nullptr;
}

// ========================================================================
// 主相机管理
// ========================================================================

Camera &CameraManager::GetMainCamera() { return m_mainCamera; }

const Camera &CameraManager::GetMainCamera() const { return m_mainCamera; }

void CameraManager::UpdateMainCamera() {
    // 保存上一帧的 ViewProj 矩阵
    m_mainCamera.PrevViewProjMatrix = m_mainCamera.ViewProjMatrix;

    // 计算相机速度（用于下一帧的预测剔除）
    // Velocity = (Position - PrevPosition) / deltaTime
    float deltaTime = 1.0f / 60.0f; // 默认 60fps 兜底
    // 尝试从全局上下文获取实际 deltaTime
    auto &schedulerCtx = Scheduler::GetSchedulerContext();
    if (schedulerCtx.frameDriver && schedulerCtx.frameDriver->GetGameContext()) {
        deltaTime = schedulerCtx.frameDriver->GetGameContext()->MainTimer->GetDeltaTime();
        if (deltaTime <= 0.0f)
            deltaTime = 1.0f / 60.0f;
    }

    XMVECTOR posVec = XMLoadFloat3(&m_mainCamera.Position);
    XMVECTOR prevPosVec = XMLoadFloat3(&m_mainCamera.PrevPosition);
    XMVECTOR velVec = (posVec - prevPosVec) / deltaTime;
    XMStoreFloat3(&m_mainCamera.Velocity, velVec);

    // 保存当前位置为上一帧位置
    m_mainCamera.PrevPosition = m_mainCamera.Position;

    CalculateMatrices(m_mainCamera);
}

// ========================================================================
// 辅助相机管理
// ========================================================================

bool CameraManager::CreateAuxiliaryCamera(const std::string &name, const Camera &templateData) {
    if (m_auxiliaryCameras.find(name) != m_auxiliaryCameras.end()) {
        return false; // 已存在
    }

    Camera cam = templateData;
    CalculateMatrices(cam); // 预计算初始矩阵
    m_auxiliaryCameras[name] = cam;
    return true;
}

bool CameraManager::DestroyAuxiliaryCamera(const std::string &name) {
    auto it = m_auxiliaryCameras.find(name);
    if (it == m_auxiliaryCameras.end()) {
        return false;
    }
    m_auxiliaryCameras.erase(it);
    return true;
}

Camera *CameraManager::GetAuxiliaryCamera(const std::string &name) {
    auto it = m_auxiliaryCameras.find(name);
    if (it == m_auxiliaryCameras.end()) {
        return nullptr;
    }
    return &it->second;
}

const Camera *CameraManager::GetAuxiliaryCamera(const std::string &name) const {
    auto it = m_auxiliaryCameras.find(name);
    if (it == m_auxiliaryCameras.end()) {
        return nullptr;
    }
    return &it->second;
}

// ========================================================================
// 全局状态同步
// ========================================================================

void CameraManager::OnResize(uint32_t width, uint32_t height) {
    if (height == 0)
        return;

    float aspectRatio = static_cast<float>(width) / static_cast<float>(height);

    // 更新主相机
    m_mainCamera.AspectRatio = aspectRatio;
    CalculateMatrices(m_mainCamera);

    // 更新所有辅助相机
    for (auto &[name, cam] : m_auxiliaryCameras) {
        cam.AspectRatio = aspectRatio;
        CalculateMatrices(cam);
    }
}

// ========================================================================
// 内部方法：纯数学计算
// ========================================================================

void CameraManager::CalculateMatrices(Camera &camera) {
    // 1. 计算旋转矩阵
    XMMATRIX rotMatrix = XMMatrixRotationRollPitchYaw(camera.Rotation.x, camera.Rotation.y, camera.Rotation.z);

    // 2. 计算前向量和上向量（用于调试或后续逻辑）
    XMVECTOR forwardVec = XMVector3TransformNormal(FXMVECTOR{0.0f, 0.0f, 1.0f}, rotMatrix);
    XMVECTOR upVec = XMVector3TransformNormal(FXMVECTOR{0.0f, 1.0f, 0.0f}, rotMatrix);

    XMStoreFloat3(&camera.Forward, forwardVec);
    XMStoreFloat3(&camera.Up, upVec);

    // 3. 计算视图矩阵 (View Matrix)
    // LookAtLH: EyePosition, FocusPosition, UpDirection
    XMVECTOR posVec = XMLoadFloat3(&camera.Position);
    XMVECTOR targetVec = posVec + forwardVec; // 看向正前方

    camera.ViewMatrix = XMMatrixLookAtLH(posVec, targetVec, upVec);

    // 4. 计算投影矩阵 (Projection Matrix)
    if (camera.Type == ProjectionType::Perspective) {
        camera.ProjMatrix = XMMatrixPerspectiveFovLH(camera.FOV, camera.AspectRatio, camera.NearPlane, camera.FarPlane);
    } else {
        // Orthographic
        float orthoWidth = camera.OrthoSize * camera.AspectRatio;
        camera.ProjMatrix = XMMatrixOrthographicLH(orthoWidth, camera.OrthoSize, camera.NearPlane, camera.FarPlane);
    }

    // 5. 计算 ViewProj 和逆矩阵
    camera.ViewProjMatrix = camera.ViewMatrix * camera.ProjMatrix;
    camera.InverseView = XMMatrixInverse(nullptr, camera.ViewMatrix);
    camera.InverseProj = XMMatrixInverse(nullptr, camera.ProjMatrix);
    camera.InverseViewProj = XMMatrixInverse(nullptr, camera.ViewProjMatrix);
}

} // namespace Renderer
} // namespace DX12Engine