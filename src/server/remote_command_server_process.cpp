#include "remote_command_server_process.hpp"
#include "remote_command_server_helper.hpp"

#ifdef _WIN32
// windows.h already pulled in via the hpp
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#endif

namespace Bn3Monkey
{
    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    void RemoteProcess::closePipes()
    {
#ifdef _WIN32
        if (_stdout_read != INVALID_HANDLE_VALUE) {
            CloseHandle(_stdout_read);
            _stdout_read = INVALID_HANDLE_VALUE;
        }
        if (_stderr_read != INVALID_HANDLE_VALUE) {
            CloseHandle(_stderr_read);
            _stderr_read = INVALID_HANDLE_VALUE;
        }
#else
        if (_stdout_read != -1) { ::close(_stdout_read); _stdout_read = -1; }
        if (_stderr_read != -1) { ::close(_stderr_read); _stderr_read = -1; }
#endif
    }

    void RemoteProcess::joinAll()
    {
        if (_executor.joinable())      _executor.join();
        if (_stdout_reader.joinable()) _stdout_reader.join();
        if (_stderr_reader.joinable()) _stderr_reader.join();
    }

    // -------------------------------------------------------------------------
    // Destructor
    // -------------------------------------------------------------------------

    RemoteProcess::~RemoteProcess()
    {
        if (_current_process_id != -1)
            close(_current_process_id);
        closePipes();
    }

    // -------------------------------------------------------------------------
    // setStreamSocket
    // -------------------------------------------------------------------------

    sock_t RemoteProcess::setStreamSocket(sock_t sock)
    {
        std::lock_guard<std::mutex> lk(_stream_mtx);
        sock_t old = _stream_sock;
        _stream_sock = sock;
        return old;
    }

    // -------------------------------------------------------------------------
    // Reader threads
    // -------------------------------------------------------------------------

