#if !defined (__REMOTE_COMMAND_SERVER_CONTEXT__)
#define __REMOTE_COMMAND_SERVER_CONTEXT__

#include "remote_command_server_discovery.hpp"
#include "remote_command_server_process.hpp"
#include "remote_command_server_stream.hpp"
#include "remote_command_server_command.hpp"

namespace Bn3Monkey
{
    struct RemoteCommandServer
    {
        RemoteProcess   process;
        StreamServer    stream_server  { process };
        CommandServer   command_server { process };
        DiscoveryServer discovery_server;
    };
}

#endif // __REMOTE_COMMAND_SERVER_CONTEXT__
