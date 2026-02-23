#include <remote_command_server.hpp>

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <atomic>

static std::atomic<bool>              g_stop{false};
static Bn3Monkey::RemoteCommandServer* g_server{nullptr};

static void on_signal(int)
{
    g_stop.store(true);
    // Closing the server interrupts the blocking accept / recv calls
    if (g_server) {
        Bn3Monkey::closeRemoteCommandServer(g_server);
        g_server = nullptr;
    }
}

int main(int argc, char* argv[])
{
    int         command_port = (argc > 1) ? std::atoi(argv[1]) : 9001;
    int         stream_port  = (argc > 2) ? std::atoi(argv[2]) : 9002;
    const char* cwd          = (argc > 3) ? argv[3]            : ".";

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    std::printf("Remote Command Server\n");
    std::printf("  Command port : %d\n", command_port);
    std::printf("  Stream  port : %d\n", stream_port);
    std::printf("  Working dir  : %s\n", cwd);
    std::printf("Press Ctrl+C to stop.\n\n");

    while (!g_stop.load()) {
        std::printf("Waiting for client connection...\n");

        g_server = Bn3Monkey::openRemoteCommandServer(command_port, stream_port, cwd);
        if (!g_server) {
            if (!g_stop.load())
                std::fprintf(stderr, "Failed to accept client connection.\n");
            break;
        }

        std::printf("Client connected. Serving...\n");

        // closeRemoteCommandServer joins the handler thread,
        // so it blocks here until the client disconnects (or signal fires).
        Bn3Monkey::closeRemoteCommandServer(g_server);
        g_server = nullptr;

        std::printf("Client disconnected.\n\n");
    }

    std::printf("Server stopped.\n");
    return 0;
}