    void RemoteProcess::stdoutReader()
    {
        setCurrentThreadName("RC_OUT");
        char buf[4096];

#ifdef _WIN32
        DWORD bytesRead;
        while (ReadFile(_stdout_read, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            std::lock_guard<std::mutex> lk(_stream_mtx);
            if (_stream_sock != INVALID_SOCK)
                sendStream(_stream_sock, RemoteCommandStreamType::STREAM_OUTPUT, buf, bytesRead);
        }
#else
        ssize_t n;
        while ((n = ::read(_stdout_read, buf, sizeof(buf))) > 0) {
            std::lock_guard<std::mutex> lk(_stream_mtx);
            if (_stream_sock != INVALID_SOCK)
                sendStream(_stream_sock, RemoteCommandStreamType::STREAM_OUTPUT, buf, static_cast<uint32_t>(n));
        }
#endif
    }

    void RemoteProcess::stderrReader()
    {
        setCurrentThreadName("RC_ERR");
        char buf[4096];

#ifdef _WIN32
        DWORD bytesRead;
        while (ReadFile(_stderr_read, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            std::lock_guard<std::mutex> lk(_stream_mtx);
            if (_stream_sock != INVALID_SOCK)
                sendStream(_stream_sock, RemoteCommandStreamType::STREAM_ERROR, buf, bytesRead);
        }
#else
        ssize_t n;
        while ((n = ::read(_stderr_read, buf, sizeof(buf))) > 0) {
            std::lock_guard<std::mutex> lk(_stream_mtx);
            if (_stream_sock != INVALID_SOCK)
                sendStream(_stream_sock, RemoteCommandStreamType::STREAM_ERROR, buf, static_cast<uint32_t>(n));
        }
#endif
    }

    // -------------------------------------------------------------------------
    // execute
    // -------------------------------------------------------------------------

    int32_t RemoteProcess::execute(const char* cwd, const char* cmd)
    {
        if (_current_process_id != -1)
            return -1;

        // Clean up threads and pipes from the previous execution
        joinAll();
        closePipes();

#ifdef _WIN32
        HANDLE stdout_write = INVALID_HANDLE_VALUE;
        HANDLE stderr_write = INVALID_HANDLE_VALUE;

        SECURITY_ATTRIBUTES sa {};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;

        if (!CreatePipe(&_stdout_read, &stdout_write, &sa, 0)) return -1;
        if (!CreatePipe(&_stderr_read, &stderr_write, &sa, 0)) {
            CloseHandle(_stdout_read); _stdout_read = INVALID_HANDLE_VALUE;
            CloseHandle(stdout_write);
            return -1;
        }

        // Read ends must not be inherited by the child
        SetHandleInformation(_stdout_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(_stderr_read, HANDLE_FLAG_INHERIT, 0);

        // Open NUL device for stdin so the child always has a valid handle,
        // even when the parent process has no console (CREATE_NO_WINDOW).
        HANDLE hNul = CreateFileA("nul", GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        STARTUPINFOA si {};
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdOutput = stdout_write;
        si.hStdError  = stderr_write;
        si.hStdInput  = (hNul != INVALID_HANDLE_VALUE) ? hNul : NULL;

        // Wrap with "cmd /c" so that shell built-ins (echo, dir, …) and PATH
        // executables (cmake, git, …) are all handled correctly.
        std::string cmdLine = std::string("cmd /c ") + cmd;

        PROCESS_INFORMATION pi {};

        bool ok = CreateProcessA(
            nullptr, cmdLine.data(),
            nullptr, nullptr,
            TRUE,               // inherit handles
            CREATE_NO_WINDOW,
            nullptr,
            (cwd && cwd[0]) ? cwd : nullptr,
            &si, &pi);

        // Write ends and nul belong to the child; close our copies
        CloseHandle(stdout_write);
        CloseHandle(stderr_write);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

        if (!ok) {
            printf("[Process] CreateProcessA failed, error=%lu\n", GetLastError());
            fflush(stdout);
            closePipes();
            return -1;
        }

        CloseHandle(pi.hThread);
        _hProcess = pi.hProcess;

#else
        int stdout_pipe[2], stderr_pipe[2];

        if (pipe(stdout_pipe) != 0) return -1;
        if (pipe(stderr_pipe) != 0) {
            ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
            return -1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
            ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
            return -1;
        }

        if (pid == 0) {
            // Child: wire up stdout/stderr to the write ends
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(stderr_pipe[1], STDERR_FILENO);
            ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
            ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
            if (cwd && cwd[0]) chdir(cwd);
            execl("/bin/sh", "sh", "-c", cmd, nullptr);
            _exit(127);
        }

        // Parent: close write ends so EOF reaches readers when child exits
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        _stdout_read = stdout_pipe[0];
        _stderr_read = stderr_pipe[0];
        _pid = pid;
#endif

        _current_process_id = 1;

        _stdout_reader = std::thread(&RemoteProcess::stdoutReader, this);
        _stderr_reader = std::thread(&RemoteProcess::stderrReader, this);

        _executor = std::thread([this]() {
            setCurrentThreadName("RC_EXEC");
#ifdef _WIN32
            WaitForSingleObject(_hProcess, INFINITE);
            CloseHandle(_hProcess);
            _hProcess = INVALID_HANDLE_VALUE;
#else
            int status;
            waitpid(_pid, &status, 0);
            _pid = -1;
#endif
            _current_process_id = -1;
            // Pipe write ends were closed after CreateProcess/fork, so the
            // reader threads receive EOF and exit on their own.
        });

        return 1;
    }

    // -------------------------------------------------------------------------
    // await  –  wait for the process + all output to finish
    // -------------------------------------------------------------------------

    void RemoteProcess::await(int32_t /*process_id*/)
    {
        joinAll();
    }

    // -------------------------------------------------------------------------
    // close  –  kill the process, then wait for everything to finish
    // -------------------------------------------------------------------------

    void RemoteProcess::close(int32_t /*process_id*/)
    {
#ifdef _WIN32
        if (_hProcess != INVALID_HANDLE_VALUE)
            TerminateProcess(_hProcess, 1);
#else
        if (_pid != -1)
            kill(_pid, SIGTERM);
#endif
        joinAll();
    }

} // namespace Bn3Monkey
