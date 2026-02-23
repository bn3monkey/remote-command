#include "../../include/remote_command_server.hpp"
#include "../protocol/remote_command_protocol.hpp"

#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <filesystem>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
   typedef SOCKET sock_t;
   static const sock_t INVALID_SOCK = INVALID_SOCKET;
   static void closeSocket(sock_t s) { closesocket(s); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <sys/wait.h>
   typedef int sock_t;
   static const sock_t INVALID_SOCK = -1;
   static void closeSocket(sock_t s) { close(s); }
#endif

namespace fs = std::filesystem;

namespace Bn3Monkey
{
    // -------------------------------------------------------------------------
    // Internal struct (opaque from the header)
    // -------------------------------------------------------------------------
    struct RemoteCommandServer
    {
        sock_t command_server_sock { INVALID_SOCK };
        sock_t stream_server_sock  { INVALID_SOCK };
        sock_t command_client_sock { INVALID_SOCK };
        sock_t stream_client_sock  { INVALID_SOCK };

        std::string     current_directory;
        std::thread     handler_thread;
        std::atomic<bool> running;

        RemoteCommandServer() : running(false) {}
    };

    // -------------------------------------------------------------------------
    // Low-level helpers (identical to client side)
    // -------------------------------------------------------------------------
    static bool sendAll(sock_t sock, const void* data, size_t size)
    {
        const char* ptr = static_cast<const char*>(data);
        size_t remaining = size;
        while (remaining > 0) {
#ifdef _WIN32
            int chunk = static_cast<int>(remaining > 65536 ? 65536 : remaining);
            int sent  = ::send(sock, ptr, chunk, 0);
#else
            ssize_t sent = ::send(sock, ptr, remaining, 0);
#endif
            if (sent <= 0) return false;
            ptr       += sent;
            remaining -= static_cast<size_t>(sent);
        }
        return true;
    }

    static bool recvAll(sock_t sock, void* data, size_t size)
    {
        char* ptr = static_cast<char*>(data);
        size_t remaining = size;
        while (remaining > 0) {
#ifdef _WIN32
            int chunk    = static_cast<int>(remaining > 65536 ? 65536 : remaining);
            int received = ::recv(sock, ptr, chunk, 0);
#else
            ssize_t received = ::recv(sock, ptr, remaining, 0);
#endif
            if (received <= 0) return false;
            ptr       += received;
            remaining -= static_cast<size_t>(received);
        }
        return true;
    }

    // -------------------------------------------------------------------------
    // Send a stream chunk (stdout or stderr) to the stream socket
    // -------------------------------------------------------------------------
    static void sendStream(sock_t stream_sock,
                           RemoteCommandStreamType type,
                           const char* data,
                           uint32_t len)
    {
        if (len == 0) return;
        RemoteCommandStreamHeader header(type, len);
        sendAll(stream_sock, &header, sizeof(header));
        sendAll(stream_sock, data,    len);
    }

    // -------------------------------------------------------------------------
    // Execute a shell command, streaming stdout/stderr to stream_sock.
    // Called from the handler thread (blocking until command finishes).
    // -------------------------------------------------------------------------
    static void executeCommand(const std::string& cmd,
                               sock_t stream_sock,
                               const std::string& work_dir)
    {
#ifdef _WIN32
        // --- Windows: CreatePipe + CreateProcess ---
        SECURITY_ATTRIBUTES sa {};
        sa.nLength              = sizeof(sa);
        sa.bInheritHandle       = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE stdout_r, stdout_w, stderr_r, stderr_w;
        if (!CreatePipe(&stdout_r, &stdout_w, &sa, 0)) return;
        if (!CreatePipe(&stderr_r, &stderr_w, &sa, 0)) {
            CloseHandle(stdout_r); CloseHandle(stdout_w);
            return;
        }
        // Don't inherit the read ends
        SetHandleInformation(stdout_r, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stderr_r, HANDLE_FLAG_INHERIT, 0);

        std::string full_cmd = "cmd /c " + cmd;
        std::vector<char> cmd_buf(full_cmd.begin(), full_cmd.end());
        cmd_buf.push_back('\0');

        STARTUPINFOA si {};
        si.cb          = sizeof(si);
        si.hStdOutput  = stdout_w;
        si.hStdError   = stderr_w;
        si.dwFlags     = STARTF_USESTDHANDLES;

        PROCESS_INFORMATION pi {};
        BOOL ok = CreateProcessA(
            nullptr, cmd_buf.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW, nullptr,
            work_dir.empty() ? nullptr : work_dir.c_str(),
            &si, &pi);

        CloseHandle(stdout_w);
        CloseHandle(stderr_w);

        if (!ok) {
            CloseHandle(stdout_r);
            CloseHandle(stderr_r);
            return;
        }

        // Read stdout and stderr in parallel threads
        auto readPipe = [stream_sock](HANDLE h, RemoteCommandStreamType type) {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
                buf[n] = '\0';
                sendStream(stream_sock, type, buf, n);
            }
            CloseHandle(h);
        };

        std::thread t_out([&]{ readPipe(stdout_r, RemoteCommandStreamType::STREAM_OUTPUT); });
        std::thread t_err([&]{ readPipe(stderr_r, RemoteCommandStreamType::STREAM_ERROR);  });

        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        t_out.join();
        t_err.join();

#else
        // --- POSIX: pipe + fork + exec ---
        int stdout_pipe[2], stderr_pipe[2];
        if (pipe(stdout_pipe) != 0) return;
        if (pipe(stderr_pipe) != 0) {
            close(stdout_pipe[0]); close(stdout_pipe[1]);
            return;
        }

        pid_t pid = fork();
        if (pid == 0) {
            // Child
            close(stdout_pipe[0]);
            close(stderr_pipe[0]);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stdout_pipe[1]);
            close(stderr_pipe[1]);
            if (!work_dir.empty())
                chdir(work_dir.c_str());
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }

        // Parent
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        if (pid < 0) {
            close(stdout_pipe[0]);
            close(stderr_pipe[0]);
            return;
        }

        auto readPipe = [stream_sock](int fd, RemoteCommandStreamType type) {
            char buf[4096];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                sendStream(stream_sock, type, buf, static_cast<uint32_t>(n));
            }
            close(fd);
        };

        std::thread t_out([&]{ readPipe(stdout_pipe[0], RemoteCommandStreamType::STREAM_OUTPUT); });
        std::thread t_err([&]{ readPipe(stderr_pipe[0], RemoteCommandStreamType::STREAM_ERROR);  });

        waitpid(pid, nullptr, 0);
        t_out.join();
        t_err.join();
