#include <remote_command_server.hpp>

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

static std::atomic<bool>               g_stop{false};
static Bn3Monkey::RemoteCommandServer* g_server{nullptr};

static void on_signal(int)
{
    g_stop.store(true);
}

int main(int argc, char* argv[])
{
    int         discovery_port = (argc > 1) ? std::atoi(argv[1]) : 9000;
    int         command_port   = (argc > 2) ? std::atoi(argv[2]) : 9001;
    int         stream_port    = (argc > 3) ? std::atoi(argv[3]) : 9002;
    const char* cwd            = (argc > 4) ? argv[4]            : ".";

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    std::printf("Remote Command Server\n");
    std::printf("  Discovery port : %d\n", discovery_port);
    std::printf("  Command port   : %d\n", command_port);
    std::printf("  Stream  port   : %d\n", stream_port);
    std::printf("  Working dir    : %s\n", cwd);
    std::printf("Press Ctrl+C to stop.\n\n");

    // openRemoteCommandServer returns immediately.
    // Client accept / serve / reconnect loop runs in the background thread.
    g_server = Bn3Monkey::openRemoteCommandServer(discovery_port, command_port, stream_port, cwd);
    if (!g_server) {
        std::fprintf(stderr, "Failed to start server.\n");
        return 1;
    }

    std::printf("Server started. Waiting for connections...\n");

    // Sleep until Ctrl+C / SIGTERM
    while (!g_stop.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::printf("\nStopping server...\n");
    Bn3Monkey::closeRemoteCommandServer(g_server);
    g_server = nullptr;

    std::printf("Server stopped.\n");
    return 0;
}
