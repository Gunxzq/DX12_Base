#pragma once

#include "Renderer/Modules/Camera/Camera.h"
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace DX12Engine {
namespace Renderer {

// 前置声明
class D3D12DeviceContext;

/**
 * @brief 相机管理器
 *
 * 职责：
 * 1. 管理唯一的主相机（用于全局渲染，走即时路径）。
 * 2. 管理多个辅助相机（用于小地图、编辑器、过场等）。
 * 3. 负责矩阵计算与 GPU 资源上传。
 *
 * @note 该类不包含输入处理逻辑，输入由 L4 层 System 负责修改 Camera 数据。
 */
class CameraManager {
public:
    // 单例访问
    static CameraManager &GetInstance();

    // 禁止拷贝和移动
    CameraManager(const CameraManager &) = delete;
    CameraManager &operator=(const CameraManager &) = delete;

    /**
     * @brief 初始化管理器
     * @param deviceContext DX12 设备上下文，用于获取命令管理器和帧资源
     */
    void Initialize(D3D12DeviceContext *deviceContext);

    /**
     * @brief 关闭管理器，释放资源
     */
    void Shutdown();

    // =========================================================================
    // 主相机管理 (Main Camera)
    // =========================================================================

    /**
     * @brief 获取主相机的可写引用
     * @return Camera& 主相机实例
     * @note L4 层（如 InputSystem）应通过此接口修改相机的 Position/Rotation
     */
    Camera &GetMainCamera();

    /**
     * @brief 获取主相机的只读引用
     */
    const Camera &GetMainCamera() const;

    /**
     * @brief 更新主相机矩阵
     *
     * 根据当前的 Position 和 Rotation 重新计算 View, Proj, ViewProj 矩阵。
     * @note 此方法应由 FrameDriver::ExecuteImmediate() 在每帧渲染前调用。
     *       计算后的矩阵将通过 Camera::ViewProjMatrix 等成员变量暴露给框架层进行打包上传。
     */
    void UpdateMainCamera();

    // =========================================================================
    // 辅助相机管理 (Auxiliary Cameras)
    // =========================================================================

    /**
     * @brief 创建辅助相机
     * @param name 相机名称（唯一标识）
     * @param templateData 相机初始参数模板
     * @return true 如果创建成功
     */
    bool CreateAuxiliaryCamera(const std::string &name, const Camera &templateData = Camera());

    /**
     * @brief 销毁辅助相机
     * @param name 相机名称
     * @return true 如果销毁成功
     */
    bool DestroyAuxiliaryCamera(const std::string &name);

    /**
     * @brief 获取辅助相机指针
     * @param name 相机名称
     * @return Camera* 如果存在则返回指针，否则返回 nullptr
     */
    Camera *GetAuxiliaryCamera(const std::string &name);

    /**
     * @brief 获取辅助相机指针（常量版）
     */
    const Camera *GetAuxiliaryCamera(const std::string &name) const;

    // =========================================================================
    // 全局状态同步
    // =========================================================================

    /**
     * @brief 当窗口大小改变时调用，批量更新所有相机的宽高比
     * @param width 窗口宽度
     * @param height 窗口高度
     */
    void OnResize(uint32_t width, uint32_t height);

private:
    CameraManager() = default;
    ~CameraManager() = default;

    // 主相机
    Camera m_mainCamera;

    // 辅助相机池
    std::unordered_map<std::string, Camera> m_auxiliaryCameras;

    // DX12 设备上下文引用
    D3D12DeviceContext *m_deviceContext = nullptr;

    /**
     * @brief 内部方法：根据 Camera 数据计算矩阵
     * @param camera 待计算的相机引用
     */
    void CalculateMatrices(Camera &camera);
};

} // namespace Renderer
} // namespace DX12Engine