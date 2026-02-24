#include "remote_command_server_discovery.hpp"
#include "remote_command_server_helper.hpp"

using namespace Bn3Monkey;

static void handleDiscoverMessage(KiottyDiscoveryServer* server, std::atomic<bool>& is_running)
{
    setCurrentThreadName("RC_DISCOV");

    do {
        while (auto* message = KiottyDiscoveryServer_awaitMessage(server, 50000))
        {
            auto* str = KiottyDiscoveryServer_getMessage(message);
            printf("[Discovery] %s\n", str);
        }
    } while (is_running);
}


bool DiscoveryServer::open(int32_t discovery_port, int32_t command_port, int32_t stream_port)
{
    _server = KiottyDiscoveryServer_createServer(discovery_port);
    if (!_server)
        return false;

    KiottyDiscoveryServer_addPort(_server, command_port, PORT_COMMAND);
    KiottyDiscoveryServer_addPort(_server, stream_port, PORT_STREAM);
    KiottyDiscoveryServer_openServer(_server);

    _running.store(true);
    _message_thread = std::thread(handleDiscoverMessage, _server, std::ref(_running));
    return true;
}

void DiscoveryServer::close()
{
    if (_server) {
        _running.store(false);
        KiottyDiscoveryServer_cancelMessage(_server);

        if (_message_thread.joinable())
            _message_thread.join();

        KiottyDiscoveryServer_releaseServer(_server);
        _server = nullptr;
    }
}
