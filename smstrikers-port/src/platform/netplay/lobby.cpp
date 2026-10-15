/*
 * lobby.cpp — Matchmaking room client implementation.
 *
 * Talks JSON over UDP to strikers_relay.  UDP drops packets, so every
 * request is resent until the relay answers, the host pings the relay while
 * it waits (otherwise the NAT / Playit mapping expires and the "opponent
 * joined" message never arrives), and a host that misses "peer_joined" still
 * notices the guest when its first handshake shows up.
 */

#include "port/netplay/lobby.h"
#include "port/netplay/netplay.h"
#include "port/netplay/transport.h"

#include "port/overlay.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" int PortMatchExists(void);   // Game.cpp

static const u64 kResendUs          = 500000;    /* resend create/join every 0.5 s */
static const u64 kLocalRelayTryUs   = 700000;    /* host: give a local relay 0.7 s */
static const u64 kRequestTimeoutUs  = 8000000;   /* give up after 8 s */
static const u64 kHostPingUs        = 2000000;   /* keep the room alive every 2 s */

static int NetplayDelayFromConfig()
{
    const char* s = std::getenv("STRIKERS_NETPLAY_DELAY");
    if (!s || !*s) s = std::getenv("STRIKERS_DELAY");
    int delay = (s && *s) ? atoi(s) : 2;
    if (delay < 0) delay = 0;
    if (delay > NETPLAY_MAX_INPUT_DELAY) delay = NETPLAY_MAX_INPUT_DELAY;
    return delay;
}

static bool IsLocalHost(const char* host)
{
    return strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0;
}

