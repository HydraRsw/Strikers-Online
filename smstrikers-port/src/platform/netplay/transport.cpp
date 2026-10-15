/*
 * transport.cpp — Cross-platform non-blocking UDP socket implementation.
 *
 * Uses Winsock2 on Windows and POSIX sockets elsewhere.
 * SDL3 is available but its networking is optional and not always present,
 * so we use the OS sockets directly.
 */

#include "port/netplay/transport.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET socket_t;
    #define SOCKET_INVALID INVALID_SOCKET
    #define SOCKET_ERROR_VAL SOCKET_ERROR
    static bool s_wsaInitialized = false;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    typedef int socket_t;
    #define SOCKET_INVALID (-1)
    #define SOCKET_ERROR_VAL (-1)
    #define closesocket close
#endif

#include <chrono>

/* ------------------------------------------------------------------------ */
/*  Internal socket structure                                                */
/* ------------------------------------------------------------------------ */

struct NetplaySocket {
    socket_t         fd;
    struct sockaddr_in peerAddr;
    bool             hasPeer;
};

/* ------------------------------------------------------------------------ */
/*  Platform init                                                            */
/* ------------------------------------------------------------------------ */

#ifdef _WIN32
static bool EnsureWSA()
{
    if (!s_wsaInitialized)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            fprintf(stderr, "[netplay] WSAStartup failed: %d\n", WSAGetLastError());
            return false;
        }
        s_wsaInitialized = true;
    }
    return true;
}
#endif

/* ------------------------------------------------------------------------ */
/*  Parse "ip:port" into sockaddr_in                                         */
/* ------------------------------------------------------------------------ */

static bool ParseAddr(const char* addr, struct sockaddr_in* out)
{
    if (!addr || !out) return false;

    /* Copy so we can mutate. */
    char buf[256];
    size_t len = strlen(addr);
    if (len >= sizeof(buf)) return false;
    memcpy(buf, addr, len + 1);

    /* Find the last colon (to handle IPv4 only). */
    char* colon = strrchr(buf, ':');
    if (!colon) return false;

    *colon = '\0';
    const char* ip   = buf;
    const char* port = colon + 1;

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons((u16)atoi(port));

    if (inet_pton(AF_INET, ip, &out->sin_addr) != 1)
    {
        struct addrinfo hints;
        struct addrinfo* res = nullptr;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        if (getaddrinfo(ip, nullptr, &hints, &res) == 0 && res)
        {
            struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
            out->sin_addr = sin->sin_addr;
            freeaddrinfo(res);
        }
        else
        {
            fprintf(stderr, "[netplay] invalid or unresolvable address: %s\n", ip);
            return false;
        }
    }

    return true;
}

/* ------------------------------------------------------------------------ */
/*  Socket API                                                               */
/* ------------------------------------------------------------------------ */

NetplaySocket* NetplaySocketCreate(u16 localPort)
{
#ifdef _WIN32
    if (!EnsureWSA()) return nullptr;
#endif

    socket_t fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == SOCKET_INVALID)
    {
        fprintf(stderr, "[netplay] socket() failed\n");
        return nullptr;
    }

    /* Set non-blocking. */
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

#ifdef _WIN32
    /* By default Windows turns an ICMP "port unreachable" (e.g. the relay was
       restarted, or nothing listens on the address we tried) into a
       WSAECONNRESET error on the NEXT recvfrom, which made the receive loops
       stop early and drop real packets.  Turn that behaviour off. */
    {
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
        BOOL  newBehavior = FALSE;
        DWORD bytesReturned = 0;
        WSAIoctl(fd, SIO_UDP_CONNRESET, &newBehavior, sizeof(newBehavior),
                 nullptr, 0, &bytesReturned, nullptr, nullptr);
    }
#endif

    /* Allow address reuse. */
    int optval = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&optval), sizeof(optval));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
#endif

    /* Bind to local port. */
    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port        = htons(localPort);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&bindAddr),
             sizeof(bindAddr)) == SOCKET_ERROR_VAL)
    {
        fprintf(stderr, "[netplay] bind(%d) failed\n", localPort);
        closesocket(fd);
        return nullptr;
    }

    NetplaySocket* sock = new NetplaySocket();
    sock->fd      = fd;
    sock->hasPeer = false;
    memset(&sock->peerAddr, 0, sizeof(sock->peerAddr));

    fprintf(stderr, "[netplay] socket bound to port %d\n", localPort);
    return sock;
}

void NetplaySocketDestroy(NetplaySocket* sock)
{
    if (!sock) return;
    closesocket(sock->fd);
    delete sock;
}

