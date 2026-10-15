/*
 * netplay.h — Rollback netcode for Super Mario Strikers PC port.
 *
 * This file defines the core data structures and interface for GGPO-style
 * rollback networking.  The design is:
 *
 *   1. Each frame, local input is read from the pad and sent to the remote.
 *   2. Remote input is received (or predicted by repeating the last known).
 *   3. Both inputs are fed into the PAD system before FixedUpdateTask ticks.
 *   4. After each tick, game state is saved into a ring buffer.
 *   5. When a remote input arrives that differs from the prediction, the
 *      engine rolls back to the earliest mispredicted frame, replays
 *      forward with corrected inputs, and resumes.
 *
 * The simulation runs at a fixed 50 Hz (g_fFixedUpdateTick = 0.02f).
 * Input packets are 8 bytes per controller per frame.
 */

#ifndef PORT_NETPLAY_H
#define PORT_NETPLAY_H

#include "types.h"
#include <stddef.h>
#include <dolphin/pad.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/*  Constants                                                                */
/* ------------------------------------------------------------------------ */

/* Maximum number of frames we can roll back.  The simulation never runs
   more than this many frames past the last fully confirmed remote input,
   so a rollback can always find its base state in the ring buffer. */
#define NETPLAY_MAX_ROLLBACK_FRAMES 10

/* Ring buffer depth for saved states (must be > NETPLAY_MAX_ROLLBACK_FRAMES). */
#define NETPLAY_STATE_BUFFER_SIZE   (NETPLAY_MAX_ROLLBACK_FRAMES + 2)

/* Ring buffer depth for inputs.  Every slot is tagged with its frame number,
   so stale entries from an earlier lap of the ring are never mistaken for
   real input. */
#define NETPLAY_INPUT_RING_SIZE     128

/* Most inputs one packet can carry.  We resend every input the peer has not
   acknowledged yet, so this has to cover a burst of lost packets. */
#define NETPLAY_MAX_INPUTS_PER_PACKET 32

/* Maximum input delay frames (adds latency but reduces rollbacks). */
#define NETPLAY_MAX_INPUT_DELAY     6

/* Network ports. */
#define NETPLAY_DEFAULT_PORT        7100

/* Magic for the handshake packet. */
#define NETPLAY_MAGIC               0x534D534EU  /* "SMSN" */

/* Protocol version — bump when packet format changes.
   v2: frame-tagged inputs with piggybacked acks, match ids, time sync,
       desync checksums.  v3: online VS screen (character/stadium picks).
       v4: the host sends its starting game state; the guest loads it.
       v5: resync with countdown whenever play resumes (goals, replays, pause).
       Both players need the same build. */
#define NETPLAY_PROTOCOL_VERSION    11

/* ------------------------------------------------------------------------ */
/*  Input packet                                                             */
/* ------------------------------------------------------------------------ */

/*
 * Compact input for one controller, one frame.  Matches the subset of
 * PADStatus that the game actually reads (analogA/analogB are unused).
 * Total: 8 bytes, naturally aligned.
 */
typedef struct NetplayInput {
    u16 button;        /* digital button bitmask (PAD_BUTTON_*) */
    s8  stickX;        /* main stick X  (-128..127) */
    s8  stickY;        /* main stick Y  (-128..127) */
    s8  substickX;     /* C-stick X     (-128..127) */
    s8  substickY;     /* C-stick Y     (-128..127) */
    u8  triggerLeft;   /* L analog      (0..255) */
    u8  triggerRight;  /* R analog      (0..255) */
    u16 remap;         /* the sender's camera angle that turns the stick (v7) */
} NetplayInput;

/* Convert between PADStatus and NetplayInput. */
static inline void NetplayInputFromPAD(NetplayInput* dst, const PADStatus* src)
{
    dst->button       = src->button;
    dst->stickX       = src->stickX;
    dst->stickY       = src->stickY;
    dst->substickX    = src->substickX;
    dst->substickY    = src->substickY;
    dst->triggerLeft  = src->triggerLeft;
    dst->triggerRight = src->triggerRight;
}

