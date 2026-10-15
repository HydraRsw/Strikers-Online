/*
 * lobby.h — Matchmaking room system for Super Mario Strikers netplay.
 *
 * Provides a room-code based interface (e.g. "Create Room" -> 4821,
 * "Join Room" -> enter 4821).  Communicates with the strikers_relay server.
 */

#ifndef PORT_NETPLAY_LOBBY_H
#define PORT_NETPLAY_LOBBY_H

#include "types.h"

#ifdef __cplusplus

enum NetplayLobbyState {
    LOBBY_STATE_IDLE         = 0,
    LOBBY_STATE_REQUESTING   = 1,
    LOBBY_STATE_HOSTING      = 2,  /* Created room, waiting for peer */
    LOBBY_STATE_JOINING      = 3,  /* Connecting to room */
    LOBBY_STATE_CONNECTED    = 4,  /* Peer connected, netplay active */
    LOBBY_STATE_ERROR        = 5,
};

class NetplayLobby {
public:
    static NetplayLobby& Instance();

    /* Host a room on the given relay server.
       Generates a 4-digit code. */
    bool HostRoom(const char* serverHost, u16 serverPort = 7200);

    /* Join a room on the given relay server using the 4-digit code. */
    bool JoinRoom(const char* serverHost, u16 serverPort, const char* roomCode);

    /* Disconnect and leave room. */
    void Leave();

    /* Poll for incoming responses (called per-frame). */
    void Update();

    NetplayLobbyState GetState() const { return m_state; }
    const char*       GetRoomCode() const { return m_roomCode; }
    const char*       GetErrorMessage() const { return m_errorMessage; }
    const char*       GetServerHost() const { return m_serverHost; }
    u16               GetServerPort() const { return m_serverPort; }
    int               GetSlot() const { return m_slot; }
    bool              UsingLocalRelay() const { return m_usingLocalRelay; }
    /* A note for the host's room screen ("your friend left, they can rejoin"). */
    const char*       GetNotice() const { return m_notice; }
    void              ClearError() { m_errorMessage[0] = '\0'; if (m_state == LOBBY_STATE_ERROR) m_state = LOBBY_STATE_IDLE; }

private:
    NetplayLobby();
    ~NetplayLobby();
    NetplayLobby(const NetplayLobby&);
    NetplayLobby& operator=(const NetplayLobby&);

    bool OpenSocket(const char* host, u16 port);
    void SendRequest();
    void StartSession(int slot);
    void Fail(const char* message);
    void WatchSession();

    NetplayLobbyState m_state;
    char              m_roomCode[16];
    char              m_errorMessage[320];
    char              m_notice[200];
    char              m_serverHost[128];   /* relay the user typed in */
    u16               m_serverPort;
    char              m_activeAddr[160];   /* relay we are actually talking to */
    bool              m_tryingLocalRelay;  /* host: trying 127.0.0.1:7200 first */
    bool              m_usingLocalRelay;
    int               m_slot;
    void*             m_socket;
    u64               m_requestTimeUs;
    u64               m_lastSendUs;
};

#endif /* __cplusplus */

#endif /* PORT_NETPLAY_LOBBY_H */