/* Pull "field":"value" out of a flat JSON object. */
static bool JsonString(const char* json, const char* field, char* out, size_t outSize)
{
    char key[64];
    snprintf(key, sizeof(key), "\"%s\":\"", field);
    const char* p = strstr(json, key);
    if (!p) return false;
    p += strlen(key);
    const char* end = strchr(p, '"');
    if (!end) return false;
    size_t len = static_cast<size_t>(end - p);
    if (len >= outSize) len = outSize - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

NetplayLobby& NetplayLobby::Instance()
{
    static NetplayLobby s_instance;
    return s_instance;
}

NetplayLobby::NetplayLobby()
    : m_state(LOBBY_STATE_IDLE)
    , m_serverPort(7200)
    , m_tryingLocalRelay(false)
    , m_usingLocalRelay(false)
    , m_slot(0)
    , m_socket(nullptr)
    , m_requestTimeUs(0)
    , m_lastSendUs(0)
{
    m_roomCode[0] = '\0';
    m_errorMessage[0] = '\0';
    m_notice[0] = '\0';
    m_serverHost[0] = '\0';
    m_activeAddr[0] = '\0';
}

NetplayLobby::~NetplayLobby()
{
    Leave();
}

void NetplayLobby::Leave()
{
    /* In a session the socket belongs to NetplayManager: take it back so the
       relay hears that we left (a guest leaving keeps the host's room open). */
    if (!m_socket && m_state == LOBBY_STATE_CONNECTED)
        m_socket = NetplayManager::Instance().DetachSocket(true);

    if (m_socket)
    {
        if (m_roomCode[0] != '\0')
        {
            char leaveReq[128];
            snprintf(leaveReq, sizeof(leaveReq), "{\"cmd\":\"leave\",\"room\":\"%s\"}", m_roomCode);
            NetplaySocketSend(static_cast<NetplaySocket*>(m_socket), leaveReq, strlen(leaveReq));
        }
        NetplaySocketDestroy(static_cast<NetplaySocket*>(m_socket));
        m_socket = nullptr;
    }

    if (NetplayManager::Instance().GetState() != NETPLAY_STATE_IDLE)
    {
        NetplayManager::Instance().Disconnect();
    }

    m_state = LOBBY_STATE_IDLE;
    m_roomCode[0] = '\0';
    m_errorMessage[0] = '\0';
    m_notice[0] = '\0';
    m_activeAddr[0] = '\0';
    m_tryingLocalRelay = false;
    m_usingLocalRelay = false;
}

void NetplayLobby::Fail(const char* message)
{
    strncpy(m_errorMessage, message, sizeof(m_errorMessage) - 1);
    m_errorMessage[sizeof(m_errorMessage) - 1] = '\0';
    m_state = LOBBY_STATE_ERROR;
    if (m_socket)
    {
        NetplaySocketDestroy(static_cast<NetplaySocket*>(m_socket));
        m_socket = nullptr;
    }
    fprintf(stderr, "[lobby] error: %s\n", m_errorMessage);
}

bool NetplayLobby::OpenSocket(const char* host, u16 port)
{
    if (!m_socket)
    {
        /* Bind to any available local UDP port. */
        m_socket = NetplaySocketCreate(0);
        if (!m_socket)
        {
            Fail("Failed to bind local UDP socket");
            return false;
        }
    }

    snprintf(m_activeAddr, sizeof(m_activeAddr), "%s:%u", host, port);
    if (!NetplaySocketSetPeer(static_cast<NetplaySocket*>(m_socket), m_activeAddr))
    {
        char msg[320];
        snprintf(msg, sizeof(msg), "Invalid or unreachable server address %s", m_activeAddr);
        Fail(msg);
        return false;
    }
    return true;
}

void NetplayLobby::SendRequest()
{
    if (!m_socket) return;

    char req[128];
    if (m_state == LOBBY_STATE_JOINING)
        snprintf(req, sizeof(req), "{\"cmd\":\"join\",\"room\":\"%s\"}", m_roomCode);
    else
        snprintf(req, sizeof(req), "{\"cmd\":\"create\"}");

    NetplaySocketSend(static_cast<NetplaySocket*>(m_socket), req, strlen(req));
    m_lastSendUs = NetplayGetTimeUs();
}

bool NetplayLobby::HostRoom(const char* serverHost, u16 serverPort)
{
    Leave();

    if (!serverHost || !*serverHost)
        serverHost = "127.0.0.1";

    strncpy(m_serverHost, serverHost, sizeof(m_serverHost) - 1);
    m_serverHost[sizeof(m_serverHost) - 1] = '\0';
    m_serverPort = serverPort;
    m_slot = 0;

    /* The usual setup runs the relay on the host's own PC behind a Playit
       tunnel.  If the host also talks to it through the tunnel, every packet
       leaves the PC, goes to Playit and comes back: extra lag on every
       frame, and one more NAT mapping that can expire.  So try the relay on
       this PC first and only fall back to the typed address if none answers. */
    m_tryingLocalRelay = !IsLocalHost(m_serverHost);
    bool ok = m_tryingLocalRelay ? OpenSocket("127.0.0.1", 7200)
                                 : OpenSocket(m_serverHost, m_serverPort);
    if (!ok)
        return false;

    m_state = LOBBY_STATE_REQUESTING;
    m_requestTimeUs = NetplayGetTimeUs();
    SendRequest();
    fprintf(stderr, "[lobby] requested room creation on %s\n", m_activeAddr);
    return true;
}

bool NetplayLobby::JoinRoom(const char* serverHost, u16 serverPort, const char* roomCode)
{
    Leave();

    if (!serverHost || !*serverHost)
        serverHost = "127.0.0.1";

    if (!roomCode || !*roomCode)
    {
        Fail("Please enter a valid 4-digit room code");
        return false;
    }

    strncpy(m_serverHost, serverHost, sizeof(m_serverHost) - 1);
    m_serverHost[sizeof(m_serverHost) - 1] = '\0';
    m_serverPort = serverPort;
    strncpy(m_roomCode, roomCode, sizeof(m_roomCode) - 1);
    m_roomCode[sizeof(m_roomCode) - 1] = '\0';
    m_slot = 1;

    if (!OpenSocket(m_serverHost, m_serverPort))
        return false;

    m_state = LOBBY_STATE_JOINING;
    m_requestTimeUs = NetplayGetTimeUs();
    SendRequest();
    fprintf(stderr, "[lobby] requested to join room %s on %s\n", m_roomCode, m_activeAddr);
    return true;
}

static void ReturnToMenuIfInMatch()
{
    if (!PortMatchExists())
        return;
    PortDebugCommand k;
    memset(&k, 0, sizeof(k));
    k.op = PDBG_RETURN_TO_FE;
    PortDebugPushCommand(&k);
}

void NetplayLobby::WatchSession()
{
    NetplayManager& mgr = NetplayManager::Instance();
    if (mgr.GetState() != NETPLAY_STATE_DISCONNECTED)
        return;

    ReturnToMenuIfInMatch();

    if (m_slot == 0)
    {
        /* Host: keep the room.  Same socket, so the relay still knows us, and
           the guest can join again with the same code. */
        m_socket     = mgr.DetachSocket(false);
        m_state      = LOBBY_STATE_HOSTING;
        m_lastSendUs = 0;   /* ping the relay right away */
        snprintf(m_notice, sizeof(m_notice),
                 "Your friend left the game. Room %s is still open: they can join again "
                 "with the same code.", m_roomCode);
        fprintf(stderr, "[lobby] opponent gone; room %s stays open for them to rejoin\n", m_roomCode);
    }
    else
    {
        /* Guest: tell the relay we are out, then show what happened. */
        m_socket = mgr.DetachSocket(false);
        if (m_socket && m_roomCode[0] != '\0')
        {
            char leaveReq[128];
            snprintf(leaveReq, sizeof(leaveReq), "{\"cmd\":\"leave\",\"room\":\"%s\"}", m_roomCode);
            NetplaySocketSend(static_cast<NetplaySocket*>(m_socket), leaveReq, strlen(leaveReq));
        }
        char msg[200];
        snprintf(msg, sizeof(msg),
                 "Lost the connection to the host. If their room is still open, join again with "
                 "code %s.", m_roomCode);
        Fail(msg);
    }
}

void NetplayLobby::StartSession(int slot)
{
    m_state = LOBBY_STATE_CONNECTED;
    m_notice[0] = '\0';

    /* Game packets go through the same relay, on the same socket. */
    void* sock = m_socket;
    m_socket = nullptr;  /* ownership moves to NetplayManager */
    NetplayManager::Instance().Connect(0, m_activeAddr, slot, NetplayDelayFromConfig(), sock);
}

void NetplayLobby::Update()
{
    if (m_state == LOBBY_STATE_CONNECTED)
    {
        WatchSession();
        if (m_state == LOBBY_STATE_CONNECTED)
            return;
    }
    if (!m_socket || m_state == LOBBY_STATE_IDLE || m_state == LOBBY_STATE_CONNECTED ||
        m_state == LOBBY_STATE_ERROR)
        return;

    for (;;)
    {
        char buf[1024];
        int n = NetplaySocketRecv(static_cast<NetplaySocket*>(m_socket), buf, sizeof(buf) - 1, nullptr, 0);
        if (n <= 0)
            break;
        buf[n] = '\0';

        if (buf[0] != '{')
        {
            /* A game packet.  If we are hosting, the guest is already talking
               to us, so the relay's "peer_joined" must have been lost. */
            if (m_state == LOBBY_STATE_HOSTING && buf[0] == NETPLAY_PKT_HANDSHAKE)
            {
                fprintf(stderr, "[lobby] opponent's handshake arrived; starting session\n");
                StartSession(0);
                return;
            }
            continue;
        }

        if (strstr(buf, "\"cmd\":\"created\""))
        {
            if (m_state != LOBBY_STATE_REQUESTING)
                continue;  /* answer to a resend */
            JsonString(buf, "room", m_roomCode, sizeof(m_roomCode));
            if (m_tryingLocalRelay)
            {
                m_usingLocalRelay = true;
                m_tryingLocalRelay = false;
                fprintf(stderr, "[lobby] using the relay running on this PC\n");
            }
            m_state = LOBBY_STATE_HOSTING;
            m_lastSendUs = NetplayGetTimeUs();
            fprintf(stderr, "[lobby] room %s created! Waiting for opponent to join...\n", m_roomCode);
        }
        else if (strstr(buf, "\"cmd\":\"peer_joined\""))
        {
            if (m_state != LOBBY_STATE_HOSTING)
                continue;
            fprintf(stderr, "[lobby] opponent joined room %s! Launching game...\n", m_roomCode);
            StartSession(0);
            return;
        }
        else if (strstr(buf, "\"cmd\":\"joined\""))
        {
            if (m_state != LOBBY_STATE_JOINING)
                continue;
            fprintf(stderr, "[lobby] successfully joined room %s! Launching game...\n", m_roomCode);
            StartSession(1);
            return;
        }
        else if (strstr(buf, "\"cmd\":\"peer_left\""))
        {
            if (m_state == LOBBY_STATE_HOSTING)
                continue;   /* our guest left; the room stays open */
            fprintf(stderr, "[lobby] the host closed room %s\n", m_roomCode);
            Fail("The host closed the room.");
            return;
        }
        else if (strstr(buf, "\"cmd\":\"error\""))
        {
            char msg[128] = "Server returned an error";
            JsonString(buf, "message", msg, sizeof(msg));
            Fail(msg);
            return;
        }
        /* "pong" needs no handling: receiving it is the point. */
    }

    u64 now = NetplayGetTimeUs();

    if (m_state == LOBBY_STATE_REQUESTING || m_state == LOBBY_STATE_JOINING)
    {
        if (m_tryingLocalRelay && now - m_requestTimeUs > kLocalRelayTryUs)
        {
            /* No relay on this PC; use the address the player typed. */
            m_tryingLocalRelay = false;
            if (!OpenSocket(m_serverHost, m_serverPort))
                return;
            m_requestTimeUs = now;
            SendRequest();
            fprintf(stderr, "[lobby] no local relay; requested room creation on %s\n", m_activeAddr);
        }
        else if (now - m_requestTimeUs > kRequestTimeoutUs)
        {
            char msg[320];
            if (m_state == LOBBY_STATE_JOINING)
                snprintf(msg, sizeof(msg),
                         "No answer from %s. Check the host's Playit address/port and that their "
                         "relay window is open.", m_activeAddr);
            else
                snprintf(msg, sizeof(msg),
                         "No answer from relay %s. Start run-public-server.bat first.", m_activeAddr);
            Fail(msg);
        }
        else if (now - m_lastSendUs > kResendUs)
        {
            SendRequest();
        }
    }
    else if (m_state == LOBBY_STATE_HOSTING)
    {
        if (now - m_lastSendUs > kHostPingUs)
        {
            char ping[96];
            snprintf(ping, sizeof(ping), "{\"cmd\":\"ping\",\"room\":\"%s\"}", m_roomCode);
            NetplaySocketSend(static_cast<NetplaySocket*>(m_socket), ping, strlen(ping));
            m_lastSendUs = now;
        }
    }
}