static inline void NetplayInputToPAD(PADStatus* dst, const NetplayInput* src)
{
    dst->button       = src->button;
    dst->stickX       = src->stickX;
    dst->stickY       = src->stickY;
    dst->substickX    = src->substickX;
    dst->substickY    = src->substickY;
    dst->triggerLeft  = src->triggerLeft;
    dst->triggerRight = src->triggerRight;
    dst->analogA      = 0;
    dst->analogB      = 0;
    dst->err          = 0;  /* PAD_ERR_NONE */
}

static inline bool NetplayInputEqual(const NetplayInput* a, const NetplayInput* b)
{
    return a->button       == b->button
        && a->stickX       == b->stickX
        && a->stickY       == b->stickY
        && a->substickX    == b->substickX
        && a->substickY    == b->substickY
        && a->triggerLeft  == b->triggerLeft
        && a->triggerRight == b->triggerRight
        && a->remap        == b->remap;
}

/* ------------------------------------------------------------------------ */
/*  Network packet types                                                     */
/* ------------------------------------------------------------------------ */

typedef enum NetplayPacketType {
    NETPLAY_PKT_HANDSHAKE       = 1,
    NETPLAY_PKT_HANDSHAKE_ACK   = 2,
    NETPLAY_PKT_INPUT           = 3,
    NETPLAY_PKT_INPUT_ACK       = 4,   /* unused since v2: acks ride on INPUT */
    NETPLAY_PKT_QUALITY_REPORT  = 5,
    NETPLAY_PKT_KEEPALIVE       = 6,
    NETPLAY_PKT_DISCONNECT      = 7,
    NETPLAY_PKT_START_MATCH     = 8,
    NETPLAY_PKT_START_MATCH_ACK = 9,
    NETPLAY_PKT_QUALITY_REPLY   = 10,
    NETPLAY_PKT_CHECKSUM        = 11,
    NETPLAY_PKT_VS_CHOICE       = 12,
    NETPLAY_PKT_MATCH_ABORT     = 13,
    NETPLAY_PKT_STATE_CHUNK     = 14,
    NETPLAY_PKT_STATE_ACK       = 15,
    NETPLAY_PKT_RESYNC_READY    = 16,
    NETPLAY_PKT_RESYNC_GO       = 17,
    NETPLAY_PKT_LEAVE           = 18,  /* agree on the frame where gameplay stops */
    NETPLAY_PKT_DUMP_CHUNK      = 19,
    NETPLAY_PKT_SKIP            = 20,  /* a player skipped the replay/cutscene: skip it on both */  /* guest's desync dump, sent to the host (same layout as STATE_CHUNK) */
} NetplayPacketType;

/* Starting game state is sent in pieces this big (fits our 1 KB receive buffers). */
#define NETPLAY_STATE_CHUNK_BYTES   900
#define NETPLAY_STATE_MAX_BYTES     (256 * 1024)

/* One player's picks on the online VS screen. */
typedef struct NetplayVsChoice {
    u8 captain;     /* eTeamID, 0xFF = not chosen yet */
    u8 sidekick;    /* eSidekickID */
    u8 stadium;     /* eStadiumID (the host's pick is the one used) */
    u8 ready;       /* 1 = locked in */
    u8 atFrontEnd;  /* 1 = this game is on the main menu and can start a match */
    /* The host's match rules, so the guest sees them while choosing (the
       guest's own are ignored; the host's are sent again with the start). */
    u8 ruleTimeSec10;   /* match length / 10 s (12 = 2 minutes) */
    u8 ruleSkill;       /* 1 rookie .. 4 legend */
    u8 ruleFlags;       /* NETPLAY_RULE_* */
    u8 ruleCustom;      /* custom items cheat, 0 = off */
} NetplayVsChoice;

