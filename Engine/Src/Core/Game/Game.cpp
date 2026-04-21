#include "Core/Game/Game.h"

#include "Core/Context/GameContext.h"
#include "Core/Logger/Logger.h"
#include "Core/Window/Window.h"

namespace DX12Engine {
namespace Core {

Game::Game(GameContext *context) : m_context(context), m_isRunning(false), m_isInitialized(false) {}

Game::~Game() {
    if (m_isRunning || m_isInitialized) {
        Shutdown();
    }
}

bool Game::Initialize() {
    // 验证 Context
    if (!m_context || !m_context->IsValid()) {
        m_context->Logging->Error("[Game] Failed to initialize: %s",
                                  m_context ? m_context->GetInvalidReason() : "Context is null");
        return false;
    }

    m_context->Logging->Info("[Game] Initializing game...");

    // 初始化游戏模块
    InitializeGameModules();

    m_isInitialized = true;
    m_context->Logging->Info("[Game] Game initialized successfully");

    return true;
}

int Game::Run() {
    if (!m_isInitialized) {
        m_context->Logging->Error("[Game] Cannot run: game is not initialized");
        return -1;
    }

    m_context->Logging->Info("[Game] Starting game loop...");
    m_isRunning = true;
    m_lastFrameTime = std::chrono::steady_clock::now();

    // 主循环就绪后才显示窗口
    m_context->Window->Show();

    // 主循环
    while (m_isRunning && !m_context->Window->ShouldClose()) {
        // 处理 Windows 消息
        m_context->Window->ProcessMessages();

        // 计算帧时间
        m_deltaTime = CalculateDeltaTime();

        // 更新逻辑
        Update(m_deltaTime);

        // 渲染画面
        Render();
    }

    m_context->Logging->Info("[Game] Game loop ended");
    Shutdown();

    return 0;
}

void Game::Shutdown() {
    if (!m_isInitialized && !m_isRunning) {
        return;
    }

    m_context->Logging->Info("[Game] Shutting down game...");

    // 清理游戏模块
    ShutdownGameModules();

    m_isRunning = false;
    m_isInitialized = false;

    m_context->Logging->Info("[Game] Game shutdown complete");
}

void Game::Update(float deltaTime) {
    // TODO: 更新游戏逻辑模块
    // m_inputManager->Update(deltaTime);
    // m_playerManager->Update(deltaTime);
    // m_levelManager->Update(deltaTime);
    // m_audioManager->Update(deltaTime);
}

void Game::Render() {
    // TODO: 渲染游戏画面
    // m_context->Renderer->Clear();
    // m_levelManager->Render();
    // m_playerManager->Render();
    // m_context->Renderer->Present();
}

void Game::InitializeGameModules() {
    m_context->Logging->Info("[Game] Initializing game modules...");

    // TODO: 初始化游戏逻辑模块
    // m_inputManager = std::make_unique<InputManager>(m_context);
    // m_playerManager = std::make_unique<PlayerManager>(m_context);
    // m_levelManager = std::make_unique<LevelManager>(m_context);
    // m_assetManager = std::make_unique<AssetManager>(m_context);
    // m_audioManager = std::make_unique<AudioManager>(m_context);

    m_context->Logging->Info("[Game] Game modules initialized");
}

void Game::ShutdownGameModules() {
    m_context->Logging->Info("[Game] Shutting down game modules...");

    // TODO: 清理游戏逻辑模块
    // m_audioManager.reset();
    // m_assetManager.reset();
    // m_levelManager.reset();
    // m_playerManager.reset();
    // m_inputManager.reset();

    m_context->Logging->Info("[Game] Game modules shutdown complete");
}

float Game::CalculateDeltaTime() {
    auto currentTime = std::chrono::steady_clock::now();
    std::chrono::duration<float> elapsed = currentTime - m_lastFrameTime;
    m_lastFrameTime = currentTime;

    // 限制最大帧时间（防止卡顿后的跳帧）
    float delta = elapsed.count();
    if (delta > 0.1f) {
        delta = 0.1f;
    }

    return delta;
}

} // namespace Core
} // namespace DX12Engine
