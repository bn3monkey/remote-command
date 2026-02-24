#if !defined (__REMOTE_COMMAND_SERVER_PROCESS__)
#define __REMOTE_COMMAND_SERVER_PROCESS__

#include "remote_command_server_socket.hpp"

#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace Bn3Monkey
{
    class RemoteProcess
    {
    public:
        RemoteProcess() {}
        ~RemoteProcess();

        // Synchronously creates the process (returns -1 on failure, 1 on success).
        // stdout/stderr reader threads start immediately and forward pipe output
        // to the current stream socket.
        // Returns -1 if a process is already running.
        int32_t execute(const char* cwd, const char* cmd);

        // Blocks until the process finishes and all output has been flushed.
        void await(int32_t process_id);

        // Kills the process, then blocks until all threads are joined.
        void close(int32_t process_id);

        inline bool is_running() const { return _current_process_id != -1; }

        // Called by StreamServer when a stream client connects / disconnects.
        // Atomically replaces the current socket and returns the old one so the
        // caller can close it.  Pass INVALID_SOCK to clear.
        sock_t setStreamSocket(sock_t sock);

    private:
        void stdoutReader();
        void stderrReader();
        void joinReaders();     // joins _stdout_reader, _stderr_reader
        void reapProcess();     // WaitForSingleObject/waitpid + handle cleanup
        void closePipes();      // closes platform pipe read-handles

        // _stream_mtx guards both _stream_sock (for setStreamSocket) and
        // concurrent sendStream calls from the two reader threads.
        sock_t     _stream_sock { INVALID_SOCK };
        std::mutex _stream_mtx;

        std::thread _stdout_reader;
        std::thread _stderr_reader;

        std::atomic<int32_t> _current_process_id { -1 };

#ifdef _WIN32
        HANDLE _hProcess    { INVALID_HANDLE_VALUE };
        HANDLE _stdin_write { INVALID_HANDLE_VALUE };
        HANDLE _stdout_read { INVALID_HANDLE_VALUE };
        HANDLE _stderr_read { INVALID_HANDLE_VALUE };
#else
        pid_t _pid         { -1 };
        int   _stdout_read { -1 };
        int   _stderr_read { -1 };
#endif
    };
}

#endif // __REMOTE_COMMAND_SERVER_PROCESS__