enum
{
    NETPLAY_RULE_ITEMS    = 1 << 0,
    NETPLAY_RULE_S2S      = 1 << 1,
    NETPLAY_RULE_BOWSER   = 1 << 2,
    NETPLAY_RULE_INFINITE = 1 << 3,
    NETPLAY_RULE_STUNNED  = 1 << 4,
};

/* Wire format: all packets start with this header.  Game packets put the
   current match id in matchId so packets from an earlier match are ignored. */
#pragma pack(push, 1)

typedef struct NetplayPacketHeader {
    u8  type;           /* NetplayPacketType */
    u8  matchId;        /* match the packet belongs to (was _pad in v1) */
    u16 sequence;       /* monotonically increasing per sender */
} NetplayPacketHeader;

typedef struct NetplayHandshakePacket {
    NetplayPacketHeader hdr;
    u32 magic;          /* NETPLAY_MAGIC */
    u32 version;        /* NETPLAY_PROTOCOL_VERSION */
    u32 rngSeed;        /* agreed-upon RNG seed */
    u8  playerSlot;     /* 0 or 1 */
    u8  inputDelay;     /* requested input delay frames */
    u8  _pad[2];
} NetplayHandshakePacket;

typedef struct NetplayStartMatchPacket {
    NetplayPacketHeader hdr;   /* hdr.matchId = id of the match being started */
    u8  team0;
    u8  team1;
    u8  stadium;
    u8  sidekick0;
    u8  sidekick1;
    u8  inputDelay;            /* delay both sides use for this match */
    u8  maxRollback;           /* 0 = lockstep (wait for inputs, never guess) */
    u8  _pad;
    u32 rngSeed;               /* RNG seed both sides apply at frame 0 */
    u8  settingsLen;           /* v8: the host's gameplay options (see GameInfo.cpp) */
    u8  settings[63];
} NetplayStartMatchPacket;

typedef struct NetplayStartMatchAckPacket {
    NetplayPacketHeader hdr;   /* hdr.matchId = id being acknowledged */
} NetplayStartMatchAckPacket;

typedef struct NetplayInputPacket {
    NetplayPacketHeader hdr;
    u32 startFrame;     /* frame number of the first input in this packet */
    u32 ackFrame;       /* sender's contiguous confirmed frame of OUR inputs, plus 1 (0 = none) */
    u8  inputCount;     /* number of inputs that follow */
    u8  segment;        /* resync epoch the inputs belong to */
    u8  _pad[2];
    /* Only inputCount entries are sent on the wire. */
    NetplayInput inputs[NETPLAY_MAX_INPUTS_PER_PACKET];
} NetplayInputPacket;

#define NETPLAY_INPUT_PACKET_HEADER_SIZE \
    (sizeof(NetplayInputPacket) - sizeof(NetplayInput) * NETPLAY_MAX_INPUTS_PER_PACKET)

typedef struct NetplayInputAckPacket {
    NetplayPacketHeader hdr;
    u32 ackedFrame;     /* latest frame the sender has confirmed */
} NetplayInputAckPacket;

typedef struct NetplayQualityReportPacket {
    NetplayPacketHeader hdr;
    s32 frameAdvantage; /* sender's estimate of (remote frame - local frame) */
    u32 sendTimeUs;     /* sender clock, echoed back for RTT */
} NetplayQualityReportPacket;

typedef struct NetplayQualityReplyPacket {
    NetplayPacketHeader hdr;
    u32 echoTimeUs;
} NetplayQualityReplyPacket;

typedef struct NetplayVsChoicePacket {
    NetplayPacketHeader hdr;
    NetplayVsChoice     choice;
    u8                  _pad[3];
} NetplayVsChoicePacket;

typedef struct NetplayMatchAbortPacket {
    NetplayPacketHeader hdr;   /* hdr.matchId = the match that could not start */
    s8  reason;                /* PortDebugStartMatchResult code */
    u8  _pad[3];
} NetplayMatchAbortPacket;

