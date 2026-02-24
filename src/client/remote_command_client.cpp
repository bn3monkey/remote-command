#include "../../include/remote_command_client.hpp"
#include "../protocol/remote_command_protocol.hpp"

#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <fstream>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
   static const sock_t INVALID_SOCK = INVALID_SOCKET;
   // shutdown() wakes up any thread blocked in recv() before releasing the fd
   static void closeSocket(sock_t s) { shutdown(s, SD_BOTH); closesocket(s); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   typedef int sock_t;
   static const sock_t INVALID_SOCK = -1;
   // POSIX: close() alone does NOT interrupt a blocked recv() in another thread.
   // shutdown(SHUT_RDWR) marks the socket unreadable so recv() returns 0 immediately.
   static void closeSocket(sock_t s) { shutdown(s, SHUT_RDWR); close(s); }
#endif

namespace Bn3Monkey
{
    // -------------------------------------------------------------------------
    // Internal struct (opaque from the header)
    // -------------------------------------------------------------------------
    struct RemoteCommandClient
    {
        sock_t          command_sock  { INVALID_SOCK };
        sock_t          stream_sock   { INVALID_SOCK };
        OnRemoteOutput  on_remote_output { nullptr };
        OnRemoteError   on_remote_error  { nullptr };
        std::thread     stream_thread;
        std::atomic<bool> running;
        char            cwd_buffer[4096] { 0 };

        RemoteCommandClient() : running(false) {}
    };

    // -------------------------------------------------------------------------
    // Low-level helpers
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
    // Send a request with 0 / 1 / 2 payloads
    // -------------------------------------------------------------------------
    static bool sendRequest(sock_t sock,
                            RemoteCommandInstruction instruction)
    {
        RemoteCommandRequestHeader header(instruction);
        return sendAll(sock, &header, sizeof(header));
    }

    static bool sendRequest(sock_t sock,
                            RemoteCommandInstruction instruction,
                            const char* p0)
    {
        uint32_t len0 = static_cast<uint32_t>(strlen(p0));
        RemoteCommandRequestHeader header(instruction, len0);
        if (!sendAll(sock, &header, sizeof(header))) return false;
        return sendAll(sock, p0, len0);
    }

    static bool sendRequest(sock_t sock,
                            RemoteCommandInstruction instruction,
                            const char* p0, const char* p1)
    {
        uint32_t len0 = static_cast<uint32_t>(strlen(p0));
        uint32_t len1 = static_cast<uint32_t>(strlen(p1));
        RemoteCommandRequestHeader header(instruction, len0, len1);
        if (!sendAll(sock, &header, sizeof(header))) return false;
        if (!sendAll(sock, p0, len0))                return false;
        return sendAll(sock, p1, len1);
    }

    // raw binary payload_0 only (used by closeProcess)
    static bool sendRequest(sock_t sock,
                            RemoteCommandInstruction instruction,
                            const void* p0_data, uint32_t p0_len)
    {
        RemoteCommandRequestHeader header(instruction, p0_len);
        if (!sendAll(sock, &header, sizeof(header))) return false;
        if (p0_len > 0 && !sendAll(sock, p0_data, p0_len)) return false;
        return true;
    }

    // string path + raw binary payload (used by uploadFile)
    static bool sendRequest(sock_t sock,
                            RemoteCommandInstruction instruction,
                            const char* p0,
                            const void* p1_data, uint32_t p1_len)
    {
        uint32_t len0 = static_cast<uint32_t>(strlen(p0));
        RemoteCommandRequestHeader header(instruction, len0, p1_len);
        if (!sendAll(sock, &header, sizeof(header))) return false;
        if (!sendAll(sock, p0, len0))                return false;
        if (p1_len > 0 && !sendAll(sock, p1_data, p1_len)) return false;
        return true;
    }

    // -------------------------------------------------------------------------
    // Receive a response header + optional payload into a vector
    // -------------------------------------------------------------------------
    static bool recvResponse(sock_t sock,
                             RemoteCommandInstruction expected,
                             std::vector<char>& payload_out)
    {
        RemoteCommandResponseHeader header(RemoteCommandInstruction::INSTRUCTION_EMPTY);
        if (!recvAll(sock, &header, sizeof(header))) return false;
        if (!header.valid())                          return false;
        if (header.instruction != expected)           return false;

        payload_out.assign(header.payload_length, '\0');
        if (header.payload_length > 0)
            return recvAll(sock, payload_out.data(), header.payload_length);
        return true;
    }

    // -------------------------------------------------------------------------
    // Stream thread: reads output/error packets and fires callbacks
    // -------------------------------------------------------------------------
    static void streamThreadFunc(RemoteCommandClient* client)
    {
        while (client->running.load()) {
            RemoteCommandStreamHeader header(RemoteCommandStreamType::INVALID, 0);
            if (!recvAll(client->stream_sock, &header, sizeof(header))) break;
            if (!header.valid()) break;

            if (header.payload_length == 0) continue;

            std::vector<char> buf(header.payload_length + 1, '\0');
            if (!recvAll(client->stream_sock, buf.data(), header.payload_length)) break;

            if (header.type == RemoteCommandStreamType::STREAM_OUTPUT) {
                if (client->on_remote_output)
                    client->on_remote_output(buf.data());
            } else if (header.type == RemoteCommandStreamType::STREAM_ERROR) {
                if (client->on_remote_error)
                    client->on_remote_error(buf.data());
            }
        }
    }

    // -------------------------------------------------------------------------
    // Connect helper
    // -------------------------------------------------------------------------
    static sock_t connectToServer(const char* host, int port)
    {
        sock_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCK) return INVALID_SOCK;

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(port));
#ifdef _WIN32
        InetPtonA(AF_INET, host, &addr.sin_addr);
#else
        inet_pton(AF_INET, host, &addr.sin_addr);
#endif

        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            closeSocket(sock);
            return INVALID_SOCK;
        }
        return sock;
    }

    // =========================================================================
    // Public API
    // =========================================================================

    RemoteCommandClient* createRemoteCommandContext(int32_t command_port, int32_t stream_port, const char* ip)
    {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return nullptr;
#endif
        const char* host = (ip && ip[0] != '\0') ? ip : "127.0.0.1";
        auto* client = new RemoteCommandClient();

        client->command_sock = connectToServer(host, command_port);
        if (client->command_sock == INVALID_SOCK) {
            delete client;
#ifdef _WIN32
            WSACleanup();
#endif
            return nullptr;
        }

        client->stream_sock = connectToServer(host, stream_port);
        if (client->stream_sock == INVALID_SOCK) {
            closeSocket(client->command_sock);
            delete client;
#ifdef _WIN32
            WSACleanup();
#endif
            return nullptr;
        }

        client->running.store(true);
        client->stream_thread = std::thread(streamThreadFunc, client);
        return client;
    }

    void releaseRemoteCommandContext(RemoteCommandClient* client)
    {
        if (!client) return;

        client->running.store(false);
        closeSocket(client->stream_sock);
        closeSocket(client->command_sock);
        client->stream_sock  = INVALID_SOCK;
        client->command_sock = INVALID_SOCK;

        if (client->stream_thread.joinable())
            client->stream_thread.join();

        delete client;
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void onRemoteOutput(RemoteCommandClient* client, OnRemoteOutput handler)
    {
        if (client) client->on_remote_output = handler;
    }

    void onRemoteError(RemoteCommandClient* client, OnRemoteError handler)
    {
        if (client) client->on_remote_error = handler;
    }

    // -------------------------------------------------------------------------
    // Directory / filesystem commands
    // -------------------------------------------------------------------------

    const char* currentWorkingDirectory(RemoteCommandClient* client)
    {
        if (!client) return nullptr;

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_CURRENT_WORKING_DIRECTORY))
            return nullptr;

        std::vector<char> payload;
        if (!recvResponse(client->command_sock,
                          RemoteCommandInstruction::INSTRUCTION_CURRENT_WORKING_DIRECTORY,
                          payload))
            return nullptr;

        size_t len = payload.size() < sizeof(client->cwd_buffer) - 1
                         ? payload.size()
                         : sizeof(client->cwd_buffer) - 1;
        memcpy(client->cwd_buffer, payload.data(), len);
        client->cwd_buffer[len] = '\0';
        return client->cwd_buffer;
    }

    bool moveWorkingDirectory(RemoteCommandClient* client, const char* path)
    {
        if (!client || !path) return false;

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_MOVE_CURRENT_WORKING_DIRECTORY,
                         path))
            return false;

        std::vector<char> payload;
        if (!recvResponse(client->command_sock,
                          RemoteCommandInstruction::INSTRUCTION_MOVE_CURRENT_WORKING_DIRECTORY,
                          payload))
            return false;

        bool result = false;
        if (payload.size() >= sizeof(bool))
            memcpy(&result, payload.data(), sizeof(bool));
        return result;
    }

    bool directoryExists(RemoteCommandClient* client, const char* path)
    {
        if (!client || !path) return false;

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_DIRECTORY_EXISTS,
                         path))
            return false;

        std::vector<char> payload;
        if (!recvResponse(client->command_sock,
                          RemoteCommandInstruction::INSTRUCTION_DIRECTORY_EXISTS,
                          payload))
            return false;

        bool result = false;
        if (payload.size() >= sizeof(bool))
            memcpy(&result, payload.data(), sizeof(bool));
        return result;
    }

    std::vector<RemoteDirectoryContent> listDirectoryContents(
        RemoteCommandClient* client, const char* path)
    {
        std::vector<RemoteDirectoryContent> result;
        if (!client) return result;

        const char* p = path ? path : ".";
        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_LIST_DIRECTORY_CONTENTS,
                         p))
            return result;

        std::vector<char> payload;
        if (!recvResponse(client->command_sock,
                          RemoteCommandInstruction::INSTRUCTION_LIST_DIRECTORY_CONTENTS,
                          payload))
            return result;

        if (payload.size() < sizeof(uint32_t)) return result;

        uint32_t count = 0;
        memcpy(&count, payload.data(), sizeof(uint32_t));

        const size_t expected = sizeof(uint32_t) +
                                count * sizeof(RemoteDirectoryContentInner);
        if (payload.size() < expected) return result;

        const RemoteDirectoryContentInner* inner =
            reinterpret_cast<const RemoteDirectoryContentInner*>(
                payload.data() + sizeof(uint32_t));

        result.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            RemoteDirectoryContent item;
            item.type = (inner[i].type == RemoteDirectoryContentTypeInner::DIRECTORY)
                            ? RemoteDirectoryContentType::DIRECTORY
                            : RemoteDirectoryContentType::FILE;
            strncpy(item.name, inner[i].name, sizeof(item.name) - 1);
            item.name[sizeof(item.name) - 1] = '\0';
            result.push_back(item);
        }
        return result;
    }

    bool createDirectory(RemoteCommandClient* client, const char* path)
    {
        if (!client || !path) return false;

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_CREATE_DIRECTORY,
                         path))
            return false;

        std::vector<char> payload;
        if (!recvResponse(client->command_sock,
                          RemoteCommandInstruction::INSTRUCTION_CREATE_DIRECTORY,
                          payload))
            return false;

        bool result = false;
        if (payload.size() >= sizeof(bool))
            memcpy(&result, payload.data(), sizeof(bool));
        return result;
    }

    bool removeDirectory(RemoteCommandClient* client, const char* path)
    {
        if (!client || !path) return false;

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_REMOVE_DIRECTORY,
                         path))
            return false;

        std::vector<char> payload;
        if (!recvResponse(client->command_sock,
                          RemoteCommandInstruction::INSTRUCTION_REMOVE_DIRECTORY,
                          payload))
            return false;

        bool result = false;
        if (payload.size() >= sizeof(bool))
            memcpy(&result, payload.data(), sizeof(bool));
        return result;
    }

    bool copyDirectory(RemoteCommandClient* client,
                       const char* from_path, const char* to_path)
    {
        if (!client || !from_path || !to_path) return false;

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_COPY_DIRECTORY,
                         from_path, to_path))
            return false;

        std::vector<char> payload;
        if (!recvResponse(client->command_sock,
                          RemoteCommandInstruction::INSTRUCTION_COPY_DIRECTORY,
                          payload))
            return false;

        bool result = false;
        if (payload.size() >= sizeof(bool))
            memcpy(&result, payload.data(), sizeof(bool));
        return result;
    }

    bool moveDirectory(RemoteCommandClient* client,
                       const char* from_path, const char* to_path)
    {
        if (!client || !from_path || !to_path) return false;

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_MOVE_DIRECTORY,
                         from_path, to_path))
            return false;

        std::vector<char> payload;
        if (!recvResponse(client->command_sock,
                          RemoteCommandInstruction::INSTRUCTION_MOVE_DIRECTORY,
                          payload))
            return false;

        bool result = false;
        if (payload.size() >= sizeof(bool))
            memcpy(&result, payload.data(), sizeof(bool));
        return result;
    }

    // -------------------------------------------------------------------------
    // Process management
    // -------------------------------------------------------------------------
    int32_t openProcessImpl(RemoteCommandClient* client, const char* cmd)
    {
        if (!client || !cmd) return -1;

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_OPEN_PROCESS,
                         cmd))
            return -1;

        std::vector<char> payload;
        if (!recvResponse(client->command_sock,
                          RemoteCommandInstruction::INSTRUCTION_OPEN_PROCESS,
                          payload))
            return -1;

        int32_t proc_id = -1;
        if (payload.size() >= sizeof(int32_t))
            memcpy(&proc_id, payload.data(), sizeof(int32_t));
        return proc_id;
    }

    void closeProcess(RemoteCommandClient* client, int32_t process_id)
    {
        if (!client) return;

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_CLOSE_PROCESS,
                         &process_id, static_cast<uint32_t>(sizeof(process_id))))
            return;

        std::vector<char> payload;
        recvResponse(client->command_sock,
                     RemoteCommandInstruction::INSTRUCTION_CLOSE_PROCESS,
                     payload);
        // void return — just wait for the server's acknowledgement
    }

    // -------------------------------------------------------------------------
    // File transfer
    // -------------------------------------------------------------------------
    bool uploadFile(RemoteCommandClient* client,
                    const char* local_file, const char* remote_file)
    {
        if (!client || !local_file || !remote_file) return false;

        std::ifstream f(local_file, std::ios::binary);
        if (!f.is_open()) return false;

        std::vector<char> data((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_UPLOAD_FILE,
                         remote_file,
                         data.empty() ? nullptr : data.data(),
                         static_cast<uint32_t>(data.size())))
            return false;

        std::vector<char> payload;
        if (!recvResponse(client->command_sock,
                          RemoteCommandInstruction::INSTRUCTION_UPLOAD_FILE,
                          payload))
            return false;

        bool result = false;
        if (payload.size() >= sizeof(bool))
            memcpy(&result, payload.data(), sizeof(bool));
        return result;
    }

    bool downloadFile(RemoteCommandClient* client,
                      const char* local_file, const char* remote_file)
    {
        if (!client || !local_file || !remote_file) return false;

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_DOWNLOAD_FILE,
                         remote_file))
            return false;

        std::vector<char> payload;
        if (!recvResponse(client->command_sock,
                          RemoteCommandInstruction::INSTRUCTION_DOWNLOAD_FILE,
                          payload))
            return false;

        // First byte is the success flag (0 = failure, 1 = success)
        if (payload.empty() || payload[0] == 0) return false;

        std::ofstream f(local_file, std::ios::binary);
        if (!f.is_open()) return false;

        if (payload.size() > 1)
            f.write(payload.data() + 1,
                    static_cast<std::streamsize>(payload.size() - 1));
        return true;
    }

    // -------------------------------------------------------------------------
    // Command execution
    //  - Sends request on command_sock
    //  - Blocks until empty response (server finished streaming)
    //  - Stream callbacks fire from the stream thread during execution
    // -------------------------------------------------------------------------
    void runCommandImpl(RemoteCommandClient* client, const char* cmd)
    {
        if (!client || !cmd) return;

        if (!sendRequest(client->command_sock,
                         RemoteCommandInstruction::INSTRUCTION_RUN_COMMAND,
                         cmd))
            return;

        std::vector<char> payload;
        recvResponse(client->command_sock,
                     RemoteCommandInstruction::INSTRUCTION_RUN_COMMAND,
                     payload);
        // payload is empty for RUN_COMMAND — just waiting for completion signal
    }

} // namespace Bn3Monkey
