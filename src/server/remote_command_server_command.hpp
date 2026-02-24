#if !defined (__REMOTE_COMMAND_SERVER_COMMAND__)
#define __REMOTE_COMMAND_SERVER_COMMAND__

#include "remote_command_server_process.hpp"
#include "remote_command_server_socket.hpp"
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>

namespace Bn3Monkey
{
    class CommandServer
    {
    public:
        explicit CommandServer(RemoteProcess& remote_process)
            : _remote_process(remote_process) {}
        ~CommandServer() { close(); }

        // initial_cwd: starting working directory (CommandServer owns it)
        bool open(int32_t command_port, const char* initial_cwd);
        void close();

    private:
        void handlerLoop();
        void handleCommand(sock_t client_sock);

        RemoteProcess&    _remote_process;
        std::string       _current_directory;
        sock_t            _server_sock  { INVALID_SOCK };
        sock_t            _client_sock  { INVALID_SOCK };  // interrupted on close()
        std::atomic<bool> _running      { false };
        std::thread       _handler;
    };
}

#endif // __REMOTE_COMMAND_SERVER_COMMAND__