typedef struct NetplayStateChunkPacket {
    NetplayPacketHeader hdr;
    u32 totalSize;      /* whole state, bytes */
    u32 offset;         /* where this piece goes */
    u16 length;         /* bytes of data that follow */
    u8  epoch;          /* 0 = match start, otherwise the resync epoch */
    u8  _pad;
    u32 frame;          /* first frame to simulate after loading it */
    u8  data[NETPLAY_STATE_CHUNK_BYTES];
} NetplayStateChunkPacket;

#define NETPLAY_STATE_CHUNK_HEADER_SIZE \
    (sizeof(NetplayStateChunkPacket) - NETPLAY_STATE_CHUNK_BYTES)

typedef struct NetplayStateAckPacket {
    NetplayPacketHeader hdr;   /* hdr.matchId = whose starting state we have */
    u8  epoch;
    u8  _pad[3];
} NetplayStateAckPacket;

typedef struct NetplayResyncPacket {
    NetplayPacketHeader hdr;
    u8  epoch;
    u8  _pad[3];
    u32 value;          /* READY: sender's next frame; GO: countdown left, ms */
} NetplayResyncPacket;

/* Leaving gameplay (goal replay, cutscene, pause, Super Strike...) happens at
   the same game frame on both PCs: whoever asks first proposes a last frame,
   the other side answers with its own proposal or "none", and both stop after
   the later of the two. */
typedef struct NetplayLeavePacket {
    NetplayPacketHeader hdr;
    u8  epoch;          /* play segment (resync epoch) the request belongs to */
    u8  needReply;      /* 1 = sender still waits for our answer */
    u8  recovery;       /* 1 = a stop to fix a desync: both resync at once */
    u8  _pad;
    s32 myStop;         /* sender's proposed last frame, -1 = sender has no request */
    s32 seenStop;       /* our proposal as the sender knows it, -2 = not seen yet */
} NetplayLeavePacket;

typedef struct NetplayChecksumPacket {
    NetplayPacketHeader hdr;
    u32 frame;
    u32 checksum;
} NetplayChecksumPacket;

#pragma pack(pop)

/* ------------------------------------------------------------------------ */
/*  Session state                                                            */
/* ------------------------------------------------------------------------ */

typedef enum NetplaySessionState {
    NETPLAY_STATE_IDLE         = 0,   /* not connected */
    NETPLAY_STATE_CONNECTING   = 1,   /* handshake in progress */
    NETPLAY_STATE_RUNNING      = 2,   /* game in progress */
    NETPLAY_STATE_DISCONNECTED = 3,   /* peer left */
} NetplaySessionState;

/* Ball-animation test switches (see netplay.cpp):
   0 clocks per rendered frame, 1 stadium per rendered frame, 2 no gameplay
   RNG of its own, 3 flick history per rendered frame, 4 button hold times by
   real time. */
int  PortNetplayDiag(int i);
void PortNetplaySetDiag(int i, int on);

#ifdef __cplusplus
}  /* extern "C" */
#endif

/* ------------------------------------------------------------------------ */
/*  C++ interface (NetplayManager)                                           */
/* ------------------------------------------------------------------------ */

#ifdef __cplusplus

class NetplayManager {
public:
    /* Singleton access. */
    static NetplayManager& Instance();

    /* Start a netplay session.  Call before entering gameplay state.
       localPort:  UDP port to bind.
       remoteAddr: "ip:port" of the peer.
       localSlot:  0 = player 1 (left), 1 = player 2 (right).
       inputDelay: frames of intentional input delay (reduces rollbacks).
       existingSocket: optional pre-created socket (e.g. from NetplayLobby). */
    bool Connect(u16 localPort, const char* remoteAddr,
                 int localSlot, int inputDelay,
                 void* existingSocket = nullptr);

    /* Pump network packets and handle handshakes / keep-alives. */
    void Update();

    /* Send handshake packet. */
    void SendHandshake();

