/*
 * strikers_relay.cpp — High-performance standalone Matchmaking and Relay Server
 * for Super Mario Strikers PC Port.
 *
 * Compiles to a native executable with ZERO external dependencies.
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define closesocket close
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <map>
#include <vector>

static const int DEFAULT_PORT = 7200;
static const time_t ROOM_TIMEOUT_SECS = 1800; // 30 minutes

struct Room {
    std::string code;
    sockaddr_in host_addr;
    sockaddr_in guest_addr;
    bool        has_guest;
    time_t      created_at;
    time_t      last_active;
    uint64_t    packets_relayed;
};

static bool addr_equal(const sockaddr_in& a, const sockaddr_in& b)
{
    return (a.sin_addr.s_addr == b.sin_addr.s_addr) && (a.sin_port == b.sin_port);
}

static std::string addr_to_string(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr.sin_addr), ip, INET_ADDRSTRLEN);
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

static std::string addr_to_ip(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr.sin_addr), ip, INET_ADDRSTRLEN);
    return std::string(ip);
}

static std::string generate_room_code(const std::map<std::string, Room>& rooms)
{
    for (int i = 0; i < 100; i++)
    {
        int num = 1000 + (rand() % 9000);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", num);
        if (rooms.find(buf) == rooms.end())
            return buf;
    }
    return std::to_string(10000 + (rand() % 90000));
}

static std::string extract_json_field(const char* json, const char* field)
{
    std::string key = std::string("\"") + field + "\"";
    const char* pos = strstr(json, key.c_str());
    if (!pos) return "";
    pos += key.length();

    // Skip whitespace and colon
    while (*pos == ' ' || *pos == '\t' || *pos == ':') pos++;

    if (*pos == '\"')
    {
        pos++;
        const char* end = strchr(pos, '\"');
        if (!end) return "";
        return std::string(pos, end - pos);
    }

    const char* end = pos;
    while (*end && *end != ',' && *end != '}' && *end != ' ' && *end != '\r' && *end != '\n') end++;
    return std::string(pos, end - pos);
}

int main(int argc, char* argv[])
{
    srand(static_cast<unsigned int>(time(NULL)));

    int port = DEFAULT_PORT;
    if (argc > 1)
    {
        int p = atoi(argv[1]);
        if (p > 0 && p < 65536)
            port = p;
    }

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        fprintf(stderr, "Failed to create UDP socket\n");
        return 1;
    }

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(static_cast<u_short>(port));

    if (bind(sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
    {
        fprintf(stderr, "Failed to bind to UDP port %d. Is the server already running?\n", port);
        closesocket(sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    printf("============================================================\n");
    printf(" Super Mario Strikers Netplay Relay Server (Native C++)\n");
    printf(" LISTENING ON UDP PORT: %d\n", port);
    printf("============================================================\n");
    printf(" NOTE: The Server Port is ALWAYS %d. Do NOT change it in the game!\n", port);
    printf(" Ready for room matchmaking and rollback packet relaying.\n\n");
    fflush(stdout);

    std::map<std::string, Room> rooms;
    time_t last_cleanup = time(NULL);

    char buf[4096];
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    for (;;)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int ret = select(static_cast<int>(sock + 1), &readfds, NULL, NULL, &tv);
        time_t now = time(NULL);

        // Periodically purge stale rooms
        if (now - last_cleanup > 30)
        {
            last_cleanup = now;
            std::vector<std::string> expired;
            for (auto it = rooms.begin(); it != rooms.end(); ++it)
            {
                if (now - it->second.last_active > ROOM_TIMEOUT_SECS)
                    expired.push_back(it->first);
            }
            for (size_t i = 0; i < expired.size(); i++)
            {
                printf("[-] Room %s expired and removed\n", expired[i].c_str());
                rooms.erase(expired[i]);
            }
            if (!expired.empty()) fflush(stdout);
        }

        if (ret <= 0 || !FD_ISSET(sock, &readfds))
            continue;

        client_len = sizeof(client_addr);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&client_addr, &client_len);
        if (n <= 0)
            continue;

        // Check if JSON command packet
        if (buf[0] == '{' && buf[n - 1] == '}')
        {
            buf[n] = '\0';
            std::string cmd = extract_json_field(buf, "cmd");

            if (cmd == "create")
            {
                /* The game resends "create" until it hears back.  If this
                   address already owns a room, answer with that room instead
                   of opening a second one. */
                bool resent = false;
                for (auto it = rooms.begin(); it != rooms.end(); ++it)
                {
                    if (addr_equal(it->second.host_addr, client_addr))
                    {
                        it->second.last_active = now;
                        std::string resp = "{\"cmd\":\"created\",\"room\":\"" + it->first + "\",\"slot\":0}";
                        sendto(sock, resp.c_str(), static_cast<int>(resp.length()), 0,
                               (sockaddr*)&client_addr, client_len);
                        resent = true;
                        break;
                    }
                }
                if (resent)
                    continue;

                std::string code = generate_room_code(rooms);
                Room r;
                r.code = code;
                r.host_addr = client_addr;
                memset(&r.guest_addr, 0, sizeof(r.guest_addr));
                r.has_guest = false;
                r.created_at = now;
                r.last_active = now;
                r.packets_relayed = 0;

                rooms[code] = r;

                std::string resp = "{\"cmd\":\"created\",\"room\":\"" + code + "\",\"slot\":0}";
                sendto(sock, resp.c_str(), static_cast<int>(resp.length()), 0,
                       (sockaddr*)&client_addr, client_len);

                printf("[+] Room %s created by Host (%s)\n", code.c_str(), addr_to_ip(client_addr).c_str());
                printf("    Share code '%s' with Player 2 to join!\n", code.c_str());
                fflush(stdout);
            }
            else if (cmd == "join")
            {
                std::string code = extract_json_field(buf, "room");
                auto it = rooms.find(code);
                if (it == rooms.end())
                {
                    std::string err = "{\"cmd\":\"error\",\"message\":\"Room " + code + " not found\"}";
                    sendto(sock, err.c_str(), static_cast<int>(err.length()), 0,
                           (sockaddr*)&client_addr, client_len);
                    printf("[!] Client (%s) tried to join non-existent room %s\n",
                           addr_to_ip(client_addr).c_str(), code.c_str());
                    fflush(stdout);
                }
                else
                {
                    Room& r = it->second;
                    bool repeat = r.has_guest && addr_equal(r.guest_addr, client_addr);
                    r.guest_addr = client_addr;
                    r.has_guest = true;
                    r.last_active = now;
                    if (repeat)
                    {
                        /* Resent join: answer both sides again (one of our
                           replies was probably lost) but don't log it twice. */
                        std::string g = "{\"cmd\":\"joined\",\"room\":\"" + code + "\",\"slot\":1}";
                        sendto(sock, g.c_str(), static_cast<int>(g.length()), 0,
                               (sockaddr*)&client_addr, client_len);
                        std::string h = "{\"cmd\":\"peer_joined\",\"room\":\"" + code + "\"}";
                        sendto(sock, h.c_str(), static_cast<int>(h.length()), 0,
                               (sockaddr*)&r.host_addr, sizeof(r.host_addr));
                        continue;
                    }

                    // Notify Guest
                    std::string guest_resp = "{\"cmd\":\"joined\",\"room\":\"" + code + "\",\"slot\":1}";
                    sendto(sock, guest_resp.c_str(), static_cast<int>(guest_resp.length()), 0,
                           (sockaddr*)&client_addr, client_len);

                    // Notify Host
                    std::string host_resp = "{\"cmd\":\"peer_joined\",\"room\":\"" + code + "\"}";
                    sendto(sock, host_resp.c_str(), static_cast<int>(host_resp.length()), 0,
                           (sockaddr*)&r.host_addr, sizeof(r.host_addr));

                    printf("[+] Guest (%s) joined Room %s! Starting match session with Host (%s)\n",
                           addr_to_ip(client_addr).c_str(), code.c_str(),
                           addr_to_ip(r.host_addr).c_str());
                    fflush(stdout);
                }
            }
            else if (cmd == "leave")
            {
                std::string room_code = extract_json_field(buf, "room");
                auto it = rooms.find(room_code);
                if (it != rooms.end())
                {
                    Room& r = it->second;
                    if (addr_equal(client_addr, r.host_addr))
                    {
                        /* The host leaves: the room closes. */
                        if (r.has_guest)
                        {
                            const char* peer_left = "{\"cmd\":\"peer_left\"}";
                            sendto(sock, peer_left, static_cast<int>(strlen(peer_left)), 0,
                                   (sockaddr*)&r.guest_addr, sizeof(r.guest_addr));
                        }
                        printf("[-] Room %s closed by the host (%s)\n", room_code.c_str(),
                               addr_to_ip(client_addr).c_str());
                        rooms.erase(it);
                    }
                    else if (r.has_guest && addr_equal(client_addr, r.guest_addr))
                    {
                        /* The guest leaves: the room stays open so they (or someone
                           else) can join again with the same code. */
                        const char* peer_left = "{\"cmd\":\"peer_left\"}";
                        sendto(sock, peer_left, static_cast<int>(strlen(peer_left)), 0,
                               (sockaddr*)&r.host_addr, sizeof(r.host_addr));
                        r.has_guest = false;
                        memset(&r.guest_addr, 0, sizeof(r.guest_addr));
                        r.packets_relayed = 0;
                        r.last_active = now;
                        printf("[-] Guest left room %s; it stays open for a rejoin\n", room_code.c_str());
                    }
                    fflush(stdout);
                }
            }
            else if (cmd == "ping")
            {
                /* A host waiting in its room pings so the NAT / tunnel mapping
                   stays open; keep the room alive too. */
                for (auto it = rooms.begin(); it != rooms.end(); ++it)
                {
                    Room& r = it->second;
                    if (addr_equal(client_addr, r.host_addr) ||
                        (r.has_guest && addr_equal(client_addr, r.guest_addr)))
                    {
                        r.last_active = now;
                        break;
                    }
                }
                const char* pong = "{\"cmd\":\"pong\"}";
                sendto(sock, pong, static_cast<int>(strlen(pong)), 0,
                       (sockaddr*)&client_addr, client_len);
            }
            continue;
        }

        // Binary netplay packet: Forward directly to peer in the same room!
        for (auto it = rooms.begin(); it != rooms.end(); ++it)
        {
            Room& r = it->second;
            if (!r.has_guest)
                continue;

            if (addr_equal(client_addr, r.host_addr))
            {
                r.last_active = now;
                if (r.packets_relayed == 0)
                {
                    printf("[*] Relaying live match packets between Host and Guest in Room %s...\n", r.code.c_str());
                    fflush(stdout);
                }
                r.packets_relayed++;
                sendto(sock, buf, n, 0, (sockaddr*)&r.guest_addr, sizeof(r.guest_addr));
                break;
            }
            else if (addr_equal(client_addr, r.guest_addr))
            {
                r.last_active = now;
                if (r.packets_relayed == 0)
                {
                    printf("[*] Relaying live match packets between Host and Guest in Room %s...\n", r.code.c_str());
                    fflush(stdout);
                }
                r.packets_relayed++;
                sendto(sock, buf, n, 0, (sockaddr*)&r.host_addr, sizeof(r.host_addr));
                break;
            }
        }
    }

    closesocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
