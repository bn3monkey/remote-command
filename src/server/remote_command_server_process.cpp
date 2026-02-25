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
        if (_stdin_write != INVALID_HANDLE_VALUE) {
            CloseHandle(_stdin_write);
            _stdin_write = INVALID_HANDLE_VALUE;
        }
        if (_stdout_read != INVALID_HANDLE_VALUE) {
            CloseHandle(_stdout_read);
            _stdout_read = INVALID_HANDLE_VALUE;
        }
        if (_stderr_read != INVALID_HANDLE_VALUE) {
            CloseHandle(_stderr_read);
            _stderr_read = INVALID_HANDLE_VALUE;
        }
#else
        if (_stdin_write != -1) { ::close(_stdin_write); _stdin_write = -1; }
        if (_stdout_read != -1) { ::close(_stdout_read); _stdout_read = -1; }
        if (_stderr_read != -1) { ::close(_stderr_read); _stderr_read = -1; }
#endif
    }

    void RemoteProcess::joinReaders()
    {
        if (_stdout_reader.joinable()) _stdout_reader.join();
        if (_stderr_reader.joinable()) _stderr_reader.join();
    }

    void RemoteProcess::reapProcess()
    {
#ifdef _WIN32
        if (_hProcess != INVALID_HANDLE_VALUE) {
            WaitForSingleObject(_hProcess, INFINITE);
            CloseHandle(_hProcess);
            _hProcess = INVALID_HANDLE_VALUE;
        }
#else
        if (_pid != -1) {
            int status;
            waitpid(_pid, &status, 0);
            _pid = -1;
        }
#endif
        _current_process_id = -1;
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
        joinReaders();
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

        // Create a stdin pipe: child gets the read end, we keep the write end open.
        // As long as _stdin_write is open the child never receives EOF on stdin,
        // which is required for interactive processes like `openssl s_server`.
        HANDLE stdin_read = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&stdin_read, &_stdin_write, &sa, 0)) {
            CloseHandle(_stdout_read); _stdout_read = INVALID_HANDLE_VALUE;
            CloseHandle(_stderr_read); _stderr_read = INVALID_HANDLE_VALUE;
            CloseHandle(stdout_write);
            CloseHandle(stderr_write);
            return -1;
        }
        // Write end must not be inherited by the child
        SetHandleInformation(_stdin_write, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si {};
        si.cb         = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = stdin_read;
        si.hStdOutput = stdout_write;
        si.hStdError  = stderr_write;

        std::string cmdLine(cmd);

        PROCESS_INFORMATION pi {};

        bool ok = CreateProcessA(
            nullptr, cmdLine.data(),
            nullptr, nullptr,
            TRUE,               // inherit handles
            CREATE_NO_WINDOW,
            nullptr,
            (cwd && cwd[0]) ? cwd : nullptr,
            &si, &pi);

        // Write ends and stdin_read belong to the child; close our copies.
        // _stdin_write is intentionally kept open so the child never gets EOF.
        CloseHandle(stdin_read);
        CloseHandle(stdout_write);
        CloseHandle(stderr_write);

        if (!ok) {
            printf("[Process] CreateProcessA failed, error=%lu\n", GetLastError());
            fflush(stdout);
            closePipes();
            return -1;
        }

        CloseHandle(pi.hThread);
        _hProcess = pi.hProcess;

#else
        int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];

        if (pipe(stdin_pipe) != 0) return -1;
        if (pipe(stdout_pipe) != 0) {
            ::close(stdin_pipe[0]); ::close(stdin_pipe[1]);
            return -1;
        }
        if (pipe(stderr_pipe) != 0) {
            ::close(stdin_pipe[0]); ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
            return -1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            ::close(stdin_pipe[0]);  ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
            ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
            return -1;
        }

        if (pid == 0) {
            // Child: become a new process group leader so kill(-pgid) later
            // can terminate the entire subtree (including grandchildren).
            setpgid(0, 0);
            dup2(stdin_pipe[0],  STDIN_FILENO);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(stderr_pipe[1], STDERR_FILENO);
            ::close(stdin_pipe[0]);  ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
            ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
            if (cwd && cwd[0]) chdir(cwd);
            execl("/bin/sh", "sh", "-c", cmd, nullptr);
            _exit(127);
        }

        // Parent: close read end of stdin pipe and both write ends of output pipes.
        // Keep _stdin_write open so the child never receives EOF on stdin.
        ::close(stdin_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);
        _stdin_write = stdin_pipe[1];
        _stdout_read = stdout_pipe[0];
        _stderr_read = stderr_pipe[0];
        _pid = pid;
