// Runtime/NetworkTest.cpp
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

#include "Network/Implementation/NetworkTransportGNS.h"
#include "Network/struct/NetworkConfig.h"

// GameNetworkingSockets 全局初始化的头文件
#include <steam/steamnetworkingsockets.h>

using namespace DX12Engine::Network;

// 全局控制
std::atomic<bool> g_running{true};

// 命令行参数解析
struct CmdLineArgs {
    uint16_t port = 7777;
    std::string connectTo;
    bool help = false;
};

CmdLineArgs ParseCommandLine(int argc, char *argv[]) {
    CmdLineArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            args.help = true;
        } else if (arg == "--port" && i + 1 < argc) {
            args.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--connect" && i + 1 < argc) {
            args.connectTo = argv[++i];
        }
    }
    return args;
}

void PrintUsage(const char *program) {
    std::cout << "Usage: " << program << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --port <port>     Set listen port (default: 7777)" << std::endl;
    std::cout << "  --connect <addr>  Connect to specified address (e.g., 127.0.0.1:7777)" << std::endl;
    std::cout << "  --help, -h        Show this help" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  Start server: " << program << " --port 7777" << std::endl;
    std::cout << "  Connect to server: " << program << " --port 7778 --connect 127.0.0.1:7777" << std::endl;
}

// 回调打印
void OnConnected(PlayerId id) { std::cout << "[TEST] Connected to peer: " << id << std::endl; }

void OnConnectFailed(const std::string &addr, uint16_t port, const std::string &reason) {
    std::cout << "[TEST] Connect failed to " << addr << ":" << port << " - " << reason << std::endl;
}

void OnDisconnected(PlayerId id, const std::string &reason) {
    std::cout << "[TEST] Disconnected from " << id << ": " << reason << std::endl;
}

void OnDataReceived(PlayerId sender, const uint8_t *data, size_t size) {
    std::cout << "[TEST] Received " << size << " bytes from " << sender << ": ";
    std::cout.write((const char *)data, size);
    std::cout << std::endl;
}

bool OnConnectionRequest(PlayerId id, const std::string &addr, uint16_t port) {
    std::cout << "[TEST] Connection request from " << addr << ":" << port << " - accepting" << std::endl;
    // 返回 true 接受连接（当前实现中 callback 返回值未使用，需要在 NetworkTransportGNS 中修改）

    return true;
}

void OnError(uint32_t code, const std::string &msg) {
    std::cout << "[TEST] Error " << code << ": " << msg << std::endl;
}

// 发送测试消息的线程
void SendLoop(NetworkTransportGNS &transport, PlayerId targetId, int intervalMs) {
    int counter = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));

        std::string msg = "Hello " + std::to_string(++counter);
        bool success = transport.Send(targetId, (const uint8_t *)msg.c_str(), msg.size(), DeliveryMethod::Reliable);

        if (success) {
            std::cout << "[TEST] Sent to " << targetId << ": " << msg << std::endl;
        } else {
            std::cout << "[TEST] Failed to send to " << targetId << std::endl;
        }
    }
}

// 初始化 GNS（全局一次）
void InitGNS() {
    SteamNetworkingErrMsg errMsg;
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        std::cerr << "Failed to init GNS: " << errMsg << std::endl;
        exit(1);
    }
    std::cout << "[TEST] GNS initialized" << std::endl;
}

void ShutdownGNS() {
    GameNetworkingSockets_Kill();
    std::cout << "[TEST] GNS shutdown" << std::endl;
}

// 独立进程 P2P 测试
void TestStandaloneP2P(uint16_t port, const std::string &connectTo) {
    std::cout << "\n========== Standalone P2P Test ==========" << std::endl;

    NetworkTransportGNS transport;

    transport.SetOnConnected(OnConnected);
    transport.SetOnConnectFailed(OnConnectFailed);
    transport.SetOnDisconnected(OnDisconnected);
    transport.SetOnDataReceived(OnDataReceived);
    transport.SetOnConnectionRequest(OnConnectionRequest);
    transport.SetOnError(OnError);

    // 启动 P2P 模式
    NetworkConfig config = NetworkConfig::MakeP2P(port);
    config.localPlayerId = port; // 使用端口号作为 PlayerId，便于区分
    if (!transport.Start(config)) {
        std::cerr << "Failed to start transport on port " << port << std::endl;
        return;
    }

    std::cout << "[TEST] Transport started on port " << port << ", PlayerId=" << transport.GetLocalPlayerId()
              << std::endl;

    // 如果指定了连接目标，尝试连接
    if (!connectTo.empty()) {
        std::string addr = connectTo;
        uint16_t targetPort = 7777;
        size_t colonPos = connectTo.find(':');
        if (colonPos != std::string::npos) {
            addr = connectTo.substr(0, colonPos);
            targetPort = static_cast<uint16_t>(std::stoi(connectTo.substr(colonPos + 1)));
        }
        std::cout << "[TEST] Connecting to " << addr << ":" << targetPort << "..." << std::endl;
        transport.Connect(addr, targetPort);
    }

    // 启动发送线程
    std::thread sendThread([&]() {
        int counter = 0;
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            auto players = transport.GetConnectedPlayers();
            if (!players.empty()) {
                std::string msg = "Msg from port " + std::to_string(port) + " #" + std::to_string(++counter);
                transport.Send(players[0], (const uint8_t *)msg.c_str(), msg.size(), DeliveryMethod::Reliable);
                std::cout << "[TEST] Sent: " << msg << std::endl;
            }
        }
    });

    // 主循环
    std::cout << "[TEST] Running main loop. Press Ctrl+C to exit..." << std::endl;
    while (g_running) {
        transport.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    sendThread.join();
    transport.Stop();

    std::cout << "[TEST] Standalone P2P test completed" << std::endl;
}