#endif
    }

    // -------------------------------------------------------------------------
    // Resolve a path relative to the server's current working directory
    // -------------------------------------------------------------------------
    static fs::path resolvePath(const std::string& cwd, const std::string& input)
    {
        fs::path p(input);
        if (p.is_absolute()) return p;
        return fs::path(cwd) / p;
    }

    // -------------------------------------------------------------------------
    // Main request-handling loop (runs in handler_thread)
    // -------------------------------------------------------------------------
    static void handleRequests(RemoteCommandServer* server)
    {
        sock_t cmd_sock = server->command_client_sock;
        sock_t stm_sock = server->stream_client_sock;

        while (server->running.load()) {
            // --- Receive request header ---
            RemoteCommandRequestHeader req_header(RemoteCommandInstruction::INSTRUCTION_EMPTY);
            if (!recvAll(cmd_sock, &req_header, sizeof(req_header))) break;
            if (!req_header.valid()) break;

            // --- Read variable-length payloads ---
            std::string p0(req_header.payload_0_length, '\0');
            std::string p1(req_header.payload_1_length, '\0');
            std::string p2(req_header.payload_2_length, '\0');
            std::string p3(req_header.payload_3_length, '\0');

            if (req_header.payload_0_length > 0 &&
                !recvAll(cmd_sock, &p0[0], req_header.payload_0_length)) break;
            if (req_header.payload_1_length > 0 &&
                !recvAll(cmd_sock, &p1[0], req_header.payload_1_length)) break;
            if (req_header.payload_2_length > 0 &&
                !recvAll(cmd_sock, &p2[0], req_header.payload_2_length)) break;
            if (req_header.payload_3_length > 0 &&
                !recvAll(cmd_sock, &p3[0], req_header.payload_3_length)) break;

            // --- Dispatch ---
            switch (req_header.instruction)
            {
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_CURRENT_WORKING_DIRECTORY:
            {
                const std::string& cwd = server->current_directory;
                RemoteCommandResponseHeader resp(req_header.instruction,
                                                 static_cast<uint32_t>(cwd.size()));
                sendAll(cmd_sock, &resp, sizeof(resp));
                sendAll(cmd_sock, cwd.c_str(), cwd.size());
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_MOVE_CURRENT_WORKING_DIRECTORY:
            {
                bool result = false;
                std::error_code ec;
                fs::path target = resolvePath(server->current_directory, p0);
                if (fs::exists(target, ec) && fs::is_directory(target, ec)) {
                    server->current_directory = target.string();
                    result = true;
                }
                RemoteCommandResponseHeader resp(req_header.instruction, sizeof(bool));
                sendAll(cmd_sock, &resp, sizeof(resp));
                sendAll(cmd_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_DIRECTORY_EXISTS:
            {
                std::error_code ec;
                fs::path target = resolvePath(server->current_directory, p0);
                bool result = fs::exists(target, ec) && fs::is_directory(target, ec);
                RemoteCommandResponseHeader resp(req_header.instruction, sizeof(bool));
                sendAll(cmd_sock, &resp, sizeof(resp));
                sendAll(cmd_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_LIST_DIRECTORY_CONTENTS:
            {
                fs::path target = resolvePath(server->current_directory,
                                              p0.empty() ? "." : p0);

                std::vector<RemoteDirectoryContentInner> contents;
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator(target, ec)) {
                    auto name = entry.path().filename().string();
                    if (entry.is_directory(ec)) {
                        contents.emplace_back(
                            RemoteDirectoryContentTypeInner::DIRECTORY, name.c_str());
                    } else if (entry.is_regular_file(ec)) {
                        contents.emplace_back(
                            RemoteDirectoryContentTypeInner::FILE, name.c_str());
                    }
                }

                uint32_t count       = static_cast<uint32_t>(contents.size());
                uint32_t payload_len = sizeof(uint32_t) +
                                       count * static_cast<uint32_t>(sizeof(RemoteDirectoryContentInner));
                RemoteCommandResponseHeader resp(req_header.instruction, payload_len);
                sendAll(cmd_sock, &resp, sizeof(resp));
                sendAll(cmd_sock, &count, sizeof(count));
                if (count > 0)
                    sendAll(cmd_sock, contents.data(),
                            count * sizeof(RemoteDirectoryContentInner));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_CREATE_DIRECTORY:
            {
                std::error_code ec;
                fs::path target = resolvePath(server->current_directory, p0);
                bool result = fs::create_directories(target, ec);
                RemoteCommandResponseHeader resp(req_header.instruction, sizeof(bool));
                sendAll(cmd_sock, &resp, sizeof(resp));
                sendAll(cmd_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_REMOVE_DIRECTORY:
            {
                std::error_code ec;
                fs::path target = resolvePath(server->current_directory, p0);
                bool result = (fs::remove_all(target, ec) > 0) && !ec;
                RemoteCommandResponseHeader resp(req_header.instruction, sizeof(bool));
                sendAll(cmd_sock, &resp, sizeof(resp));
                sendAll(cmd_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_COPY_DIRECTORY:
            {
                std::error_code ec;
                fs::path from = resolvePath(server->current_directory, p0);
                fs::path to   = resolvePath(server->current_directory, p1);
                fs::copy(from, to, fs::copy_options::recursive, ec);
                bool result = !ec;
                RemoteCommandResponseHeader resp(req_header.instruction, sizeof(bool));
                sendAll(cmd_sock, &resp, sizeof(resp));
                sendAll(cmd_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_MOVE_DIRECTORY:
            {
                std::error_code ec;
                fs::path from = resolvePath(server->current_directory, p0);
                fs::path to   = resolvePath(server->current_directory, p1);
                fs::rename(from, to, ec);
                bool result = !ec;
                RemoteCommandResponseHeader resp(req_header.instruction, sizeof(bool));
                sendAll(cmd_sock, &resp, sizeof(resp));
                sendAll(cmd_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_RUN_COMMAND:
            {
                // Execute, stream all output/error, then send empty response
                executeCommand(p0, stm_sock, server->current_directory);
                RemoteCommandResponseHeader resp(req_header.instruction, 0);
                sendAll(cmd_sock, &resp, sizeof(resp));
                break;
            }
            // -----------------------------------------------------------------
            default:
                break;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Create a listening TCP socket bound to port
    // -------------------------------------------------------------------------
    static sock_t createListenSocket(int port)
    {
        sock_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCK) return INVALID_SOCK;

        int yes = 1;
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

        sockaddr_in addr {};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(static_cast<uint16_t>(port));

        if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            closeSocket(sock);
            return INVALID_SOCK;
        }
        if (::listen(sock, 1) != 0) {
            closeSocket(sock);
            return INVALID_SOCK;
        }
        return sock;
    }

    // =========================================================================
    // Public API
    // =========================================================================

    RemoteCommandServer* openRemoteCommandServer(int32_t command_port,
                                                 int32_t stream_port,
                                                 const char* current_working_directory)
    {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return nullptr;
#endif
        auto* server = new RemoteCommandServer();

        // Resolve the initial working directory
        {
            std::error_code ec;
            fs::path p = (current_working_directory && current_working_directory[0] != '\0')
                             ? fs::path(current_working_directory)
                             : fs::current_path(ec);
            auto canonical = fs::canonical(p, ec);
            server->current_directory = ec ? p.string() : canonical.string();
        }

        // Create listening sockets
        server->command_server_sock = createListenSocket(command_port);
        if (server->command_server_sock == INVALID_SOCK) {
            delete server;
#ifdef _WIN32
            WSACleanup();
#endif
            return nullptr;
        }

        server->stream_server_sock = createListenSocket(stream_port);
        if (server->stream_server_sock == INVALID_SOCK) {
            closeSocket(server->command_server_sock);
            delete server;
#ifdef _WIN32
            WSACleanup();
#endif
            return nullptr;
        }

        // Accept command connection (blocking)
        server->command_client_sock = ::accept(server->command_server_sock, nullptr, nullptr);
        if (server->command_client_sock == INVALID_SOCK) {
            closeSocket(server->command_server_sock);
            closeSocket(server->stream_server_sock);
            delete server;
#ifdef _WIN32
            WSACleanup();
#endif
            return nullptr;
        }

        // Accept stream connection (blocking)
        server->stream_client_sock = ::accept(server->stream_server_sock, nullptr, nullptr);
        if (server->stream_client_sock == INVALID_SOCK) {
            closeSocket(server->command_client_sock);
            closeSocket(server->command_server_sock);
            closeSocket(server->stream_server_sock);
            delete server;
#ifdef _WIN32
            WSACleanup();
#endif
            return nullptr;
        }

        server->running.store(true);
        server->handler_thread = std::thread(handleRequests, server);
        return server;
    }

    void closeRemoteCommandServer(RemoteCommandServer* server)
    {
        if (!server) return;

        server->running.store(false);

        // Close client sockets to unblock any blocking recv in the handler
        if (server->command_client_sock != INVALID_SOCK) {
            closeSocket(server->command_client_sock);
            server->command_client_sock = INVALID_SOCK;
        }
        if (server->stream_client_sock != INVALID_SOCK) {
            closeSocket(server->stream_client_sock);
            server->stream_client_sock = INVALID_SOCK;
        }

        if (server->handler_thread.joinable())
            server->handler_thread.join();

        if (server->command_server_sock != INVALID_SOCK)
            closeSocket(server->command_server_sock);
        if (server->stream_server_sock != INVALID_SOCK)
            closeSocket(server->stream_server_sock);

        delete server;
#ifdef _WIN32
        WSACleanup();
#endif
    }

} // namespace Bn3Monkey
