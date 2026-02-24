#if !defined (__REMOTE_COMMAND_SERVER_DISCOVERY__)
#define __REMOTE_COMMAND_SERVER_DISCOVERY__

#include <kiotty_discovery_server.hpp>
#include <thread>
#include <atomic>

#include "../protocol/remote_command_protocol.hpp"

namespace Bn3Monkey
{
    class DiscoveryServer
    {
    public:
        DiscoveryServer() {}
        virtual ~DiscoveryServer() { close(); }
        bool open(int32_t discovery_port, int32_t command_port, int32_t stream_port);
        void close();

    private:
        KiottyDiscoveryServer* _server{ nullptr };
        std::thread _message_thread;
        std::atomic<bool> _running{ false };
    };
}

#endif // __REMOTE_COMMAND_SERVER_DISCOVERY__