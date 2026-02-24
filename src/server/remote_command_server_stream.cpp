#include "remote_command_server_stream.hpp"
#include "remote_command_server_helper.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif

namespace Bn3Monkey
{
    // -------------------------------------------------------------------------
    // acceptLoop  (runs in _accepter thread)
    // -------------------------------------------------------------------------

    void StreamServer::acceptLoop()
    {
        setCurrentThreadName("RC_STACC");

        while (_running.load()) {
            sock_t new_sock = acceptWithSelect(_server_sock, nullptr, _running);
            if (new_sock == INVALID_SOCK)
                break;

            // Hand the new socket to RemoteProcess; get back the old one to close.
            sock_t old_sock = _remote_process.setStreamSocket(new_sock);
            if (old_sock != INVALID_SOCK)
                closeSocket(old_sock);
        }

        // Clear the socket in RemoteProcess on the way out
        sock_t remaining = _remote_process.setStreamSocket(INVALID_SOCK);
        if (remaining != INVALID_SOCK)
            closeSocket(remaining);
    }

    // -------------------------------------------------------------------------
    // open
    // -------------------------------------------------------------------------

    bool StreamServer::open(int32_t stream_port)
    {
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
        addr.sin_port        = htons(static_cast<uint16_t>(stream_port));

        if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(sock, 1) != 0) {
            closeSocket(sock);
            return false;
        }

        _server_sock = sock;
        _running.store(true);
        _accepter = std::thread(&StreamServer::acceptLoop, this);
        return true;
    }

    // -------------------------------------------------------------------------
    // close
    // -------------------------------------------------------------------------

    void StreamServer::close()
    {
        if (!_running.load()) return;

        _running.store(false);

        if (_accepter.joinable())
            _accepter.join();

        if (_server_sock != INVALID_SOCK) {
            closeSocket(_server_sock);
            _server_sock = INVALID_SOCK;
        }
    }

} // namespace Bn3Monkey