    /* Host only: tell the guest to start a match, and reset the frame
       counters for it.  Resent until the guest acknowledges. */
    void SendStartMatch(u8 team0, u8 team1, u8 stadium, u8 sidekick0, u8 sidekick1);

    /* Tear down the session. */
    void Disconnect();

    /* End the session but keep the socket, for the lobby to reuse (a host
       keeps its room open so the guest can come back).  Returns the socket. */
    void* DetachSocket(bool tellPeer);

    /* Seconds since anything arrived from the opponent (0 when not running). */
    float GetSecondsSinceLastPacket() const;

    /* --- Per-frame interface (called from FixedUpdateTask) --- */

    /* Called once per rendered frame before the fixed-update loop: samples
       the local player's controller (always physical port 0). */
    void CaptureLocalInput();

    /* Called once per simulation tick, before game logic runs.
       Performs any pending rollback, then injects this frame's inputs into
       the PAD system.  Returns false if the frame must not run yet (waiting
       for the peer, or slowing down to let it catch up). */
    bool PreUpdate(u32 simFrame);

    /* Called once per simulation tick, after game logic runs.
       Saves a snapshot of the current game state. */
    void PostUpdate(u32 simFrame);

    /* Called after the fixed-update loop: drops the netplay pad overrides
       so the next CaptureLocalInput reads the real controller. */
    void ReleaseVirtualPads();

    /* Called when a rendered frame ran no simulation tick (stalled): puts the
       last tick's inputs back on the pads, so code that reads the pads at
       render rate still sees the synced inputs rather than this PC's
       controller. */
    void ReapplyLastInputs();

    /* Around the fixed-update loop: gameplay gets its own copy of the game's
       random number generators.  Render-rate code (effects, camera, crowd)
       draws from the same generators, and a PC drawing more frames used up
       more numbers, which changed gameplay. */
    void BeginSimRng();
    void EndSimRng();

    /* Frame counter for the current match (reset at every match start). */
    u32  GetSimFrame() const { return m_simFrame; }
    void AdvanceSimFrame() { m_simFrame++; }
    /* Inside a game frame (gameplay's random numbers in use). */
    bool InSimRng() const { return m_inSimRng; }
    u32  GetMatchSeed() const { return m_rngSeed; }

    /* --- State queries --- */

    NetplaySessionState GetState() const { return m_state; }
    bool IsActive() const { return m_state == NETPLAY_STATE_RUNNING; }
    int  GetLocalSlot() const { return m_localSlot; }
    int  GetRemoteSlot() const { return 1 - m_localSlot; }
    u32  GetPingMs() const { return m_pingUs / 1000; }
    int  GetRollbackFrames() const { return m_lastRollbackSize; }
    int  GetInputDelay() const { return m_inputDelay; }
    int  GetMaxRollback() const { return m_maxRollback; }
    int  GetFrameAdvantage() const { return m_localFrameAdvantage; }
    bool IsDesynced() const { return m_desyncFrame >= 0; }
    u8   GetMatchId() const { return m_matchId; }
    int  GetResyncCount() const { return m_doneEpoch; }   /* resyncs finished this match */

    /* --- Resync when play resumes ---
       Goal celebrations, replays and the pause menu are not fixed frames: each
       PC plays them by itself, for its own length of time.  So every time play
       resumes, both games wait for each other, the host sends its state, and a
       countdown starts both at once. */
    enum ResyncPhase { RESYNC_NONE = 0, RESYNC_WAITING = 1, RESYNC_TRANSFER = 2, RESYNC_COUNTDOWN = 3 };
    void OnLeaveGameplay(u32 lastSimulatedFrame);   /* a frame just asked to leave gameplay */
    void OnEnterGameplay();                          /* the game is back in gameplay */
    void MirrorLocalPadForMenus();                   /* outside gameplay: guest's pad works too */
    int   GetResyncPhase() const { return m_resyncPhase; }
    bool  IsLocalReadyForResync() const { return m_localReadyForResync; }
    bool  IsPeerReadyForResync() const { return m_peerReadyEpoch == m_resyncEpoch && m_resyncPhase != RESYNC_NONE; }
    float GetCountdownSeconds() const;

