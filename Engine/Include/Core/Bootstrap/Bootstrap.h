#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "Core/Config/ConfigManager.h"
#include "Core/Window/Window.h"

namespace DX12Engine {
namespace Core {

class Bootstrap {
public:
    Bootstrap() = default;
    ~Bootstrap() = default;

    // 禁止拷贝和移动
    Bootstrap(const Bootstrap &) = delete;
    Bootstrap &operator=(const Bootstrap &) = delete;
    Bootstrap(Bootstrap &&) = delete;
    Bootstrap &operator=(Bootstrap &&) = delete;

    /**
     * @brief 初始化启动模块
     * @param configDir 配置目录路径 (如 "Config")
     * @return bool 初始化是否成功
     */
    bool Initialize(const std::filesystem::path &configDir);

    /**
     * @brief 运行主循环
     * @note 会阻塞直到窗口关闭
     */
    void Run();

    /**
     * @brief 关闭并清理
     */
    void Shutdown();

private:
    /**
     * @brief 获取窗口描述符
     * @return Window::Desc 从配置中读取的窗口配置
     */
    Window::Desc GetWindowDesc() const;

private:
    std::unique_ptr<ConfigManager> m_configManager;
    std::unique_ptr<Window> m_window;
    bool m_isRunning = false;
};

} // namespace Core
} // namespace DX12Engine
