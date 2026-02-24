#if !defined(__BN3MONKEY_REMOTE_COMMAND_SERVER__)
#define __BN3MONKEY_REMOTE_COMMAND_SERVER__

#include <cstdio>
#include <cstdint>

namespace Bn3Monkey
{
    struct RemoteCommandServer;

    RemoteCommandServer* openRemoteCommandServer(int32_t discovery_port, int32_t command_port, int32_t stream_port, const char* current_working_directory = ".");
    void closeRemoteCommandServer(RemoteCommandServer* server);
}

#endif // __BN3MONKEY_REMOTE_COMMAND_SERVER__