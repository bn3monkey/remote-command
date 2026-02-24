#include "../../include/remote_command_server.hpp"
#include "remote_command_server_context.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#endif

namespace Bn3Monkey
{
    RemoteCommandServer* openRemoteCommandServer(
        int32_t     discovery_port,
        int32_t     command_port,
        int32_t     stream_port,
        const char* current_working_directory)
    {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return nullptr;
#endif

        auto* server = new RemoteCommandServer();

        if (!server->stream_server.open(stream_port)) {
            delete server;
#ifdef _WIN32
            WSACleanup();
#endif
            return nullptr;
        }

        if (!server->command_server.open(command_port, current_working_directory)) {
            server->stream_server.close();
            delete server;
#ifdef _WIN32
            WSACleanup();
#endif
            return nullptr;
        }

        if (!server->discovery_server.open(discovery_port, command_port, stream_port)) {
            server->command_server.close();
            server->stream_server.close();
            delete server;
#ifdef _WIN32
            WSACleanup();
#endif
            return nullptr;
        }

        return server;
    }

    void closeRemoteCommandServer(RemoteCommandServer* server)
    {
        if (!server) return;

        server->discovery_server.close();
        server->command_server.close();
        server->stream_server.close();

        delete server;

#ifdef _WIN32
        WSACleanup();
#endif
    }

} // namespace Bn3Monkey
