#if !defined(__BN3MONKEY_REMOTE_COMMAND_CLIENT__)
#define __BN3MONKEY_REMOTE_COMMAND_CLIENT__

#include <cstdio>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Portable printf-format checking macros
//
// RC_PRINTF_FUNC(fmt, first) – function-level attribute that instructs the
//   compiler to verify format strings at every call site.
//   fmt  = 1-based index of the format-string parameter
//   first= 1-based index of the first variadic argument
//   Supported: GCC, Clang (Android, Linux, MinGW, macOS, …)
//
// RC_PRINTF_STR – parameter annotation placed directly before the format-
//   string argument.  Enables /analyze checks on MSVC.  Empty elsewhere.
//
// RC_PUSH_NO_FMT / RC_POP_NO_FMT – suppress "format string is not a string
//   literal" inside wrapper bodies where the format has already been
//   validated at the call site by RC_PRINTF_FUNC / RC_PRINTF_STR.
// ---------------------------------------------------------------------------
#if defined(__clang__) || defined(__GNUC__)
    #define RC_PRINTF_FUNC(fmt, first) \
        __attribute__((format(printf, fmt, first)))
    #define RC_PRINTF_STR
    #define RC_PUSH_NO_FMT \
        _Pragma("GCC diagnostic push") \
        _Pragma("GCC diagnostic ignored \"-Wformat-security\"")
    #define RC_POP_NO_FMT \
        _Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER)
    #include <sal.h>
    #define RC_PRINTF_FUNC(fmt, first)
    #define RC_PRINTF_STR _Printf_format_string_
    #define RC_PUSH_NO_FMT \
        __pragma(warning(push)) \
        __pragma(warning(disable: 4774))
    #define RC_POP_NO_FMT \
        __pragma(warning(pop))
#else
    #define RC_PRINTF_FUNC(fmt, first)
    #define RC_PRINTF_STR
    #define RC_PUSH_NO_FMT
    #define RC_POP_NO_FMT
#endif

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

    RemoteCommandClient* discoverRemoteCommandClient(int32_t discovery_port);
    RemoteCommandClient* createRemoteCommandClient(int32_t command_port, int32_t stream_port, const char* ip = "127.0.0.1");
    void releaseRemoteCommandClient(RemoteCommandClient* client);

    const char* getRemoteCommandServerAddress(RemoteCommandClient* client);

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
    RC_PRINTF_FUNC(2, 3)
    void runCommand(RemoteCommandClient* client, RC_PRINTF_STR const char* fmt, Args... args) {
        char buffer[4096] {0};
        RC_PUSH_NO_FMT
        snprintf(buffer, sizeof(buffer), fmt, args...);
        RC_POP_NO_FMT
        runCommandImpl(client, buffer);
    }

    int32_t openProcessImpl(RemoteCommandClient* client, const char* cmd);
    template<typename ...Args>
    RC_PRINTF_FUNC(2, 3)
    int32_t openProcess(RemoteCommandClient* client, RC_PRINTF_STR const char* fmt, Args... args)
    {
        char buffer[4096] {0};
        RC_PUSH_NO_FMT
        snprintf(buffer, sizeof(buffer), fmt, args...);
        RC_POP_NO_FMT
        return openProcessImpl(client, buffer);
    }

    void closeProcess(RemoteCommandClient* client, int32_t process_id);
}

#endif // __BN3MONKEY_REMOTE_COMMAND_CLIENT__