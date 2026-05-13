#include "Game.h"
#include "Common/d3dUtil.h"
#include "Core/Context/GameContext.h"
#include "Renderer/Core/D3D12DeviceContext.h"
#include "System/ECS/Registry.h"
#include "System/Event/MessageDispatcher.h"
#include "System/Framework/SystemRegistry.h"
#include "System/Scheduler/FrameDriver.h"
#include "System/Window/Window.h"

using namespace DX12Engine;
using namespace DX12Engine::Scheduler;
using namespace DX12Engine::ECS;

Game::Game(Core::GameContext *context) : m_context(context), m_isRunning(false), m_isInitialized(false) {}

Game::~Game() {
    if (m_isRunning || m_isInitialized) {
        Shutdown();
    }
}

bool Game::Initialize() {
    if (!m_context || !m_context->IsValid()) {
        m_context->Logging->Error("[Game] Failed to initialize: %s",
                                  m_context ? m_context->GetInvalidReason() : "Context is null");
        return false;
    }

    m_context->Logging->Info("[Game] Initializing game...");
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

    if (!m_context->FrameDriver) {
        m_context->Logging->Error("[Game] Cannot run: FrameDriver is not set");
        return -1;
    }

    m_context->Logging->Info("[Game] Starting game loop with FrameDriver...");
    m_isRunning = true;
    m_context->MainTimer->Reset();
    m_context->Window->Show();

    // 初始化 FrameDriver
    m_context->FrameDriver->Initialize();

    while (m_isRunning && !m_context->Window->ShouldClose()) {
        m_context->Window->ProcessMessages();

        // 调用 FrameDriver::Tick() 来处理消息和执行注册的 Systems
        m_context->FrameDriver->Tick();
    }

    // 停止 FrameDriver
    m_context->FrameDriver->Stop();

    m_context->Logging->Info("[Game] Game loop ended");
    Shutdown();
    return 0;
}

void Game::Shutdown() {
    if (!m_isInitialized && !m_isRunning) {
        return;
    }

    m_context->Logging->Info("[Game] Shutting down game...");
    ShutdownGameModules();

    m_isRunning = false;
    m_isInitialized = false;
    m_context->Logging->Info("[Game] Game shutdown complete");
}

void Game::Update(float deltaTime) {
    // 此方法保留给纯逻辑更新（如物理、动画等）
}

void Game::InitializeGameModules() {
    m_context->Logging->Info("[Game] Initializing game modules...");

    // ─────────────────────────────────────────────────
    // L4: 注册消息驱动的 Systems（利用调度层能力）
    // ─────────────────────────────────────────────────

    // WindowResizeSystem - 处理窗口大小变化 (主线程执行)
    SystemRegistry::Register(
        {.name = "WindowResizeSystem",
         .func =
             [this](Registry &, const MessageContext &ctx) {
                 // 首先输出到调试器
                 char dbgBuf[256];
                 sprintf_s(dbgBuf, "[WindowResizeSystem] Executed! Width=%u Height=%u Payload=0x%llX\n", ctx.GetLow32(),
                           ctx.GetHigh32(), (unsigned long long)ctx.payload);
                 ::OutputDebugStringA(dbgBuf);

                 // 尝试 spdlog 输出
                 if (m_context && m_context->Logging) {
                     m_context->Logging->Info("[WindowResizeSystem] spdlog: {}x{}", ctx.GetLow32(), ctx.GetHigh32());
                 } else {
                     ::OutputDebugStringA("[WindowResizeSystem] WARNING: m_context or Logging is null!\n");
                 }

                 // DX12 resize
                 if (m_context && m_context->DeviceContext) {
                     m_context->DeviceContext->OnResize(ctx.GetLow32(), ctx.GetHigh32());
                 }
             },
         .phase = TaskPhase::EarlyUpdate,
         .threadType = ThreadType::Main, // 主线程执行
         .interestedMessages = {System::Event::WindowResizeEvent::StaticTypeHash}});

    // 验证注册
    auto allSystems = SystemRegistry::GetAllSystems();
    wchar_t buf[128];
    swprintf_s(buf, L"[Game] Total systems registered: %zu", allSystems.size());
    ::OutputDebugStringW(buf);

    auto interested = SystemRegistry::GetInterestedSystems(System::Event::WindowResizeEvent::StaticTypeHash);
    swprintf_s(buf, L"[Game] Systems interested in WindowResizeEvent: %zu", interested.size());
    ::OutputDebugStringW(buf);

    m_context->Logging->Info("[Game] Game modules initialized");
}

void Game::ShutdownGameModules() {
    m_context->Logging->Info("[Game] Shutting down game modules...");

    SystemRegistry::Clear();

    m_context->Logging->Info("[Game] Game modules shutdown complete");
}