    /* --- Coordinated leave ---
       Called from nlTaskManager::SetNextState.  While a match runs online,
       leaving gameplay is held back until both games reach the same frame.
       Returns true if the request was taken over (do not change state now). */
    bool InterceptStateChange(u32 currState, u32 nextState);
    /* The opponent has stopped play (pause...) and this game waits for it. */
    bool IsWaitingForOpponentBreak() const;
    /* The opponent is in gameplay waiting for this game (which is still in
       the intro, a replay or a cutscene): skip it. */
    bool IsOpponentWaiting() const;
    /* Skip the current replay/cutscene: this player pressed A/B/START, the
       opponent did, or the opponent waits.  Pressing here tells the opponent. */
    bool SkipWanted();
    void LocalSkip();

    /* --- Online VS screen --- */
    void SetVsChoice(const NetplayVsChoice& choice);
    const NetplayVsChoice& GetVsChoice() const { return m_vsLocal; }
    /* False until the opponent's picks have arrived (or if they went quiet). */
    bool GetRemoteVsChoice(NetplayVsChoice* out) const;

    /* This game could not start the match: tell the opponent so their game
       backs out instead of waiting forever. */
    void SendMatchAbort(int reason);
    /* True once if the opponent reported that the current match could not
       start; *reason gets their code. */
    bool TakeRemoteAbort(int* reason);
    int  GetDesyncFrame() const { return m_desyncFrame; }

    /* Kept for compatibility: rollback now happens inside PreUpdate.
       Rolls back now if a misprediction is pending; returns frames resimulated. */
    int  CheckAndPerformRollback(u32 currentFrame);

    NetplayManager();
    ~NetplayManager();

private:
    NetplayManager(const NetplayManager&);
    NetplayManager& operator=(const NetplayManager&);

    struct InputSlot {
        s32          frame;   /* frame this slot holds, -1 = empty */
        NetplayInput input;
    };

    struct SavedState {
        u8*  data;
        u32  size;
        u32  capacity;
        s32  frame;
        u32  checksum;
    };

    struct ChecksumEntry {
        s32 frame;
        u32 checksum;
    };

    /* --- Match lifecycle --- */
    void ResetForMatch(u8 matchId, u32 rngSeed, int inputDelay);
    void SeedRngForMatch();

    /* --- Networking --- */
    void SendGamePacket(void* pkt, size_t len, u8 type);
    void SendInputs(bool evenDuringResync = false);
    void SendKeepAlive();
    void SendQualityReport();
    void SendChecksums();
    void SendVsChoice();
    void SendInitialState();
    void BeginResync();
    void StepResync();
    void FinishResync();
    void SendResync(u8 type, u32 value);
    void ResetLeave();
    void SendLeave();
    void HandleLeave(const NetplayLeavePacket* lp);
    bool LeaveFinalKnown() const;
    s32  LeaveFinal() const;
    s32  LeaveInputCap() const;
    bool StepLeave(s32 frame);   /* true = this frame must not run */
    void VerifyLoadedState(const u8* expected, u32 size, const char* what);
    void WriteSyncDump(int epoch);
    static void CopySavedState(SavedState& dst, const SavedState& src);
    void HandleStateChunk(const NetplayStateChunkPacket* pkt, int n);
    void ReceivePackets();
    void HandleRemoteInput(s32 frame, const NetplayInput& input);

    /* --- Rollback --- */
    NetplayInput PredictInput() const;
    void SaveSnapshot(SavedState& ss, s32 frame);
    int  DoRollback();
    void InjectInputs(s32 frame);
    void CheckDesync();
    void KeepChecksumState(s32 frame, const SavedState& ss);
    void DumpDesync(s32 frame, u32 remoteChecksum);

