// Runtime/NetworkP2PTest.cpp
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "Network/INetworkTransport.h"
#include "Network/Implementation/NetworkTransportGNS.h"
#include "Network/NetworkTopologyP2P.h"

#include <steam/steamnetworkingsockets.h>

using namespace DX12Engine::Network;

std::atomic<bool> g_running{true};

void PrintHelp() {
    std::cout << "Usage: NetworkP2PTest.exe [--port <port>] [--connect <ip:port>]" << std::endl;
    std::cout << "  --port <port>      Local port (default: 7777)" << std::endl;
    std::cout << "  --connect <addr>   Connect to peer (optional)" << std::endl;
    std::cout << "  --help             Show this help" << std::endl;
}

class P2PTestGame {
public:
    void Initialize(P2PTopology *topology) {
        m_topology = topology;
        m_localPlayerId = topology->GetLocalPlayerId();

        topology->SetOnPlayerJoined(
            [](PlayerId playerId) { std::cout << "[Game] Player " << playerId << " joined" << std::endl; });
        topology->SetOnPlayerLeft(
            [](PlayerId playerId) { std::cout << "[Game] Player " << playerId << " left" << std::endl; });
        topology->SetOnGameMessage([](PlayerId senderId, const uint8_t *data, size_t size) {
            std::cout << "[Game] Received message from " << senderId << " (" << size << " bytes)" << std::endl;
        });
    }

    void Update(float dt) {
        static int counter = 0;
        // 模拟输入：简单的循环数值
        uint32_t inputData = (counter++ / 60) % 10;

        m_topology->SubmitLocalInput(inputData);

        uint32_t currentFrame = m_topology->GetCurrentFrame();
        uint32_t myInput = m_topology->GetPlayerInput(m_localPlayerId, currentFrame);

        static int printCounter = 0;
        if (++printCounter >= 60) {
            printCounter = 0;

            auto players = m_topology->GetPlayers();
            for (auto playerId : players) {
                if (playerId != m_localPlayerId) {
                    uint32_t otherInput = m_topology->GetPlayerInput(playerId, currentFrame);
                    std::cout << "[Game]   Player " << playerId << " input: " << otherInput << std::endl;
                }
            }
        }
    }

private:
    P2PTopology *m_topology = nullptr;
    PlayerId m_localPlayerId = 0;
};

void InitGNS() {
    SteamNetworkingErrMsg errMsg;
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        std::cerr << "Failed to init GNS" << std::endl;
        exit(1);
    }
    std::cout << "[System] GNS initialized" << std::endl;
}

void ShutdownGNS() {
    GameNetworkingSockets_Kill();
    std::cout << "[System] GNS shutdown" << std::endl;
}

int main(int argc, char *argv[]) {
    uint16_t localPort = 7777;
    std::string connectTo;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            localPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            connectTo = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            PrintHelp();
            return 0;
        }
    }

    if (connectTo.empty()) {
        std::cout << "[System] Mode: LISTEN on port " << localPort << std::endl;
    } else {
        std::cout << "[System] Mode: CONNECT on port " << localPort << " -> " << connectTo << std::endl;
    }

    InitGNS();

    NetworkTransportGNS transport;
    transport.SetOnError([](uint32_t code, const std::string &msg) {
        std::cerr << "[Transport] Error: " << code << " - " << msg << std::endl;
    });

    NetworkConfig config = NetworkConfig::MakeP2P(localPort);
    // 端口 7777 使用 PlayerId = 1，端口 7778 使用 PlayerId = 2
    config.localPlayerId = (localPort == 7777) ? 1 : 2;
    if (!transport.Start(config)) {
        std::cerr << "Failed to start transport" << std::endl;
        ShutdownGNS();
        return 1;
    }

    std::cout << "[System] Transport started, PlayerId=" << transport.GetLocalPlayerId() << std::endl;

    // 创建 P2PTopology
    P2PTopology topology;
    if (!topology.Initialize(config, &transport)) {
        std::cerr << "Failed to initialize topology" << std::endl;
        transport.Stop();
        ShutdownGNS();
        return 1;
    }

    P2PTestGame game;
    game.Initialize(&topology);

    // 连接对端
    if (!connectTo.empty()) {
        size_t colonPos = connectTo.find(':');
        if (colonPos != std::string::npos) {
            std::string addr = connectTo.substr(0, colonPos);
            uint16_t port = static_cast<uint16_t>(std::stoi(connectTo.substr(colonPos + 1)));
            std::cout << "[System] Connecting to " << addr << ":" << port << "..." << std::endl;
            transport.Connect(addr, port);
        }
    }

    std::cout << "[System] Running. Press Ctrl+C to exit..." << std::endl;

    auto lastTime = std::chrono::steady_clock::now();
    const float TARGET_DT = 1.0f / 60.0f;

    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();

        if (dt >= TARGET_DT) {
            lastTime = now;
            topology.Update(dt);
            topology.OnFrameSync();
            game.Update(dt);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    topology.Shutdown();
    transport.Stop();
    ShutdownGNS();

    return 0;
}