#include "remote_command_server_socket.hpp"

using namespace Bn3Monkey;

bool Bn3Monkey::sendAll(sock_t sock, const void* data, size_t size)
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

bool Bn3Monkey::recvAll(sock_t sock, void* data, size_t size)
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
// Send a stream chunk (stdout or stderr) to the stream socket.
// The Locked variant serialises concurrent writes (openProcess IO threads
// and runCommand IO threads may run simultaneously).
// -------------------------------------------------------------------------
void Bn3Monkey::sendStream(sock_t stream_sock,
                        RemoteCommandStreamType type,
                        const char* data,
                        uint32_t len)
{
    if (len == 0) return;
    RemoteCommandStreamHeader header(type, len);
    Bn3Monkey::sendAll(stream_sock, &header, sizeof(header));
    Bn3Monkey::sendAll(stream_sock, data,    len);
}

void Bn3Monkey::sendStreamLocked(std::mutex& mtx,
                                sock_t stream_sock,
                                RemoteCommandStreamType type,
                                const char* data,
                                uint32_t len)
{
    if (len == 0) return;
    RemoteCommandStreamHeader header(type, len);
    std::lock_guard<std::mutex> lk(mtx);
    Bn3Monkey::sendAll(stream_sock, &header, sizeof(header));
    Bn3Monkey::sendAll(stream_sock, data,    len);
}

sock_t Bn3Monkey::acceptWithSelect(sock_t          server_sock,
                                   sockaddr_in*    addr_out,
                                   std::atomic<bool>& running)
{
    while (running.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_sock, &read_fds);

        timeval tv{};
        tv.tv_usec = 100 * 1000; // 100 ms

#ifdef _WIN32
        int ret = ::select(0, &read_fds, nullptr, nullptr, &tv);
#else
        int ret = ::select(static_cast<int>(server_sock) + 1,
                            &read_fds, nullptr, nullptr, &tv);
#endif
        if (ret <= 0) continue; // timeout or transient error → retry

        if (FD_ISSET(server_sock, &read_fds)) {
            socklen_t len = sizeof(sockaddr_in);
            sockaddr_in tmp{};
            sock_t client = ::accept(server_sock,
                                        reinterpret_cast<sockaddr*>(&tmp), &len);
            if (client == INVALID_SOCK) continue;
            if (addr_out) *addr_out = tmp;
            return client;
        }
    }
    return INVALID_SOCK;
}