    static int Ring(s32 frame) { return static_cast<int>(static_cast<u32>(frame) % NETPLAY_INPUT_RING_SIZE); }
    static int StateRing(s32 frame) { return static_cast<int>(static_cast<u32>(frame) % NETPLAY_STATE_BUFFER_SIZE); }

    /* --- State --- */
    NetplaySessionState m_state;
    int   m_localSlot;       /* 0 or 1 */
    int   m_inputDelay;      /* frames of intentional delay */
    int   m_maxRollback;     /* frames we may run ahead of confirmed input (0 = lockstep) */
    int   m_configDelay;     /* strikers.ini "delay": the minimum */
    u32   m_rngSeed;         /* agreed seed for determinism */

    /* Match. */
    u8    m_matchId;         /* current match (0 = none yet) */
    u32   m_simFrame;        /* next frame FixedUpdateTask will simulate */
    bool  m_rngSeeded;

    /* Host: pending START_MATCH until acknowledged. */
    NetplayStartMatchPacket m_pendingStart;
    bool  m_startPending;
    u64   m_startLastSendUs;
    int   m_startSends;

    /* Inputs. */
    NetplayInput m_capturedLocal;
    InputSlot m_localInputs[NETPLAY_INPUT_RING_SIZE];
    InputSlot m_remoteInputs[NETPLAY_INPUT_RING_SIZE];      /* confirmed only */
    InputSlot m_usedRemoteInputs[NETPLAY_INPUT_RING_SIZE];  /* what the sim actually ran with */
    s32 m_localInputLatest;   /* newest frame we have local input for */
    s32 m_peerAckedFrame;     /* newest frame of OUR inputs the peer has (contiguous) */
    s32 m_remoteLatest;       /* newest remote frame received */
    s32 m_remoteConfirmed;    /* every remote input up to here has arrived */
    s32 m_lastSimFrame;       /* newest frame simulated */
    s32 m_firstMispredict;    /* earliest simulated frame whose remote input was wrong, -1 = none */

    /* Time sync. */
    int   m_localFrameAdvantage;
    int   m_remoteFrameAdvantage;
    float m_advantageAvg;
    int   m_framesToStall;
    s32   m_lastStallFrame;
    s32   m_lastSyncCheckFrame;
    u64   m_lastQualityUs;

    /* Network stats. */
    u32 m_pingUs;
    int m_lastRollbackSize;
    u64 m_lastRecvUs;
    u64 m_lastKeepAliveUs;

    /* Online VS screen. */
    NetplayVsChoice m_vsLocal;
    NetplayVsChoice m_vsRemote;
    bool m_vsRemoteKnown;
    u64  m_vsRemoteUs;
    u64  m_vsLastSendUs;
    bool m_remoteAbort;
    int  m_remoteAbortReason;

    /* Starting state: the host sends its frame-0 state, the guest loads it,
       so both matches start from identical bytes however they loaded. */
    bool m_initialStateReady;    /* host: state captured and being sent */
    bool m_initialStateAcked;    /* host: guest has it */
    bool m_initialStateLoaded;   /* guest: loaded it */
    u64  m_initialStateSendUs;
    u8*  m_rxState;              /* guest: reassembly buffer */
    u32  m_rxStateSize;
    u32  m_rxStateCapacity;
    u8   m_rxChunkHave[(NETPLAY_STATE_MAX_BYTES / NETPLAY_STATE_CHUNK_BYTES) + 1];
    u32  m_rxChunksNeeded;
    u32  m_rxChunksHave;
    u64  m_waitStateSinceUs;
    const SavedState* m_txState;   /* what SendInitialState sends */
    u8   m_txEpoch;
    s32  m_txFrame;
    u8   m_rxEpoch;
    s32  m_rxFrame;