#endif

        _current_process_id = 1;

        _stdout_reader = std::thread(&RemoteProcess::stdoutReader, this);
        _stderr_reader = std::thread(&RemoteProcess::stderrReader, this);

        return 1;
    }

    // -------------------------------------------------------------------------
    // await  –  wait for the process + all output to finish
    // -------------------------------------------------------------------------

    void RemoteProcess::await(int32_t /*process_id*/)
    {
        // Reader threads exit naturally when the process ends (pipe EOF).
        joinReaders();
        reapProcess();  // waitpid/WaitForSingleObject + sets _current_process_id = -1
    }

    // -------------------------------------------------------------------------
    // close  –  kill the process, then wait for everything to finish
    // -------------------------------------------------------------------------

    void RemoteProcess::close(int32_t /*process_id*/)
    {
#ifdef _WIN32
        if (_hProcess != INVALID_HANDLE_VALUE)
            TerminateProcess(_hProcess, 1);

        // CancelSynchronousIo cancels the pending ReadFile in each reader thread,
        // causing it to return ERROR_OPERATION_ABORTED immediately.
        // This avoids the CloseHandle/ReadFile deadlock: we never touch the pipe
        // handle while ReadFile is in progress; we just cancel the I/O first,
        // then close the handles after the threads have exited.
        if (_stdout_reader.joinable())
            CancelSynchronousIo(_stdout_reader.native_handle());
        if (_stderr_reader.joinable())
            CancelSynchronousIo(_stderr_reader.native_handle());

        joinReaders();
        closePipes();
#else
        if (_pid != -1)
            kill(-_pid, SIGTERM);   // negative PID = kill entire process group

        // On Linux, close(fd) does not block on a concurrent read(fd),
        // so closePipes() before joinReaders() is safe.
        closePipes();
        joinReaders();
#endif
        reapProcess();  // reap zombie + sets _current_process_id = -1
    }

    int32_t RemoteProcess::executeWithoutPipe(const char* cwd, const char* cmd)
    {
        if (_current_process_id != -1)
            return -1;

#ifdef _WIN32
        char buffer[4096] {0};
        snprintf(buffer, sizeof(buffer), "%s", cmd);

        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);

        BOOL result = CreateProcessA(
            nullptr,
            buffer,
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            (cwd && cwd[0]) ? cwd : nullptr,
            &si,
            &pi
        );

        if (!result)
            return -1;

        CloseHandle(pi.hThread);
        _hProcess = pi.hProcess;

#else
        pid_t pid = fork();
        if (pid < 0)
            return -1;

        if (pid == 0) {
            setpgid(0, 0);
            if (cwd && cwd[0]) chdir(cwd);
            execl("/bin/sh", "sh", "-c", cmd, nullptr);
            _exit(127);
        }

        _pid = pid;
#endif

        _current_process_id = 1;
        return 1;
    }

    void RemoteProcess::closeWithoutPipe(int32_t /*process_id*/)
    {
#ifdef _WIN32
        if (_hProcess != INVALID_HANDLE_VALUE) {
            TerminateProcess(_hProcess, 1);
            WaitForSingleObject(_hProcess, INFINITE);
            CloseHandle(_hProcess);
            _hProcess = INVALID_HANDLE_VALUE;
        }
#else
        if (_pid != -1) {
            kill(-_pid, SIGTERM);
            int status;
            waitpid(_pid, &status, 0);
            _pid = -1;
        }
#endif
        _current_process_id = -1;
    }

} // namespace Bn3Monkey