bool NetplaySocketSetPeer(NetplaySocket* sock, const char* addr)
{
    if (!sock) return false;

    if (!ParseAddr(addr, &sock->peerAddr))
        return false;

    sock->hasPeer = true;

    char ipBuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sock->peerAddr.sin_addr, ipBuf, sizeof(ipBuf));
    fprintf(stderr, "[netplay] peer set to %s:%d\n",
            ipBuf, ntohs(sock->peerAddr.sin_port));
    return true;
}

int NetplaySocketSend(NetplaySocket* sock, const void* data, size_t len)
{
    if (!sock || !sock->hasPeer) return -1;

    int result = sendto(sock->fd,
                        static_cast<const char*>(data),
                        static_cast<int>(len),
                        0,
                        reinterpret_cast<struct sockaddr*>(&sock->peerAddr),
                        sizeof(sock->peerAddr));

    if (result == SOCKET_ERROR_VAL)
    {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 0;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
#endif
        return -1;
    }

    return result;
}

int NetplaySocketRecv(NetplaySocket* sock, void* buf, size_t bufSize,
                      char* fromAddr, size_t fromAddrSize)
{
    if (!sock) return -1;

    struct sockaddr_in senderAddr;
    socklen_t addrLen = sizeof(senderAddr);

    int result = recvfrom(sock->fd,
                          static_cast<char*>(buf),
                          static_cast<int>(bufSize),
                          0,
                          reinterpret_cast<struct sockaddr*>(&senderAddr),
                          &addrLen);

    if (result == SOCKET_ERROR_VAL)
    {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 0;
        /* A packet bigger than our buffer, or a leftover ICMP error: neither
           means the socket is empty, so tell the caller to keep reading. */
        if (err == WSAEMSGSIZE || err == WSAECONNRESET) return NetplaySocketRecv(sock, buf, bufSize, fromAddr, fromAddrSize);
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == ECONNREFUSED) return NetplaySocketRecv(sock, buf, bufSize, fromAddr, fromAddrSize);
#endif
        return -1;
    }

    if (result > 0 && fromAddr && fromAddrSize > 0)
    {
        char ipBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &senderAddr.sin_addr, ipBuf, sizeof(ipBuf));
        snprintf(fromAddr, fromAddrSize, "%s:%d",
                 ipBuf, ntohs(senderAddr.sin_port));
    }

    return result;
}

/* ------------------------------------------------------------------------ */
/*  Monotonic time                                                           */
/* ------------------------------------------------------------------------ */

u64 NetplayGetTimeUs()
{
    using namespace std::chrono;
    auto now = steady_clock::now();
    return static_cast<u64>(
        duration_cast<microseconds>(now.time_since_epoch()).count());
}

const char* NetplayGetLocalIP()
{
    static char s_detectedIP[64] = "";
    if (s_detectedIP[0] != '\0')
        return s_detectedIP;

#ifdef _WIN32
    if (!EnsureWSA())
    {
        strncpy(s_detectedIP, "127.0.0.1", sizeof(s_detectedIP) - 1);
        return s_detectedIP;
    }
#endif

    char host[256];
    if (gethostname(host, sizeof(host)) != 0)
    {
        strncpy(s_detectedIP, "127.0.0.1", sizeof(s_detectedIP) - 1);
        return s_detectedIP;
    }

    struct addrinfo hints;
    struct addrinfo* res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res)
    {
        strncpy(s_detectedIP, "127.0.0.1", sizeof(s_detectedIP) - 1);
        return s_detectedIP;
    }

    char tailscaleIP[64] = "";
    char lanIP[64] = "";

    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next)
    {
        struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));

        if (strncmp(ipStr, "100.", 4) == 0 && tailscaleIP[0] == '\0')
        {
            strncpy(tailscaleIP, ipStr, sizeof(tailscaleIP) - 1);
        }
        else if (strncmp(ipStr, "127.", 4) != 0 && strncmp(ipStr, "169.254.", 8) != 0 && lanIP[0] == '\0')
        {
            strncpy(lanIP, ipStr, sizeof(lanIP) - 1);
        }
    }
    freeaddrinfo(res);

    if (tailscaleIP[0] != '\0')
    {
        strncpy(s_detectedIP, tailscaleIP, sizeof(s_detectedIP) - 1);
    }
    else if (lanIP[0] != '\0')
    {
        strncpy(s_detectedIP, lanIP, sizeof(s_detectedIP) - 1);
    }
    else
    {
        strncpy(s_detectedIP, "127.0.0.1", sizeof(s_detectedIP) - 1);
    }

    return s_detectedIP;
}
