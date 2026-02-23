#if !defined(__BN3MONKEY_REMOTE_COMMAND_CLIENT__)
#define __BN3MONKEY_REMOTE_COMMAND_CLIENT__

#include <cstdio>
#include <cstdint>
#include <vector>

namespace Bn3Monkey
{
    struct RemoteCommandClient;

    enum class RemoteDirectoryContentType
    {
        FILE,
        DIRECTORY
    };
    struct RemoteDirectoryContent
    {
        RemoteDirectoryContentType type;
        char name[128] {0};
    };

    RemoteCommandClient* createRemoteCommandContext(int32_t command_port, int32_t stream_port, const char* ip = "127.0.0.1");
    void releaseRemoteCommandContext(RemoteCommandClient* client);

    using OnRemoteOutput = void (*)(const char*);
    void onRemoteOutput(RemoteCommandClient* client, OnRemoteOutput on_remote_output);
    using OnRemoteError = void (*)(const char*);
    void onRemoteError(RemoteCommandClient* client, OnRemoteError on_remote_error);

    const char* currentWorkingDirectory(RemoteCommandClient* client);
    bool moveWorkingDirectory(RemoteCommandClient* client, const char* path);
    bool directoryExists(RemoteCommandClient* client, const char* path);
    std::vector<RemoteDirectoryContent> listDirectoryContents(RemoteCommandClient* client, const char* path = ".");

    bool createDirectory(RemoteCommandClient* client, const char* path);
    bool removeDirectory(RemoteCommandClient* client, const char* path);
    bool copyDirectory(RemoteCommandClient* client, const char* from_path, const char* to_path);
    bool moveDirectory(RemoteCommandClient* client, const char* from_path, const char* to_path);

    bool uploadFile(RemoteCommandClient* client, const char* local_file, const char* remote_file);
    bool downloadFile(RemoteCommandClient* client, const char* local_file, const char* remote_file);

    void runCommandImpl(RemoteCommandClient* client, const char* cmd);

    template<typename ...Args>
    void runCommand(RemoteCommandClient* client, const char* fmt, Args... args) {
        char buffer[4096] {0};
        snprintf(buffer, sizeof(buffer), fmt, args...);
        runCommandImpl(client, buffer);
    }

}

#endif // __BN3MONKEY_REMOTE_COMMAND_CLIENT__