    /* Resync. */
    int  m_resyncPhase;
    u8   m_resyncEpoch;           /* epoch in progress (m_doneEpoch + 1) */
    u8   m_doneEpoch;             /* last finished; tags input packets */
    u8   m_peerReadyEpoch;
    bool m_localReadyForResync;
    bool m_resyncOnReturn;
    bool m_resyncLoaded;          /* guest: host's state is loaded */
    s32  m_resyncFrame;
    s32  m_noRollbackBefore;      /* rollbacks may not reach before this frame */
    s32  m_segmentStartFrame;
    u64  m_resyncSendUs;
    u64  m_countdownEndUs;
    u64  m_breakStartUs;
    u32  m_countdownMs;
    u64  m_gameplayEnteredUs;     /* when the game last (re)entered gameplay; 0 = not yet */
    u64  m_lastFrameRunUs;        /* when the last game frame ran (break length) */
    bool m_holdTimesReset;        /* button hold times zeroed for this match */

    /* Stutter statistics, logged every 10 s. */
    u64  m_statSinceUs;
    u32  m_statFrameAtStart;
    int  m_statWaitInput;
    int  m_statWaitSync;
    SavedState m_resyncState;

    /* Coordinated leave (see InterceptStateChange). */
    bool m_leaveDecided;          /* our answer for this segment is fixed */
    s32  m_leaveMyStop;           /* our proposed last frame, -1 = none */
    bool m_leavePeerKnown;        /* the opponent's answer has arrived */
    s32  m_leavePeerStop;         /* its proposed last frame, -1 = none */
    s32  m_leavePeerSeen;         /* what the opponent knows of our answer (-2 = nothing) */
    u32  m_leaveState;            /* state this game asked for, 0 = none */
    bool m_leaveDone;             /* reached the last frame (applied or holding) */
    u64  m_leaveHoldSinceUs;      /* holding at the last frame without a state of our own */
    u64  m_leaveSendUs;
    bool m_applyingState;         /* our own SetNextState call: let it through */
    bool m_leaveRecovery;         /* this stop repairs a desync: resync right away */
    u64  m_lastRecoveryUs;
    void RequestRecoveryStop(s32 desyncFrame);
    const SavedState* m_reloadState;  /* load this before the next frame runs */

    /* Guest -> host copies of debug dumps. */
    void QueueDumpForHost(u8 kind, u32 id, const u8* data, u32 size);
    void PumpDumpToHost();
    u8*  m_dumpTx;
    u32  m_dumpTxSize;
    u32  m_dumpTxCursor;
    u32  m_dumpTxId;
    int  m_dumpTxPasses;
    u8   m_dumpTxKind;

    /* Shared skip. */
    bool m_peerSkip;
    u8   m_peerSkipEpoch;
    u8   m_localSkipEpoch;
    int  m_localSkipSends;
    u64  m_localSkipSendUs;
    u16  m_skipPrevButtons;
    u64  m_skipLastCallUs;

    /* Desync detection. */
    ChecksumEntry m_localSums[16];
    ChecksumEntry m_remoteSums[16];
    s32 m_lastChecksumSent;
    int m_desyncFrame;
    SavedState m_sumStates[4];   /* full states of recent checksum frames, for desync dumps */

    /* Saved game states for rollback (opaque blobs). */
    SavedState m_savedStates[NETPLAY_STATE_BUFFER_SIZE];
    SavedState m_preFrameZeroState;

    /* Gameplay's random generator state while render code runs. */
    unsigned long m_simMt[624];
    int           m_simMti;
    unsigned int  m_simNlSeed;
    bool          m_simRngValid;
    unsigned long m_renderMt[624];
    int           m_renderMti;
    unsigned int  m_renderNlSeed;
    bool          m_inSimRng;

    /* UDP socket handle (platform-specific, implemented in netplay_transport). */
    void* m_socket;

    /* Packet sequence counter. */
    u16 m_sendSequence;

    /* Handshake retry state. */
    u64 m_lastHandshakeUs;
    int m_handshakeRetries;
};

#endif /* __cplusplus */

#endif /* PORT_NETPLAY_H */
