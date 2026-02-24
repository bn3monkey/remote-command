#if !defined(__REMOTE_COMMAND_SERVER_STREAM__)
#define __REMOTE_COMMAND_SERVER_STREAM__

#include "remote_command_server_process.hpp"
#include "remote_command_server_socket.hpp"
#include <thread>
#include <atomic>

namespace Bn3Monkey
{
    class StreamServer
    {
    public:
        explicit StreamServer(RemoteProcess& remote_process)
            : _remote_process(remote_process) {}
        ~StreamServer() { close(); }

        bool open(int32_t stream_port);
        void close();

    private:
        void acceptLoop();

        RemoteProcess&    _remote_process;
        sock_t            _server_sock { INVALID_SOCK };
        std::atomic<bool> _running     { false };
        std::thread       _accepter;
    };
}

#endif // __REMOTE_COMMAND_SERVER_STREAM__
