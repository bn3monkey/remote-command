#if !defined(__REMOTE_COMMAND_SERVER_SOCKET__)
#define __REMOTE_COMMAND_SERVER_SOCKET__

#include "../protocol/remote_command_protocol.hpp"
#include <mutex>
#include <atomic>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
   typedef SOCKET sock_t;
   static constexpr sock_t INVALID_SOCK = INVALID_SOCKET;
   inline void closeSocket(sock_t s) { closesocket(s); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <sys/wait.h>
#  include <signal.h>
   typedef int sock_t;
   static constexpr sock_t INVALID_SOCK = -1;
   inline void closeSocket(sock_t s) { close(s); }
#endif



namespace Bn3Monkey {
    bool sendAll(sock_t sock, const void* data, size_t size);
    bool recvAll(sock_t sock, void* data, size_t size);

    // -------------------------------------------------------------------------
    // Send a stream chunk (stdout or stderr) to the stream socket.
    // The Locked variant serialises concurrent writes (openProcess IO threads
    // and runCommand IO threads may run simultaneously).
    // -------------------------------------------------------------------------
    void sendStream(sock_t stream_sock,
                            RemoteCommandStreamType type,
                            const char* data,
                            uint32_t len);

    void sendStreamLocked(std::mutex& mtx,
                            sock_t stream_sock,
                            RemoteCommandStreamType type,
                            const char* data,
                            uint32_t len);

    // -------------------------------------------------------------------------
    // Accept one connection on server_sock, using select() with a 100 ms
    // timeout so the loop can be interrupted by setting running = false.
    // Returns INVALID_SOCK when running becomes false or on error.
    // addr_out may be nullptr if the caller does not need the peer address.
    // -------------------------------------------------------------------------
    sock_t acceptWithSelect(sock_t          server_sock,
                                   sockaddr_in*    addr_out,
                                   std::atomic<bool>& running);
    
}

#endif // __REMOTE_COMMAND_SERVER_SOCKET__