// 测试 Client-Server 模式
void TestClientServer() {
    std::cout << "\n========== Client-Server Test ==========" << std::endl;

    // 服务器
    NetworkTransportGNS server;
    server.SetOnConnected(OnConnected);
    server.SetOnDisconnected(OnDisconnected);
    server.SetOnDataReceived(OnDataReceived);
    server.SetOnConnectionRequest(OnConnectionRequest);
    server.SetOnError(OnError);

    // 客户端
    NetworkTransportGNS client;
    client.SetOnConnected(OnConnected);
    client.SetOnConnectFailed(OnConnectFailed);
    client.SetOnDisconnected(OnDisconnected);
    client.SetOnDataReceived(OnDataReceived);
    client.SetOnError(OnError);

    // 启动服务器
    NetworkConfig serverConfig = NetworkConfig::MakeServer(8888);
    serverConfig.localPlayerId = 100;
    if (!server.Start(serverConfig)) {
        std::cerr << "Server start failed" << std::endl;
        return;
    }
    std::cout << "[TEST] Server started on port 8888, PlayerId=" << server.GetLocalPlayerId() << std::endl;

    // 启动客户端
    NetworkConfig clientConfig = NetworkConfig::MakeClient("127.0.0.1", 8888);
    clientConfig.localPlayerId = 1;
    if (!client.Start(clientConfig)) {
        std::cerr << "Client start failed" << std::endl;
        return;
    }
    std::cout << "[TEST] Client started, PlayerId=" << client.GetLocalPlayerId() << std::endl;

    // 客户端连接服务器
    std::cout << "[TEST] Client connecting to server..." << std::endl;
    client.Connect("127.0.0.1", 8888);

    // 运行等待连接
    for (int i = 0; i < 30 && g_running; ++i) {
        server.Update();
        client.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 客户端发送消息
    auto clientPlayers = client.GetConnectedPlayers();
    if (!clientPlayers.empty()) {
        std::string msg = "Hello from client!";
        std::cout << "[TEST] Client sending to server..." << std::endl;
        client.Send(clientPlayers[0], (const uint8_t *)msg.c_str(), msg.size(), DeliveryMethod::Reliable);
    }

    // 继续运行接收消息
    for (int i = 0; i < 20 && g_running; ++i) {
        server.Update();
        client.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 停止
    client.Stop();
    server.Stop();

    std::cout << "[TEST] Client-Server test completed" << std::endl;
}

int main(int argc, char *argv[]) {
    std::cout << "=== Network Transport Test ===" << std::endl;

    // 解析命令行参数
    CmdLineArgs args = ParseCommandLine(argc, argv);

    if (args.help) {
        PrintUsage(argv[0]);
        return 0;
    }

    // 全局初始化 GNS
    InitGNS();

    // 注册 Ctrl+C 信号处理
#ifdef _WIN32
    // Windows 信号处理
    auto handler = [](DWORD ctrlType) {
        if (ctrlType == CTRL_C_EVENT) {
            g_running = false;
            std::cout << "\n[TEST] Received Ctrl+C, stopping..." << std::endl;
            return TRUE;
        }
        return FALSE;
    };
    SetConsoleCtrlHandler((PHANDLER_ROUTINE)handler, TRUE);
#else
    // Unix 信号处理
    signal(SIGINT, [](int) {
        g_running = false;
        std::cout << "\n[TEST] Received SIGINT, stopping..." << std::endl;
    });
#endif

    // 运行独立进程测试
    TestStandaloneP2P(args.port, args.connectTo);

    // 全局关闭
    ShutdownGNS();

    return 0;
}