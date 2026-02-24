#include "remote_command_server_command.hpp"
#include "remote_command_server_helper.hpp"
#include "../protocol/remote_command_protocol.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#endif

#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

namespace Bn3Monkey
{
    // -------------------------------------------------------------------------
    // Helper: resolve a path relative to cwd
    // -------------------------------------------------------------------------
    static fs::path resolvePath(const std::string& cwd, const std::string& p)
    {
        fs::path fp(p);
        return fp.is_absolute() ? fp : fs::path(cwd) / fp;
    }

    // -------------------------------------------------------------------------
    // handleCommand  –  serve one connected client until it disconnects
    // -------------------------------------------------------------------------

    void CommandServer::handleCommand(sock_t client_sock)
    {
        while (_running.load()) {
            RemoteCommandRequestHeader req(RemoteCommandInstruction::INSTRUCTION_EMPTY);
            if (!recvAll(client_sock, &req, sizeof(req))) break;
            if (!req.valid()) break;

            std::string p0(req.payload_0_length, '\0');
            std::string p1(req.payload_1_length, '\0');
            std::string p2(req.payload_2_length, '\0');
            std::string p3(req.payload_3_length, '\0');

            if (req.payload_0_length > 0 && !recvAll(client_sock, p0.data(), req.payload_0_length)) break;
            if (req.payload_1_length > 0 && !recvAll(client_sock, p1.data(), req.payload_1_length)) break;
            if (req.payload_2_length > 0 && !recvAll(client_sock, p2.data(), req.payload_2_length)) break;
            if (req.payload_3_length > 0 && !recvAll(client_sock, p3.data(), req.payload_3_length)) break;

            switch (req.instruction)
            {
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_CURRENT_WORKING_DIRECTORY:
            {
                const std::string& cwd = _current_directory;
                RemoteCommandResponseHeader resp(req.instruction,
                    static_cast<uint32_t>(cwd.size()));
                sendAll(client_sock, &resp, sizeof(resp));
                sendAll(client_sock, cwd.c_str(), cwd.size());
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_MOVE_CURRENT_WORKING_DIRECTORY:
            {
                std::error_code ec;
                fs::path target = resolvePath(_current_directory, p0);
                bool result = false;
                if (fs::exists(target, ec) && fs::is_directory(target, ec)) {
                    _current_directory = target.string();
                    result = true;
                }
                RemoteCommandResponseHeader resp(req.instruction, sizeof(bool));
                sendAll(client_sock, &resp, sizeof(resp));
                sendAll(client_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_DIRECTORY_EXISTS:
            {
                std::error_code ec;
                fs::path target = resolvePath(_current_directory, p0);
                bool result = fs::exists(target, ec) && fs::is_directory(target, ec);
                RemoteCommandResponseHeader resp(req.instruction, sizeof(bool));
                sendAll(client_sock, &resp, sizeof(resp));
                sendAll(client_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_LIST_DIRECTORY_CONTENTS:
            {
                fs::path target = resolvePath(_current_directory, p0.empty() ? "." : p0);
                std::vector<RemoteDirectoryContentInner> contents;
                std::error_code ec;
                for (const auto& entry : fs::directory_iterator(target, ec)) {
                    auto name = entry.path().filename().string();
                    if (entry.is_directory(ec))
                        contents.emplace_back(RemoteDirectoryContentTypeInner::DIRECTORY, name.c_str());
                    else if (entry.is_regular_file(ec))
                        contents.emplace_back(RemoteDirectoryContentTypeInner::FILE, name.c_str());
                }
                uint32_t count = static_cast<uint32_t>(contents.size());
                uint32_t payload_len = sizeof(uint32_t) +
                    count * static_cast<uint32_t>(sizeof(RemoteDirectoryContentInner));
                RemoteCommandResponseHeader resp(req.instruction, payload_len);
                sendAll(client_sock, &resp, sizeof(resp));
                sendAll(client_sock, &count, sizeof(count));
                if (count > 0)
                    sendAll(client_sock, contents.data(), count * sizeof(RemoteDirectoryContentInner));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_CREATE_DIRECTORY:
            {
                std::error_code ec;
                fs::path target = resolvePath(_current_directory, p0);
                bool result = fs::create_directories(target, ec);
                RemoteCommandResponseHeader resp(req.instruction, sizeof(bool));
                sendAll(client_sock, &resp, sizeof(resp));
                sendAll(client_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_REMOVE_DIRECTORY:
            {
                std::error_code ec;
                fs::path target = resolvePath(_current_directory, p0);
                bool result = (fs::remove_all(target, ec) > 0) && !ec;
                RemoteCommandResponseHeader resp(req.instruction, sizeof(bool));
                sendAll(client_sock, &resp, sizeof(resp));
                sendAll(client_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_COPY_DIRECTORY:
            {
                std::error_code ec;
                fs::path from = resolvePath(_current_directory, p0);
                fs::path to   = resolvePath(_current_directory, p1);
                fs::copy(from, to, fs::copy_options::recursive, ec);
                bool result = !ec;
                RemoteCommandResponseHeader resp(req.instruction, sizeof(bool));
                sendAll(client_sock, &resp, sizeof(resp));
                sendAll(client_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_MOVE_DIRECTORY:
            {
                std::error_code ec;
                fs::path from = resolvePath(_current_directory, p0);
                fs::path to   = resolvePath(_current_directory, p1);
                fs::rename(from, to, ec);
                bool result = !ec;
                RemoteCommandResponseHeader resp(req.instruction, sizeof(bool));
                sendAll(client_sock, &resp, sizeof(resp));
                sendAll(client_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_RUN_COMMAND:
            {
                // execute() starts the process + reader threads (stream via RemoteProcess)
                int32_t pid = _remote_process.execute(_current_directory.c_str(), p0.c_str());
                if (pid != -1)
                    _remote_process.await(pid);   // blocks until done + all output flushed

                RemoteCommandResponseHeader resp(req.instruction, 0);
                sendAll(client_sock, &resp, sizeof(resp));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_OPEN_PROCESS:
            {
                int32_t proc_id = _remote_process.execute(_current_directory.c_str(), p0.c_str());
                RemoteCommandResponseHeader resp(req.instruction, sizeof(int32_t));
                sendAll(client_sock, &resp, sizeof(resp));
                sendAll(client_sock, &proc_id, sizeof(proc_id));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_CLOSE_PROCESS:
            {
                int32_t proc_id = -1;
                if (req.payload_0_length >= sizeof(int32_t))
                    memcpy(&proc_id, p0.data(), sizeof(int32_t));

                if (proc_id != -1)
                    _remote_process.close(proc_id);

                RemoteCommandResponseHeader resp(req.instruction, 0);
                sendAll(client_sock, &resp, sizeof(resp));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_UPLOAD_FILE:
            {
                std::error_code ec;
                fs::path target = resolvePath(_current_directory, p0);
                fs::create_directories(target.parent_path(), ec);

                bool result = false;
                std::ofstream file(target, std::ios::binary);
                if (file.is_open()) {
                    if (!p1.empty())
                        file.write(p1.data(), static_cast<std::streamsize>(p1.size()));
                    result = !file.fail();
                }
                RemoteCommandResponseHeader resp(req.instruction, sizeof(bool));
                sendAll(client_sock, &resp, sizeof(resp));
                sendAll(client_sock, &result, sizeof(result));
                break;
            }
            // -----------------------------------------------------------------
            case RemoteCommandInstruction::INSTRUCTION_DOWNLOAD_FILE:
            {
                fs::path target = resolvePath(_current_directory, p0);
                std::ifstream file(target, std::ios::binary);
                if (!file.is_open()) {
                    uint8_t fail = 0;
                    RemoteCommandResponseHeader resp(req.instruction, sizeof(fail));
                    sendAll(client_sock, &resp, sizeof(resp));
                    sendAll(client_sock, &fail, sizeof(fail));
                } else {
                    std::vector<char> data(
                        (std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
                    uint32_t payload_len = 1u + static_cast<uint32_t>(data.size());
                    RemoteCommandResponseHeader resp(req.instruction, payload_len);
                    sendAll(client_sock, &resp, sizeof(resp));
                    uint8_t ok = 1;
                    sendAll(client_sock, &ok, sizeof(ok));
                    if (!data.empty())
                        sendAll(client_sock, data.data(), data.size());
                }
                break;
            }
            // -----------------------------------------------------------------
            default:
                break;
            }
        }
    }

    // -------------------------------------------------------------------------
    // handlerLoop  (runs in _handler thread)
    // -------------------------------------------------------------------------

    void CommandServer::handlerLoop()
    {
        setCurrentThreadName("RC_CMDH");

        while (_running.load()) {
            sockaddr_in client_addr {};
            sock_t client_sock = acceptWithSelect(_server_sock, &client_addr, _running);
            if (client_sock == INVALID_SOCK) break;

            char ip[INET_ADDRSTRLEN] = "?.?.?.?";
            inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
            printf("[Command] Client connected: %s:%d\n", ip, ntohs(client_addr.sin_port));
            fflush(stdout);

            _client_sock = client_sock;
            handleCommand(client_sock);

            // Kill any process left running when client disconnects
            if (_remote_process.is_running())
                _remote_process.close(1);

            printf("[Command] Client disconnected: %s:%d\n", ip, ntohs(client_addr.sin_port));
            fflush(stdout);

            closeSocket(client_sock);
            _client_sock = INVALID_SOCK;
        }
    }

    // -------------------------------------------------------------------------
    // open / close
    // -------------------------------------------------------------------------

    bool CommandServer::open(int32_t command_port, const char* initial_cwd)
    {
        // Resolve initial working directory
        {
            std::error_code ec;
            fs::path p = (initial_cwd && initial_cwd[0])
                             ? fs::path(initial_cwd)
                             : fs::current_path(ec);
            auto canonical = fs::canonical(p, ec);
            _current_directory = ec ? p.string() : canonical.string();
        }

        sock_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCK) return false;

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
        addr.sin_port        = htons(static_cast<uint16_t>(command_port));

        if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(sock, 1) != 0) {
#ifdef _WIN32
            shutdown(sock, SD_BOTH);
#else
            shutdown(sock, SHUT_RDWR);
#endif
            closeSocket(sock);
            return false;
        }

        _server_sock = sock;
        _running.store(true);
        _handler = std::thread(&CommandServer::handlerLoop, this);
        return true;
    }

    void CommandServer::close()
    {
        if (!_running.load()) return;

        _running.store(false);

        // Wake up handleCommand if it is blocked on recvAll
        if (_client_sock != INVALID_SOCK) {

            closeSocket(_client_sock);
            _client_sock = INVALID_SOCK;
        }

        if (_handler.joinable())
            _handler.join();

        if (_server_sock != INVALID_SOCK) {
#ifdef _WIN32
            shutdown(_server_sock, SD_BOTH);
#else
            shutdown(_server_sock, SHUT_RDWR);
#endif
            closeSocket(_server_sock);
            _server_sock = INVALID_SOCK;
        }
    }

} // namespace Bn3Monkey
