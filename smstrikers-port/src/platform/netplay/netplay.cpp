/*
 * netplay.cpp — GGPO-style rollback netcode manager.
 *
 * Per simulation tick:
 *   1. PreUpdate: receive packets; if a remote input arrived that differs
 *      from what we predicted for an already-simulated frame, load the state
 *      from just before that frame and resimulate up to the present.
 *   2. PreUpdate: store this frame's local input `inputDelay` frames in the
 *      future, send every input the peer has not acknowledged, and inject
 *      this frame's local + remote (confirmed or predicted) input.
 *   3. The game ticks.
 *   4. PostUpdate: save the game state for this frame.
 *
 * Every input slot is tagged with its frame number, so nothing from an
 * earlier lap of a ring buffer can be mistaken for real input.  The game
 * never runs more than NETPLAY_MAX_ROLLBACK_FRAMES past the last frame for
 * which every remote input has arrived, so a rollback always finds its base
 * state.  Both sides exchange their frame advantage and the one that is
 * ahead slows down a little, which keeps rollbacks short.
 */

#include "port/netplay/netplay.h"
#include "port/netplay/savestate.h"
#include "port/netplay/transport.h"

#include "Game/FixedUpdateTask.h"
#include "Game/Pad/FlickDetection.h"
#include "Game/Sys/clock.h"
#include "Game/WorldManager.h"
#include "NL/nlTask.h"
#include "NL/platpad.h"
#include "NL/globalpad.h"
#include "port/overlay.h"

#include <dolphin/pad.h>
#include <SDL3/SDL_keyboard.h>   /* SDL_GetKeyboardFocus: is this window the active one? */
#include <cmath>
#include <math.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern float g_fFixedUpdateTick;
extern void UseFixedUpdatePad();
extern "C" void PortPadStepFixedHoldTimes(float dt);
extern "C" void PortPadResetFixedHoldTimes(void);
extern void UpdatePlatPad(float);

extern unsigned int nlDefaultSeed;
extern "C" void netplay_mt_set_state(const unsigned long* in_mt, int in_mti);
extern "C" unsigned long netplay_mt_get_state(unsigned long* out_mt, int* out_mti);

static const u64 kKeepAliveIntervalUs   = 500000;    /* 0.5 s */
static const u64 kQualityIntervalUs     = 200000;    /* 0.2 s */
static const u64 kStartResendUs         = 250000;    /* 0.25 s */
static const int kStartMaxSends         = 40;        /* ~10 s */
static const u64 kPeerTimeoutUs         = 10000000;  /* 10 s without any packet */
static const int kChecksumInterval      = 30;        /* frames */
static const int kSyncCheckInterval     = 20;        /* frames */
static const int kMaxStallPerCheck      = 8;
static const float kFrameMs             = 20.0f;     /* 50 Hz simulation */
/* Ball-animation test switches (Netplay tab): each one puts back the offline
   behaviour of one online change.  They break sync; they are only for finding
   which change makes the held ball look wrong. */
static int s_diag[8];
extern "C" int PortNetplayDiag(int i) { return (i >= 0 && i < 8) ? s_diag[i] : 0; }
extern "C" void PortNetplaySetDiag(int i, int on) { if (i >= 0 && i < 8) s_diag[i] = on; }

static const u64 kResyncResendUs        = 200000;    /* 0.2 s */
static const u32 kCountdownLongMs       = 3000;      /* after a goal, replay or pause */
static const u32 kCountdownShortMs      = 0;         /* after a short break (kickoff, hit freeze...) */
static const u64 kLongBreakUs           = 2000000;   /* a break this long gets the full countdown */
static const u64 kSettleUs              = 1000000;   /* gameplay must hold this long before frames run */
static const u64 kLeaveResendUs         = 50000;     /* 0.05 s */
static const u64 kLeaveHoldUs           = 2000000;   /* held at the agreed frame without a break of our own */

/* strikers.ini "rollback = N": how many frames the game may guess ahead of the
   opponent's input.  0 (the default) is lockstep: it waits for the real input
   and never rolls back.  Rollback needs a save state that captures the whole
   game, and this port's does not yet, so every rollback is a partial load. */
static int RollbackFromConfig()
{
    const char* s = std::getenv("STRIKERS_NETPLAY_ROLLBACK");
    if (!s || !*s) s = std::getenv("STRIKERS_ROLLBACK");
    int n = (s && *s) ? atoi(s) : 0;
    if (n < 0) n = 0;
    if (n > NETPLAY_MAX_ROLLBACK_FRAMES) n = NETPLAY_MAX_ROLLBACK_FRAMES;
    return n;
}

/* Lockstep needs the opponent's input to arrive within the input delay:
   half the round trip plus some margin for jitter, in 20 ms frames. */
static int DelayForPing(u32 pingUs)
{
    u32 oneWayMs = pingUs / 2000;
    int frames = static_cast<int>((oneWayMs + 15 + 19) / 20);
    if (frames < 2) frames = 2;
    if (frames > NETPLAY_MAX_INPUT_DELAY) frames = NETPLAY_MAX_INPUT_DELAY;
    return frames;
}

static bool TaskManagerLeavingGameplay()
{
    return nlTaskManager::m_pInstance != nullptr && nlTaskManager::m_pInstance->m_PendingState != 2;
}

/* Port 1 must look connected while a session runs (see ReleaseVirtualPads). */
static void PlugInPort1()
{
    PADStatus pad;
    memset(&pad, 0, sizeof(pad));
    pad.err = 0;  /* PAD_ERR_NONE: a controller is there */
    PADSetVirtualStatus(1, &pad);
}

extern "C" void PortSetPadRemap(int port, unsigned short remap);
extern "C" unsigned short PortGetPadRemap(int port);
extern "C" void PortSetPadRemapActive(int on);
extern "C" unsigned short PortCameraJoystickRemap(void);

extern "C" int  PortGetMatchSettings(void* out, int max);
extern "C" void PortApplyMatchSettings(const void* in, int n);
extern "C" void PortRestoreOwnSettings(void);

static NetplayInput NeutralInput()
{
    NetplayInput in;
    memset(&in, 0, sizeof(in));
    return in;
}

/* ======================================================================== */
/*  Singleton                                                                */
/* ======================================================================== */

NetplayManager& NetplayManager::Instance()
{
    static NetplayManager s_instance;
    return s_instance;
}

/* ======================================================================== */
/*  Constructor                                                              */
/* ======================================================================== */

NetplayManager::NetplayManager()
    : m_state(NETPLAY_STATE_IDLE)
    , m_localSlot(0)
    , m_inputDelay(2)
    , m_maxRollback(0)
    , m_configDelay(2)
    , m_rngSeed(12345)
    , m_matchId(0)
    , m_simFrame(0)
    , m_rngSeeded(false)
    , m_startPending(false)
    , m_startLastSendUs(0)
    , m_startSends(0)
    , m_localInputLatest(-1)
    , m_peerAckedFrame(-1)
    , m_remoteLatest(-1)
    , m_remoteConfirmed(-1)
    , m_lastSimFrame(-1)
    , m_firstMispredict(-1)
    , m_localFrameAdvantage(0)
    , m_remoteFrameAdvantage(0)
    , m_advantageAvg(0.0f)
    , m_framesToStall(0)
    , m_lastStallFrame(-1000)
    , m_lastSyncCheckFrame(0)
    , m_lastQualityUs(0)
    , m_pingUs(0)
    , m_lastRollbackSize(0)
    , m_lastRecvUs(0)
    , m_lastKeepAliveUs(0)
    , m_lastChecksumSent(-kChecksumInterval)
    , m_desyncFrame(-1)
    , m_socket(nullptr)
    , m_sendSequence(0)
    , m_lastHandshakeUs(0)
    , m_handshakeRetries(0)
{
    memset(&m_pendingStart, 0, sizeof(m_pendingStart));
    m_capturedLocal = NeutralInput();
    m_dumpTx = nullptr;
    m_peerSkip = false;
    m_peerSkipEpoch = 0;
    m_localSkipEpoch = 0;
    m_localSkipSends = 0;
    m_localSkipSendUs = 0;
    m_skipPrevButtons = 0;
    m_skipLastCallUs = 0;
    m_dumpTxSize = m_dumpTxCursor = m_dumpTxId = 0;
    m_dumpTxPasses = 0;
    m_dumpTxKind = 0;
    m_reloadState = nullptr;
    m_applyingState = false;
    m_leaveRecovery = false;
    m_lastRecoveryUs = 0;

    memset(&m_vsLocal, 0, sizeof(m_vsLocal));
    memset(&m_vsRemote, 0, sizeof(m_vsRemote));
    m_vsLocal.captain = 0xFF;
    m_vsRemoteKnown = false;
    m_vsRemoteUs = 0;
    m_vsLastSendUs = 0;
    m_remoteAbort = false;
    m_remoteAbortReason = 0;
    m_rxState = nullptr;
    m_rxStateSize = 0;
    m_rxStateCapacity = 0;
    m_simRngValid = false;
    m_inSimRng = false;
    m_resyncState.data = nullptr;
    m_resyncState.size = 0;
    m_resyncState.capacity = 0;
    m_resyncState.frame = -2;
    m_resyncState.checksum = 0;
    m_simMti = 0;
    m_simNlSeed = 0;
    m_renderMti = 0;
    m_renderNlSeed = 0;

    for (int i = 0; i < NETPLAY_STATE_BUFFER_SIZE; i++)
    {
        m_savedStates[i].data     = nullptr;
        m_savedStates[i].size     = 0;
        m_savedStates[i].capacity = 0;
        m_savedStates[i].frame    = -2;
        m_savedStates[i].checksum = 0;
    }
    for (int i = 0; i < 4; i++)
    {
        m_sumStates[i].data     = nullptr;
        m_sumStates[i].size     = 0;
        m_sumStates[i].capacity = 0;
        m_sumStates[i].frame    = -2;
        m_sumStates[i].checksum = 0;
    }
    m_preFrameZeroState.data     = nullptr;
    m_preFrameZeroState.size     = 0;
    m_preFrameZeroState.capacity = 0;
    m_preFrameZeroState.frame    = -2;
    m_preFrameZeroState.checksum = 0;

    ResetForMatch(0, m_rngSeed, m_inputDelay);
}

NetplayManager::~NetplayManager()
{
    Disconnect();
    for (int i = 0; i < NETPLAY_STATE_BUFFER_SIZE; i++)
        free(m_savedStates[i].data);
    free(m_preFrameZeroState.data);
    free(m_rxState);
    for (int i = 0; i < 4; i++)
        free(m_sumStates[i].data);
    free(m_resyncState.data);
}

/* ======================================================================== */
/*  Match lifecycle                                                          */
/* ======================================================================== */

void NetplayManager::ResetForMatch(u8 matchId, u32 rngSeed, int inputDelay)
{
    if (inputDelay < 0) inputDelay = 0;
    if (inputDelay > NETPLAY_MAX_INPUT_DELAY) inputDelay = NETPLAY_MAX_INPUT_DELAY;

    m_matchId    = matchId;
    m_rngSeed    = rngSeed;
    m_remoteAbort = false;
    m_inputDelay = inputDelay;
    m_simFrame   = 0;
    m_rngSeeded  = false;

    for (int i = 0; i < NETPLAY_INPUT_RING_SIZE; i++)
    {
        m_localInputs[i].frame      = -1;
        m_remoteInputs[i].frame     = -1;
        m_usedRemoteInputs[i].frame = -1;
        m_localInputs[i].input      = NeutralInput();
        m_remoteInputs[i].input     = NeutralInput();
        m_usedRemoteInputs[i].input = NeutralInput();
    }

    /* The first `inputDelay` frames have no local input yet: they are neutral
       by definition, and are sent to the peer like any other input. */
    for (int f = 0; f < m_inputDelay; f++)
    {
        m_localInputs[Ring(f)].frame = f;
        m_localInputs[Ring(f)].input = NeutralInput();
    }
    m_localInputLatest = m_inputDelay - 1;

    m_peerAckedFrame  = -1;
    m_remoteLatest    = -1;
    m_remoteConfirmed = -1;
    m_lastSimFrame    = -1;
    m_firstMispredict = -1;

    m_localFrameAdvantage  = 0;
    m_remoteFrameAdvantage = 0;
    m_advantageAvg         = 0.0f;
    m_framesToStall        = 0;
    m_lastStallFrame       = -1000;
    m_lastSyncCheckFrame   = 0;
    m_lastRollbackSize     = 0;

    for (int i = 0; i < 16; i++)
    {
        m_localSums[i].frame  = -1;
        m_remoteSums[i].frame = -1;
    }
    m_lastChecksumSent = -kChecksumInterval;
    m_desyncFrame      = -1;
    for (int i = 0; i < 4; i++)
        m_sumStates[i].frame = -2;

    for (int i = 0; i < NETPLAY_STATE_BUFFER_SIZE; i++)
        m_savedStates[i].frame = -2;
    m_preFrameZeroState.frame = -2;

    m_simRngValid        = false;   /* gameplay RNG starts from the match seed */
    m_holdTimesReset     = false;
    m_statSinceUs        = 0;
    m_statFrameAtStart   = 0;
    m_statWaitInput      = 0;
    m_statWaitSync       = 0;
    m_txState            = &m_preFrameZeroState;
    m_txEpoch            = 0;
    m_txFrame            = 0;
    m_rxEpoch            = 0;
    m_rxFrame            = 0;
    m_resyncPhase        = RESYNC_NONE;
    m_resyncEpoch        = 1;
    m_doneEpoch          = 0;
    m_peerReadyEpoch     = 0;
    m_localReadyForResync = false;
    m_resyncOnReturn     = false;
    m_resyncLoaded       = false;
    m_resyncFrame        = 0;
    m_noRollbackBefore   = 0;
    m_segmentStartFrame  = 0;
    m_resyncSendUs       = 0;
    m_countdownEndUs     = 0;
    m_breakStartUs       = 0;
    m_countdownMs        = 0;
    m_gameplayEnteredUs  = 0;
    m_lastFrameRunUs     = 0;
    m_initialStateReady  = false;
    m_initialStateAcked  = false;
    m_initialStateLoaded = false;
    m_initialStateSendUs = 0;
    m_rxStateSize        = 0;
    m_rxChunksNeeded     = 0;
    m_rxChunksHave       = 0;
    m_waitStateSinceUs   = 0;
    memset(m_rxChunkHave, 0, sizeof(m_rxChunkHave));
    ResetLeave();
    m_applyingState = false;
    m_reloadState = nullptr;
    m_dumpTxSize  = 0;
    m_peerSkip = false;
    m_localSkipSends = 0;

    fprintf(stderr, "[netplay] match %u reset: seed=%u, input delay=%d\n",
            matchId, rngSeed, m_inputDelay);
}

void NetplayManager::SeedRngForMatch()
{
    /* Each PC seeded its RNG from its own clock at boot, so the two games
       start every match with different random numbers.  Put both on the
       agreed seed at frame 0 (standard MT19937 initialisation). */
    static unsigned long mt[624];
    mt[0] = static_cast<unsigned long>(m_rngSeed) & 0xFFFFFFFFUL;
    for (int i = 1; i < 624; i++)
    {
        mt[i] = (1812433253UL * (mt[i - 1] ^ (mt[i - 1] >> 30)) + static_cast<unsigned long>(i))
                & 0xFFFFFFFFUL;
    }
    netplay_mt_set_state(mt, 624);
    nlDefaultSeed = m_rngSeed;
    fprintf(stderr, "[netplay] RNG seeded for match %u (seed=%u)\n", m_matchId, m_rngSeed);
}

/* ======================================================================== */
/*  Connect / Disconnect                                                     */
/* ======================================================================== */

void NetplayManager::SendGamePacket(void* pkt, size_t len, u8 type)
{
    if (!m_socket) return;
    NetplayPacketHeader* hdr = static_cast<NetplayPacketHeader*>(pkt);
    hdr->type     = type;
    hdr->matchId  = m_matchId;
    hdr->sequence = m_sendSequence++;
    NetplaySocketSend(static_cast<NetplaySocket*>(m_socket), pkt, len);
}

void NetplayManager::SendHandshake()
{
    NetplayHandshakePacket hs;
    memset(&hs, 0, sizeof(hs));
    hs.magic      = NETPLAY_MAGIC;
    hs.version    = NETPLAY_PROTOCOL_VERSION;
    hs.rngSeed    = m_rngSeed;
    hs.playerSlot = static_cast<u8>(m_localSlot);
    hs.inputDelay = static_cast<u8>(m_inputDelay);
    SendGamePacket(&hs, sizeof(hs), NETPLAY_PKT_HANDSHAKE);
}

void NetplayManager::SendStartMatch(u8 team0, u8 team1, u8 stadium, u8 sidekick0, u8 sidekick1)
{
    if (!m_socket) return;

    u8 newId = static_cast<u8>(m_matchId + 1);
    if (newId == 0) newId = 1;
    u32 seed = static_cast<u32>(NetplayGetTimeUs() & 0xFFFFFFFFu) ^ 0x9E3779B9u;
    if (seed == 0) seed = 12345;

    /* The ini delay is a minimum; raise it to what the measured ping needs. */
    int delay = DelayForPing(m_pingUs);
    if (delay < m_configDelay)
        delay = m_configDelay;
    m_maxRollback = RollbackFromConfig();
    ResetForMatch(newId, seed, delay);
    m_vsLocal.ready = 0;
    SeedRngForMatch();  /* both games load the match from the same random numbers */

    memset(&m_pendingStart, 0, sizeof(m_pendingStart));
    m_pendingStart.team0      = team0;
    m_pendingStart.team1      = team1;
    m_pendingStart.stadium    = stadium;
    m_pendingStart.sidekick0  = sidekick0;
    m_pendingStart.sidekick1  = sidekick1;
    m_pendingStart.inputDelay = static_cast<u8>(m_inputDelay);
    m_pendingStart.maxRollback = static_cast<u8>(m_maxRollback);
    m_pendingStart.rngSeed    = seed;
    m_pendingStart.settingsLen = static_cast<u8>(PortGetMatchSettings(m_pendingStart.settings,
                                                                      sizeof(m_pendingStart.settings)));

    SendGamePacket(&m_pendingStart, sizeof(m_pendingStart), NETPLAY_PKT_START_MATCH);
    m_startPending    = true;
    m_startLastSendUs = NetplayGetTimeUs();
    m_startSends      = 1;

    fprintf(stderr, "[netplay] sent match %u start: team0=%u, team1=%u, stadium=%u, "
                    "input delay %d (ping %u ms), %s\n",
            newId, team0, team1, stadium, m_inputDelay, m_pingUs / 1000,
            m_maxRollback ? "rollback" : "lockstep");
}

bool NetplayManager::Connect(u16 localPort, const char* remoteAddr,
                              int localSlot, int inputDelay,
                              void* existingSocket)
{
    if (m_state != NETPLAY_STATE_IDLE)
        Disconnect();

#if defined(_WIN32) && defined(_M_X64)
    /* The Windows C runtime picks its sin/cos/pow/... code by the processor
       (an FMA3 path on newer CPUs), and the two paths can round the last bit
       differently.  Two PCs then drift apart a little at a time.  One path
       for everyone online. */
    _set_FMA3_enable(0);
#endif

    m_localSlot = localSlot;

    if (existingSocket)
    {
        m_socket = existingSocket;
    }
    else
    {
        m_socket = NetplaySocketCreate(localPort);
        if (!m_socket)
        {
            fprintf(stderr, "[netplay] failed to create socket on port %d\n", localPort);
            return false;
        }

        if (!NetplaySocketSetPeer(static_cast<NetplaySocket*>(m_socket), remoteAddr))
        {
            fprintf(stderr, "[netplay] failed to set peer %s\n", remoteAddr);
            NetplaySocketDestroy(static_cast<NetplaySocket*>(m_socket));
            m_socket = nullptr;
            return false;
        }
    }

    m_sendSequence = 0;
    m_pingUs       = 0;
    m_startPending = false;
    m_maxRollback  = RollbackFromConfig();
    m_configDelay  = inputDelay;
    m_vsRemoteKnown = false;
    m_vsLocal.ready = 0;
    ResetForMatch(0, 12345, inputDelay);

    m_state            = NETPLAY_STATE_CONNECTING;
    m_lastHandshakeUs  = NetplayGetTimeUs();
    m_lastRecvUs       = m_lastHandshakeUs;
    m_lastKeepAliveUs  = m_lastHandshakeUs;
    m_lastQualityUs    = m_lastHandshakeUs;
    m_handshakeRetries = 0;

    SendHandshake();

    fprintf(stderr, "[netplay] connecting as player %d with %d frames input delay to %s\n",
            m_localSlot, m_inputDelay, remoteAddr ? remoteAddr : "(relay)");
    return true;
}

void NetplayManager::Disconnect()
{
    if (m_socket)
    {
        NetplayPacketHeader pkt;
        memset(&pkt, 0, sizeof(pkt));
        SendGamePacket(&pkt, sizeof(pkt), NETPLAY_PKT_DISCONNECT);

        NetplaySocketDestroy(static_cast<NetplaySocket*>(m_socket));
        m_socket = nullptr;
    }
    m_state        = NETPLAY_STATE_IDLE;
    m_startPending = false;
    PADClearAllVirtualStatus();
    PortRestoreOwnSettings();   /* the guest's own options again */
}

void* NetplayManager::DetachSocket(bool tellPeer)
{
    void* sock = m_socket;
    if (sock && tellPeer)
    {
        NetplayPacketHeader pkt;
        memset(&pkt, 0, sizeof(pkt));
        SendGamePacket(&pkt, sizeof(pkt), NETPLAY_PKT_DISCONNECT);
    }
    m_socket       = nullptr;
    m_state        = NETPLAY_STATE_IDLE;
    m_startPending = false;
    PADClearAllVirtualStatus();
    PortRestoreOwnSettings();   /* the guest's own options again */
    return sock;
}

float NetplayManager::GetSecondsSinceLastPacket() const
{
    if (m_state != NETPLAY_STATE_RUNNING)
        return 0.0f;
    u64 now = NetplayGetTimeUs();
    return now > m_lastRecvUs ? static_cast<float>(now - m_lastRecvUs) / 1000000.0f : 0.0f;
}

void NetplayManager::Update()
{
    if (m_state == NETPLAY_STATE_IDLE)
        return;

    ReceivePackets();

    u64 now = NetplayGetTimeUs();

    if (m_state == NETPLAY_STATE_CONNECTING)
    {
        if (now - m_lastHandshakeUs > 350000) /* 350 ms */
        {
            m_lastHandshakeUs = now;
            m_handshakeRetries++;
            if (m_handshakeRetries > 40) /* 14 seconds */
            {
                fprintf(stderr, "[netplay] handshake timed out\n");
                m_state = NETPLAY_STATE_DISCONNECTED;
                return;
            }
            SendHandshake();
        }
    }
    else if (m_state == NETPLAY_STATE_RUNNING)
    {
        if (now - m_lastRecvUs > kPeerTimeoutUs)
        {
            fprintf(stderr, "[netplay] no packets from peer for %llu s, disconnecting\n",
                    static_cast<unsigned long long>(kPeerTimeoutUs / 1000000));
            m_state = NETPLAY_STATE_DISCONNECTED;
            PADClearAllVirtualStatus();
            return;
        }

        if (now - m_lastKeepAliveUs > kKeepAliveIntervalUs)
        {
            m_lastKeepAliveUs = now;
            SendKeepAlive();
        }

        /* Every 10 s, how evenly the game frames ran: each wait is a rendered
           frame with no game frame, which shows as a small stutter. */
        if (m_statSinceUs == 0)
        {
            m_statSinceUs = now;
            m_statFrameAtStart = m_simFrame;
        }
        else if (now - m_statSinceUs > 10000000)
        {
            if (m_simFrame != m_statFrameAtStart || m_statWaitInput || m_statWaitSync)
                fprintf(stderr, "[netplay] last 10 s: %u game frames, waited for input %d times, "
                                "slowed down to stay in step %d times (ping %u ms, delay %d)\n",
                        m_simFrame - m_statFrameAtStart, m_statWaitInput, m_statWaitSync,
                        m_pingUs / 1000, m_inputDelay);
            m_statSinceUs = now;
            m_statFrameAtStart = m_simFrame;
            m_statWaitInput = 0;
            m_statWaitSync = 0;
        }

        if (now - m_lastQualityUs > kQualityIntervalUs)
        {
            m_lastQualityUs = now;
            SendQualityReport();
        }

        if (now - m_vsLastSendUs > 150000)
            SendVsChoice();

        PumpDumpToHost();

        /* A skip is sent a few times over half a second. */
        if (m_localSkipSends > 0 && now - m_localSkipSendUs > 100000)
        {
            NetplayResyncPacket sp;
            memset(&sp, 0, sizeof(sp));
            sp.epoch = m_localSkipEpoch;
            SendGamePacket(&sp, sizeof(sp), NETPLAY_PKT_SKIP);
            m_localSkipSendUs = now;
            m_localSkipSends--;
        }

        if (m_initialStateReady && !m_initialStateAcked && now - m_initialStateSendUs > 100000)
            SendInitialState();

        /* Keep resync messages flowing even while this game is in a replay or
           menu (PreUpdate is not called there). */
        if (m_resyncPhase != RESYNC_NONE && m_localReadyForResync && now - m_resyncSendUs > kResyncResendUs)
        {
            if (m_localSlot == 0 && m_resyncPhase == RESYNC_COUNTDOWN)
            {
                u32 left = (m_countdownEndUs > now) ? static_cast<u32>((m_countdownEndUs - now) / 1000) : 0;
                SendResync(NETPLAY_PKT_RESYNC_GO, left);
            }
            else if (m_resyncPhase == RESYNC_WAITING || m_resyncPhase == RESYNC_TRANSFER)
            {
                SendResync(NETPLAY_PKT_RESYNC_READY, m_simFrame);
            }
        }

        /* Coordinated leave: keep talking until both sides know both answers,
           and keep our last inputs flowing even after this game stopped
           calling PreUpdate (it is in a replay or menu by then). */
        if (m_leaveDecided && now - m_leaveSendUs > kLeaveResendUs)
        {
            if (!m_leavePeerKnown || m_leavePeerSeen != m_leaveMyStop)
                SendLeave();
            else
                m_leaveSendUs = now;
            if (m_peerAckedFrame < m_localInputLatest)
                SendInputs(true);
        }

        if (m_startPending && now - m_startLastSendUs > kStartResendUs)
        {
            if (m_startSends >= kStartMaxSends)
            {
                fprintf(stderr, "[netplay] guest never acknowledged match %u start\n", m_matchId);
                m_startPending = false;
            }
            else
            {
                m_startLastSendUs = now;
                m_startSends++;
                SendGamePacket(&m_pendingStart, sizeof(m_pendingStart), NETPLAY_PKT_START_MATCH);
            }
        }
    }
}

/* ======================================================================== */
/*  Receive packets                                                          */
/* ======================================================================== */

void NetplayManager::HandleRemoteInput(s32 frame, const NetplayInput& input)
{
    if (frame < 0 || frame <= m_remoteConfirmed)
        return;  /* already have it */

    /* The peer can only be a few frames ahead of us; anything further is
       garbage and would overwrite live slots. */
    if (frame > m_lastSimFrame + NETPLAY_INPUT_RING_SIZE / 2)
        return;

    InputSlot& slot = m_remoteInputs[Ring(frame)];
    if (slot.frame == frame)
        return;

    slot.frame = frame;
    slot.input = input;

    if (frame > m_remoteLatest)
        m_remoteLatest = frame;

    /* Already simulated this frame with a guess?  If the guess was wrong,
       remember the earliest such frame for the next rollback. */
    if (frame <= m_lastSimFrame)
    {
        const InputSlot& used = m_usedRemoteInputs[Ring(frame)];
        if (used.frame == frame && !NetplayInputEqual(&used.input, &input))
        {
            if (m_firstMispredict < 0 || frame < m_firstMispredict)
                m_firstMispredict = frame;
        }
    }

    while (m_remoteInputs[Ring(m_remoteConfirmed + 1)].frame == m_remoteConfirmed + 1)
        m_remoteConfirmed++;
}

void NetplayManager::ReceivePackets()
{
    if (!m_socket) return;

    u8 buf[1024];
    char fromAddr[64];

    for (;;)
    {
        int n = NetplaySocketRecv(static_cast<NetplaySocket*>(m_socket),
                                  buf, sizeof(buf), fromAddr, sizeof(fromAddr));
        if (n <= 0) break;

        /* Relay control messages are JSON; they are not for us. */
        if (buf[0] == '{')
        {
            /* Relay control messages.  "peer_left": the opponent closed the
               game or left the room. */
            if (n < static_cast<int>(sizeof(buf)) && memchr(buf, '\0', n) == nullptr)
            {
                buf[n < static_cast<int>(sizeof(buf)) - 1 ? n : static_cast<int>(sizeof(buf)) - 1] = '\0';
                if (strstr(reinterpret_cast<char*>(buf), "\"cmd\":\"peer_left\"") &&
                    m_state != NETPLAY_STATE_IDLE)
                {
                    fprintf(stderr, "[netplay] relay says the opponent left\n");
                    m_state = NETPLAY_STATE_DISCONNECTED;
                    PADClearAllVirtualStatus();
                    return;
                }
            }
            continue;
        }

        if (n < static_cast<int>(sizeof(NetplayPacketHeader)))
            continue;

        NetplayPacketHeader* hdr = reinterpret_cast<NetplayPacketHeader*>(buf);
        m_lastRecvUs = NetplayGetTimeUs();

        switch (hdr->type)
        {
        case NETPLAY_PKT_HANDSHAKE:
        case NETPLAY_PKT_HANDSHAKE_ACK:
        {
            if (n < static_cast<int>(sizeof(NetplayHandshakePacket)))
                break;
            NetplayHandshakePacket* hs = reinterpret_cast<NetplayHandshakePacket*>(buf);
            if (hs->magic != NETPLAY_MAGIC || hs->version != NETPLAY_PROTOCOL_VERSION)
            {
                fprintf(stderr, "[netplay] handshake rejected: peer runs protocol %u, we run %u "
                                "(both players need the same build)\n",
                        hs->version, NETPLAY_PROTOCOL_VERSION);
                break;
            }
            if (hs->playerSlot == m_localSlot)
            {
                fprintf(stderr, "[netplay] WARNING: both players are slot %d; one must be Player 2\n",
                        m_localSlot);
            }

            if (hdr->type == NETPLAY_PKT_HANDSHAKE)
            {
                NetplayHandshakePacket ack;
                memset(&ack, 0, sizeof(ack));
                ack.magic      = NETPLAY_MAGIC;
                ack.version    = NETPLAY_PROTOCOL_VERSION;
                ack.rngSeed    = m_rngSeed;
                ack.playerSlot = static_cast<u8>(m_localSlot);
                ack.inputDelay = static_cast<u8>(m_inputDelay);
                SendGamePacket(&ack, sizeof(ack), NETPLAY_PKT_HANDSHAKE_ACK);
            }

            if (m_state == NETPLAY_STATE_CONNECTING)
            {
                m_state = NETPLAY_STATE_RUNNING;
                PlugInPort1();
                fprintf(stderr, "[netplay] session started with player %d\n", hs->playerSlot + 1);
            }
            break;
        }

        case NETPLAY_PKT_INPUT:
        {
            if (n < static_cast<int>(NETPLAY_INPUT_PACKET_HEADER_SIZE))
                break;
            if (hdr->matchId != m_matchId)
                break;  /* left over from another match */
            NetplayInputPacket* pkt = reinterpret_cast<NetplayInputPacket*>(buf);
            if (pkt->segment != m_doneEpoch || m_resyncPhase != RESYNC_NONE)
                break;  /* from before the last resync, or one is in progress */

            int count = pkt->inputCount;
            if (count > NETPLAY_MAX_INPUTS_PER_PACKET)
                count = NETPLAY_MAX_INPUTS_PER_PACKET;
            if (n < static_cast<int>(NETPLAY_INPUT_PACKET_HEADER_SIZE + count * sizeof(NetplayInput)))
                break;

            if (pkt->ackFrame > 0)
            {
                s32 acked = static_cast<s32>(pkt->ackFrame) - 1;
                if (acked > m_peerAckedFrame && acked <= m_localInputLatest)
                    m_peerAckedFrame = acked;
            }

            for (int j = 0; j < count; j++)
                HandleRemoteInput(static_cast<s32>(pkt->startFrame) + j, pkt->inputs[j]);
            break;
        }

        case NETPLAY_PKT_QUALITY_REPORT:
        {
            if (n < static_cast<int>(sizeof(NetplayQualityReportPacket)))
                break;
            NetplayQualityReportPacket* qr = reinterpret_cast<NetplayQualityReportPacket*>(buf);
            if (hdr->matchId == m_matchId)
                m_remoteFrameAdvantage = qr->frameAdvantage;

            NetplayQualityReplyPacket reply;
            memset(&reply, 0, sizeof(reply));
            reply.echoTimeUs = qr->sendTimeUs;
            SendGamePacket(&reply, sizeof(reply), NETPLAY_PKT_QUALITY_REPLY);
            break;
        }

        case NETPLAY_PKT_QUALITY_REPLY:
        {
            if (n < static_cast<int>(sizeof(NetplayQualityReplyPacket)))
                break;
            NetplayQualityReplyPacket* rp = reinterpret_cast<NetplayQualityReplyPacket*>(buf);
            u32 rtt = static_cast<u32>(NetplayGetTimeUs() & 0xFFFFFFFFu) - rp->echoTimeUs;
            if (rtt < 5000000u)
                m_pingUs = (m_pingUs == 0) ? rtt : (m_pingUs * 7 + rtt) / 8;
            break;
        }

        case NETPLAY_PKT_KEEPALIVE:
        case NETPLAY_PKT_INPUT_ACK:
            break;

        case NETPLAY_PKT_DISCONNECT:
            fprintf(stderr, "[netplay] peer disconnected\n");
            m_state = NETPLAY_STATE_DISCONNECTED;
            PADClearAllVirtualStatus();
            return;

        case NETPLAY_PKT_START_MATCH:
        {
            if (n < static_cast<int>(sizeof(NetplayStartMatchPacket)))
                break;
            NetplayStartMatchPacket* p = reinterpret_cast<NetplayStartMatchPacket*>(buf);
            u8 id = hdr->matchId;

            /* Always acknowledge, so the host stops resending. */
            NetplayStartMatchAckPacket ack;
            memset(&ack, 0, sizeof(ack));
            u8 savedId = m_matchId;
            m_matchId = id;
            SendGamePacket(&ack, sizeof(ack), NETPLAY_PKT_START_MATCH_ACK);
            m_matchId = savedId;

            if (id == m_matchId)
                break;  /* duplicate of a start we already ran */

            fprintf(stderr, "[netplay] host started match %u: team0=%u, team1=%u, stadium=%u\n",
                    id, p->team0, p->team1, p->stadium);

            m_maxRollback = p->maxRollback > NETPLAY_MAX_ROLLBACK_FRAMES ? NETPLAY_MAX_ROLLBACK_FRAMES
                                                                         : p->maxRollback;
            ResetForMatch(id, p->rngSeed, p->inputDelay);
            m_vsLocal.ready = 0;
            /* Play by the host's rules (skill level, items...), not this PC's
               memory card settings. */
            PortApplyMatchSettings(p->settings, p->settingsLen);
            SeedRngForMatch();  /* both games load the match from the same random numbers */

            PortDebugCommand k;
            memset(&k, 0, sizeof(k));
            k.op   = PDBG_START_MATCH;
            k.a    = p->team0;
            k.b    = p->team1;
            k.c    = p->stadium;
            k.f[0] = static_cast<float>(p->sidekick0);
            k.f[1] = static_cast<float>(p->sidekick1);
            k.f[2] = 99.0f; // Dual-player Netplay match flag
            k.f[3] = static_cast<float>(id);  // lets the VS screen see this request's result
            PortDebugPushCommand(&k);
            break;
        }

        case NETPLAY_PKT_START_MATCH_ACK:
            if (m_startPending && hdr->matchId == m_matchId)
            {
                m_startPending = false;
                fprintf(stderr, "[netplay] guest acknowledged match %u start\n", m_matchId);
            }
            break;

        case NETPLAY_PKT_VS_CHOICE:
        {
            if (n < static_cast<int>(sizeof(NetplayVsChoicePacket)))
                break;
            NetplayVsChoicePacket* vp = reinterpret_cast<NetplayVsChoicePacket*>(buf);
            m_vsRemote      = vp->choice;
            m_vsRemoteKnown = true;
            m_vsRemoteUs    = NetplayGetTimeUs();
            break;
        }

        case NETPLAY_PKT_DUMP_CHUNK:
        {
            /* The guest's copy of a desynced frame, so the host has both. */
            if (n < static_cast<int>(NETPLAY_STATE_CHUNK_HEADER_SIZE) || m_localSlot != 0)
                break;
            const NetplayStateChunkPacket* dp = reinterpret_cast<const NetplayStateChunkPacket*>(buf);
            static u8  s_dump[NETPLAY_STATE_MAX_BYTES];
            static u8  s_have[(NETPLAY_STATE_MAX_BYTES / NETPLAY_STATE_CHUNK_BYTES) + 1];
            static u32 s_frame = 0xFFFFFFFFu, s_total = 0, s_need = 0, s_got = 0;
            if (dp->totalSize == 0 || dp->totalSize > NETPLAY_STATE_MAX_BYTES ||
                dp->offset + dp->length > dp->totalSize || dp->length > NETPLAY_STATE_CHUNK_BYTES ||
                n < static_cast<int>(NETPLAY_STATE_CHUNK_HEADER_SIZE + dp->length))
                break;
            static u8 s_kind = 0xFF;
            if (dp->frame != s_frame || dp->totalSize != s_total || dp->epoch != s_kind)
            {
                s_kind  = dp->epoch;
                s_frame = dp->frame;
                s_total = dp->totalSize;
                s_need  = (s_total + NETPLAY_STATE_CHUNK_BYTES - 1) / NETPLAY_STATE_CHUNK_BYTES;
                s_got   = 0;
                memset(s_have, 0, sizeof(s_have));
            }
            u32 idx = dp->offset / NETPLAY_STATE_CHUNK_BYTES;
            if (idx >= s_need || s_have[idx] || s_got >= s_need)
                break;
            memcpy(s_dump + dp->offset, dp->data, dp->length);
            s_have[idx] = 1;
            if (++s_got == s_need)
            {
                char path[96];
                if (s_kind == 1)
                    snprintf(path, sizeof(path), "syncdump-p2-e%u.bin", s_frame);
                else
                    snprintf(path, sizeof(path), "desync-p2-frame%u.bin", s_frame);
                FILE* f = fopen(path, "wb");
                if (f)
                {
                    fwrite(s_dump, 1, s_total, f);
                    fclose(f);
                }
                fprintf(stderr, "[netplay] got the opponent's copy of frame %u: wrote %s\n", s_frame, path);
            }
            break;
        }

        case NETPLAY_PKT_STATE_CHUNK:
        {
            if (n < static_cast<int>(NETPLAY_STATE_CHUNK_HEADER_SIZE))
                break;
            if (hdr->matchId != m_matchId)
                break;
            HandleStateChunk(reinterpret_cast<NetplayStateChunkPacket*>(buf), n);
            break;
        }

        case NETPLAY_PKT_STATE_ACK:
        {
            if (n < static_cast<int>(sizeof(NetplayStateAckPacket)))
                break;
            const NetplayStateAckPacket* ap = reinterpret_cast<const NetplayStateAckPacket*>(buf);
            if (hdr->matchId == m_matchId && m_initialStateReady && !m_initialStateAcked &&
                ap->epoch == m_txEpoch)
            {
                m_initialStateAcked = true;
                fprintf(stderr, "[netplay] guest has the %s state (match %u, epoch %u)\n",
                        m_txEpoch == 0 ? "starting" : "resync", m_matchId, m_txEpoch);
            }
            break;
        }

        case NETPLAY_PKT_RESYNC_READY:
        case NETPLAY_PKT_RESYNC_GO:
        {
            if (n < static_cast<int>(sizeof(NetplayResyncPacket)) || hdr->matchId != m_matchId)
                break;
            const NetplayResyncPacket* rp = reinterpret_cast<const NetplayResyncPacket*>(buf);
            if (hdr->type == NETPLAY_PKT_RESYNC_READY)
            {
                if (rp->epoch == static_cast<u8>(m_doneEpoch + 1))
                {
                    if (m_leaveDecided && !m_leaveDone)
                        break;   /* still playing the frames up to the agreed stop; it resends */
                    if (m_resyncPhase == RESYNC_NONE)
                    {
                        fprintf(stderr, "[netplay] opponent is ready to resume (epoch %u)\n", rp->epoch);
                        BeginResync();   /* join it; we become ready once back in gameplay */
                    }
                    m_peerReadyEpoch = rp->epoch;
                }
                else if (rp->epoch == m_doneEpoch && m_localSlot == 0)
                {
                    /* The guest missed our GO and still waits: tell it to go now. */
                    SendResync(NETPLAY_PKT_RESYNC_GO, 0);
                }
            }
            else if (m_localSlot != 0 && rp->epoch == m_resyncEpoch && m_resyncPhase != RESYNC_NONE)
            {
                u64 now = NetplayGetTimeUs();
                u64 halfRtt = m_pingUs / 2;
                u64 leftUs = static_cast<u64>(rp->value) * 1000;
                u64 end = now + (leftUs > halfRtt ? leftUs - halfRtt : 0);
                if (m_resyncPhase != RESYNC_COUNTDOWN || end < m_countdownEndUs)
                    m_countdownEndUs = end;
                if (m_resyncPhase != RESYNC_COUNTDOWN)
                    fprintf(stderr, "[netplay] countdown %u ms (epoch %u)\n", rp->value, rp->epoch);
                m_resyncPhase = RESYNC_COUNTDOWN;
            }
            break;
        }

        case NETPLAY_PKT_SKIP:
        {
            if (n < static_cast<int>(sizeof(NetplayResyncPacket)) || hdr->matchId != m_matchId)
                break;
            const NetplayResyncPacket* sp = reinterpret_cast<const NetplayResyncPacket*>(buf);
            if (sp->epoch == m_doneEpoch && !(m_peerSkip && m_peerSkipEpoch == sp->epoch))
            {
                m_peerSkip = true;
                m_peerSkipEpoch = sp->epoch;
                fprintf(stderr, "[netplay] opponent skipped the replay/cutscene: skipping it here too\n");
            }
            break;
        }

        case NETPLAY_PKT_LEAVE:
        {
            if (n < static_cast<int>(sizeof(NetplayLeavePacket)) || hdr->matchId != m_matchId)
                break;
            HandleLeave(reinterpret_cast<const NetplayLeavePacket*>(buf));
            break;
        }

        case NETPLAY_PKT_MATCH_ABORT:
        {
            if (n < static_cast<int>(sizeof(NetplayMatchAbortPacket)))
                break;
            /* Match 0 is "no match yet": nothing was started, so nothing to abort. */
            if (hdr->matchId != m_matchId || m_remoteAbort || m_matchId == 0)
                break;
            NetplayMatchAbortPacket* ap = reinterpret_cast<NetplayMatchAbortPacket*>(buf);
            m_remoteAbort       = true;
            m_remoteAbortReason = ap->reason;
            m_startPending      = false;
            fprintf(stderr, "[netplay] opponent could not start match %u (reason %d)\n",
                    m_matchId, m_remoteAbortReason);
            break;
        }

        case NETPLAY_PKT_CHECKSUM:
        {
            if (n < static_cast<int>(sizeof(NetplayChecksumPacket)))
                break;
            if (hdr->matchId != m_matchId)
                break;
            NetplayChecksumPacket* cp = reinterpret_cast<NetplayChecksumPacket*>(buf);
            s32 frame = static_cast<s32>(cp->frame);
            if (frame < 0 || frame % kChecksumInterval != 0)
                break;
            ChecksumEntry& e = m_remoteSums[(frame / kChecksumInterval) % 16];
            e.frame    = frame;
            e.checksum = cp->checksum;
            CheckDesync();
            break;
        }

        default:
            break;
        }
    }
}

/* ======================================================================== */
/*  Send helpers                                                             */
/* ======================================================================== */

void NetplayManager::SendInputs(bool evenDuringResync)
{
    if (!m_socket || (m_resyncPhase != RESYNC_NONE && !evenDuringResync)) return;

    NetplayInputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));

    /* Everything the peer has not acknowledged yet, oldest first.  This is
       what makes the input stream survive any amount of packet loss. */
    s32 from  = m_peerAckedFrame + 1;
    s32 to    = m_localInputLatest;
    int count = (to >= from) ? static_cast<int>(to - from + 1) : 0;
    if (count > NETPLAY_MAX_INPUTS_PER_PACKET)
        count = NETPLAY_MAX_INPUTS_PER_PACKET;

    pkt.startFrame = static_cast<u32>(from < 0 ? 0 : from);
    pkt.ackFrame   = static_cast<u32>(m_remoteConfirmed + 1);
    pkt.inputCount = static_cast<u8>(count);
    pkt.segment    = m_doneEpoch;

    for (int i = 0; i < count; i++)
    {
        s32 f = from + i;
        const InputSlot& slot = m_localInputs[Ring(f)];
        pkt.inputs[i] = (slot.frame == f) ? slot.input : NeutralInput();
    }

    SendGamePacket(&pkt, NETPLAY_INPUT_PACKET_HEADER_SIZE + count * sizeof(NetplayInput),
                   NETPLAY_PKT_INPUT);
}

void NetplayManager::SendVsChoice()
{
    m_vsLastSendUs = NetplayGetTimeUs();
    NetplayVsChoicePacket vp;
    memset(&vp, 0, sizeof(vp));
    vp.choice = m_vsLocal;
    SendGamePacket(&vp, sizeof(vp), NETPLAY_PKT_VS_CHOICE);
}

void NetplayManager::SetVsChoice(const NetplayVsChoice& choice)
{
    bool changed = memcmp(&choice, &m_vsLocal, sizeof(choice)) != 0;
    m_vsLocal = choice;
    if (changed && m_state == NETPLAY_STATE_RUNNING)
        SendVsChoice();  /* the opponent sees it right away */
}

void NetplayManager::SendInitialState()
{
    m_initialStateSendUs = NetplayGetTimeUs();
    if (m_txState == nullptr)
        return;
    const SavedState& ss = *m_txState;
    if (ss.data == nullptr || ss.size == 0)
        return;

    /* Every piece, every 100 ms, until the guest says it has them all.
       ~10-20 pieces for this game's state, so this costs nothing. */
    NetplayStateChunkPacket pkt;
    for (u32 off = 0; off < ss.size; off += NETPLAY_STATE_CHUNK_BYTES)
    {
        u32 len = ss.size - off;
        if (len > NETPLAY_STATE_CHUNK_BYTES)
            len = NETPLAY_STATE_CHUNK_BYTES;
        memset(&pkt, 0, NETPLAY_STATE_CHUNK_HEADER_SIZE);
        pkt.totalSize = ss.size;
        pkt.offset    = off;
        pkt.length    = static_cast<u16>(len);
        pkt.epoch     = m_txEpoch;
        pkt.frame     = static_cast<u32>(m_txFrame);
        memcpy(pkt.data, ss.data + off, len);
        SendGamePacket(&pkt, NETPLAY_STATE_CHUNK_HEADER_SIZE + len, NETPLAY_PKT_STATE_CHUNK);
    }
}

void NetplayManager::HandleStateChunk(const NetplayStateChunkPacket* pkt, int n)
{
    if (m_localSlot == 0)
        return;  /* only the guest takes the host's state */

    /* Which transfer is this?  Epoch 0 is the match start; anything else is
       the resync in progress. */
    u8 epoch = pkt->epoch;
    bool wanted = (epoch == 0) ? true
                               : (m_resyncPhase != RESYNC_NONE && epoch == m_resyncEpoch);
    if (!wanted)
        return;

    bool sameTransfer = (epoch == m_rxEpoch && m_rxChunksNeeded != 0);
    if (sameTransfer && m_rxChunksHave == m_rxChunksNeeded)
    {
        /* Already complete: the ack was probably lost, say it again. */
        NetplayStateAckPacket ack;
        memset(&ack, 0, sizeof(ack));
        ack.epoch = epoch;
        SendGamePacket(&ack, sizeof(ack), NETPLAY_PKT_STATE_ACK);
        return;
    }
    if (epoch == 0 && m_initialStateLoaded)
        return;

    u32 total = pkt->totalSize;
    u32 off   = pkt->offset;
    u32 len   = pkt->length;
    if (total == 0 || total > NETPLAY_STATE_MAX_BYTES || len > NETPLAY_STATE_CHUNK_BYTES ||
        off % NETPLAY_STATE_CHUNK_BYTES != 0 || off + len > total ||
        static_cast<u32>(n) < NETPLAY_STATE_CHUNK_HEADER_SIZE + len)
        return;

    if (!sameTransfer || m_rxStateSize != total)
    {
        /* First piece (or the size changed): start over. */
        if (m_rxStateCapacity < total)
        {
            m_rxState = static_cast<u8*>(realloc(m_rxState, total));
            m_rxStateCapacity = total;
        }
        m_rxStateSize    = total;
        m_rxChunksNeeded = (total + NETPLAY_STATE_CHUNK_BYTES - 1) / NETPLAY_STATE_CHUNK_BYTES;
        m_rxChunksHave   = 0;
        m_rxEpoch        = epoch;
        m_rxFrame        = static_cast<s32>(pkt->frame);
        memset(m_rxChunkHave, 0, sizeof(m_rxChunkHave));
    }

    u32 index = off / NETPLAY_STATE_CHUNK_BYTES;
    if (m_rxChunkHave[index])
        return;
    memcpy(m_rxState + off, pkt->data, len);
    m_rxChunkHave[index] = 1;
    m_rxChunksHave++;

    if (m_rxChunksHave == m_rxChunksNeeded)
    {
        fprintf(stderr, "[netplay] received the host's %s state (%u bytes)\n",
                epoch == 0 ? "starting" : "resync", total);
        NetplayStateAckPacket ack;
        memset(&ack, 0, sizeof(ack));
        ack.epoch = epoch;
        SendGamePacket(&ack, sizeof(ack), NETPLAY_PKT_STATE_ACK);
    }
}

void NetplayManager::SendMatchAbort(int reason)
{
    m_startPending = false;
    NetplayMatchAbortPacket ap;
    memset(&ap, 0, sizeof(ap));
    ap.reason = static_cast<s8>(reason);
    for (int i = 0; i < 3; i++)  /* tiny and rare: just send it a few times */
        SendGamePacket(&ap, sizeof(ap), NETPLAY_PKT_MATCH_ABORT);
    fprintf(stderr, "[netplay] this game could not start match %u (reason %d)\n", m_matchId, reason);
}

bool NetplayManager::TakeRemoteAbort(int* reason)
{
    if (!m_remoteAbort)
        return false;
    m_remoteAbort = false;
    if (reason) *reason = m_remoteAbortReason;
    return true;
}

bool NetplayManager::GetRemoteVsChoice(NetplayVsChoice* out) const
{
    if (!m_vsRemoteKnown || m_state != NETPLAY_STATE_RUNNING)
        return false;
    if (NetplayGetTimeUs() - m_vsRemoteUs > 3000000)
        return false;
    if (out) *out = m_vsRemote;
    return true;
}

void NetplayManager::SendKeepAlive()
{
    NetplayPacketHeader pkt;
    memset(&pkt, 0, sizeof(pkt));
    SendGamePacket(&pkt, sizeof(pkt), NETPLAY_PKT_KEEPALIVE);
}

void NetplayManager::SendQualityReport()
{
    /* Where is the peer right now?  Its newest input is for (its frame +
       delay), and that input is half a round trip old. */
    if (m_lastSimFrame >= 0 && m_remoteLatest >= 0)
    {
        /* In slow motion (Super Strikes) game frames come slower than every 20 ms. */
        float scale = FixedUpdateTask::mTimeScale;
        scale = scale < 0.01f ? 0.01f : (scale > 1.0f ? 1.0f : scale);
        float halfRttFrames = (m_pingUs / 1000.0f) / (kFrameMs / scale) * 0.5f;
        float remoteNow = static_cast<float>(m_remoteLatest - m_inputDelay) + halfRttFrames;
        float adv = remoteNow - static_cast<float>(m_lastSimFrame);
        m_localFrameAdvantage = static_cast<int>(adv >= 0.0f ? adv + 0.5f : adv - 0.5f);
    }

    NetplayQualityReportPacket qr;
    memset(&qr, 0, sizeof(qr));
    qr.frameAdvantage = m_localFrameAdvantage;
    qr.sendTimeUs     = static_cast<u32>(NetplayGetTimeUs() & 0xFFFFFFFFu);
    SendGamePacket(&qr, sizeof(qr), NETPLAY_PKT_QUALITY_REPORT);
}

void NetplayManager::SendChecksums()
{
    /* Only frames every remote input has arrived for are final. */
    s32 limit = m_remoteConfirmed < m_lastSimFrame ? m_remoteConfirmed : m_lastSimFrame;
    if (m_firstMispredict >= 0 && m_firstMispredict - 1 < limit)
        limit = m_firstMispredict - 1;

    for (s32 f = m_lastChecksumSent + kChecksumInterval; f <= limit; f += kChecksumInterval)
    {
        const ChecksumEntry& e = m_localSums[(f / kChecksumInterval) % 16];
        if (e.frame == f)
        {
            NetplayChecksumPacket cp;
            memset(&cp, 0, sizeof(cp));
            cp.frame    = static_cast<u32>(f);
            cp.checksum = e.checksum;
            SendGamePacket(&cp, sizeof(cp), NETPLAY_PKT_CHECKSUM);
        }
        m_lastChecksumSent = f;
    }
}

void NetplayManager::KeepChecksumState(s32 frame, const SavedState& ss)
{
    SavedState& dst = m_sumStates[(frame / kChecksumInterval) % 4];
    if (dst.data == nullptr || dst.capacity < ss.size)
    {
        dst.data     = static_cast<u8*>(realloc(dst.data, ss.size));
        dst.capacity = ss.size;
    }
    memcpy(dst.data, ss.data, ss.size);
    dst.size     = ss.size;
    dst.frame    = frame;
    dst.checksum = ss.checksum;
}

void NetplayManager::DumpDesync(s32 frame, u32 remoteChecksum)
{
    /* Both players' dumps of the same frame, compared byte by byte, show the
       exact field the two games disagree on. */
    const SavedState& ss = m_sumStates[(frame / kChecksumInterval) % 4];
    if (ss.frame != frame || ss.data == nullptr)
    {
        fprintf(stderr, "[netplay] (no saved state left for frame %d to dump)\n", frame);
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), "desync-p%d-frame%d.bin", m_localSlot + 1, frame);
    FILE* f = fopen(path, "wb");
    if (f)
    {
        fwrite(ss.data, 1, ss.size, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "desync-p%d-frame%d-fields.txt", m_localSlot + 1, frame);
    f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "# match %u frame %d size %u local %08X remote %08X\n",
                m_matchId, frame, ss.size, ss.checksum, remoteChecksum);
        NetplayWriteStateFieldMap(f);
        fclose(f);
    }
    fprintf(stderr, "[netplay] wrote desync-p%d-frame%d.bin (%u bytes) and its field list\n",
            m_localSlot + 1, frame, ss.size);

    /* Guest: send the host a copy too, so both dumps end up on one PC. */
    QueueDumpForHost(0, static_cast<u32>(frame), ss.data, ss.size);
}

void NetplayManager::QueueDumpForHost(u8 kind, u32 id, const u8* data, u32 size)
{
    /* Sent a few chunks per rendered frame, several times over: a burst of
       large packets through the internet tunnel lost some of them. */
    if (m_localSlot == 0 || data == nullptr || size == 0 || size > NETPLAY_STATE_MAX_BYTES)
        return;
    if (m_dumpTx == nullptr)
        m_dumpTx = static_cast<u8*>(malloc(NETPLAY_STATE_MAX_BYTES));
    if (m_dumpTx == nullptr)
        return;
    memcpy(m_dumpTx, data, size);
    m_dumpTxSize   = size;
    m_dumpTxKind   = kind;
    m_dumpTxId     = id;
    m_dumpTxCursor = 0;
    m_dumpTxPasses = 0;
}

void NetplayManager::PumpDumpToHost()
{
    if (m_dumpTxSize == 0 || !m_socket)
        return;
    NetplayStateChunkPacket pkt;
    for (int k = 0; k < 3; k++)
    {
        u32 off = m_dumpTxCursor;
        u32 len = m_dumpTxSize - off;
        if (len > NETPLAY_STATE_CHUNK_BYTES)
            len = NETPLAY_STATE_CHUNK_BYTES;
        memset(&pkt, 0, NETPLAY_STATE_CHUNK_HEADER_SIZE);
        pkt.totalSize = m_dumpTxSize;
        pkt.offset    = off;
        pkt.length    = static_cast<u16>(len);
        pkt.epoch     = m_dumpTxKind;
        pkt.frame     = m_dumpTxId;
        memcpy(pkt.data, m_dumpTx + off, len);
        SendGamePacket(&pkt, NETPLAY_STATE_CHUNK_HEADER_SIZE + len, NETPLAY_PKT_DUMP_CHUNK);
        m_dumpTxCursor += len;
        if (m_dumpTxCursor >= m_dumpTxSize)
        {
            m_dumpTxCursor = 0;
            if (++m_dumpTxPasses >= 4)
            {
                m_dumpTxSize = 0;
                return;
            }
        }
    }
}

void NetplayManager::CheckDesync()
{
    s32 limit = m_remoteConfirmed < m_lastSimFrame ? m_remoteConfirmed : m_lastSimFrame;
    if (m_firstMispredict >= 0 && m_firstMispredict - 1 < limit)
        limit = m_firstMispredict - 1;

    for (int i = 0; i < 16; i++)
    {
        ChecksumEntry& r = m_remoteSums[i];
        if (r.frame >= 0 && r.frame < m_segmentStartFrame)
        {
            r.frame = -1;   /* from before the last resync */
            continue;
        }
        if (r.frame < 0 || r.frame > limit)
            continue;
        const ChecksumEntry& l = m_localSums[i];
        if (l.frame == r.frame && l.checksum != r.checksum && m_desyncFrame < 0)
        {
            m_desyncFrame = r.frame;
            fprintf(stderr, "[netplay] DESYNC at frame %d: local %08X, remote %08X\n",
                    r.frame, l.checksum, r.checksum);
            DumpDesync(r.frame, r.checksum);
            RequestRecoveryStop(r.frame);
        }
        r.frame = -1;
    }
}

/* ======================================================================== */
/*  Input prediction / injection                                             */
/* ======================================================================== */

NetplayInput NetplayManager::PredictInput() const
{
    /* Repeat the newest remote input we have: right for held buttons and
       sticks, which is most frames of an action game. */
    if (m_remoteLatest >= 0)
    {
        const InputSlot& slot = m_remoteInputs[Ring(m_remoteLatest)];
        if (slot.frame == m_remoteLatest)
            return slot.input;
    }
    return NeutralInput();
}

void NetplayManager::InjectInputs(s32 frame)
{
    const InputSlot& localSlot = m_localInputs[Ring(frame)];
    NetplayInput local = (localSlot.frame == frame) ? localSlot.input : NeutralInput();

    const InputSlot& remoteSlot = m_remoteInputs[Ring(frame)];
    NetplayInput remote = (remoteSlot.frame == frame) ? remoteSlot.input : PredictInput();

    InputSlot& used = m_usedRemoteInputs[Ring(frame)];
    used.frame = frame;
    used.input = remote;

    PADStatus localPad, remotePad;
    memset(&localPad, 0, sizeof(localPad));
    memset(&remotePad, 0, sizeof(remotePad));
    NetplayInputToPAD(&localPad, &local);
    NetplayInputToPAD(&remotePad, &remote);

    /* The game reads the controllers once per rendered frame, before any task
       runs, into PadStatus::s_Current, and the fixed step reads that buffer.
       So the inputs go straight into it: a virtual pad status only reaches it
       at the next rendered frame, which left port 0 as this PC's own
       controller and port 1 a frame late, and made rollback resimulate with
       stale inputs. */
    PadStatus::s_Current[m_localSlot]     = localPad;
    PadStatus::s_Current[1 - m_localSlot] = remotePad;

    /* Each stick turns by its owner's camera angle.  Neutral filler inputs
       (remap 0) keep the angle the port had, which is in the save state. */
    if (local.remap != 0)
        PortSetPadRemap(m_localSlot, local.remap);
    if (remote.remap != 0)
        PortSetPadRemap(1 - m_localSlot, remote.remap);
    PortSetPadRemapActive(1);

    /* The virtual statuses keep the next frame's reads consistent (and port 1
       connected). */
    PADSetVirtualStatus(static_cast<u32>(m_localSlot), &localPad);
    PADSetVirtualStatus(static_cast<u32>(1 - m_localSlot), &remotePad);
}

/* TEST-ONLINE.bat: a fake player.  Runs in a random direction for a while,
   tapping or holding pass, shoot, lob, deke, turbo and item buttons. */
extern "C" int PortAutotestActive(void);

static NetplayInput AutotestInput(int slot)
{
    static u32 s_rng = 0;
    static u64 s_nextChangeUs = 0, s_releaseUs = 0;
    static NetplayInput s_cur;
    if (s_rng == 0)
        s_rng = 0x2545F491u ^ (static_cast<u32>(slot + 1) * 0x9E3779B9u) ^ static_cast<u32>(NetplayGetTimeUs());
    struct R { static u32 next(u32& r) { r = r * 1664525u + 1013904223u; return r >> 8; } };
    const u64 now = NetplayGetTimeUs();
    if (now >= s_nextChangeUs)
    {
        memset(&s_cur, 0, sizeof(s_cur));
        const float angle = static_cast<float>(R::next(s_rng) % 360) * 0.0174533f;
        const u32 m = R::next(s_rng) % 100;
        const float mag = m < 12 ? 0.0f : (m < 75 ? 1.0f : 0.5f);
        s_cur.stickX = static_cast<s8>(cosf(angle) * 56.0f * mag);
        s_cur.stickY = static_cast<s8>(sinf(angle) * 56.0f * mag);
        const u32 b = R::next(s_rng) % 100;
        if (b < 25)       s_cur.button = 0x0100;   /* A: pass */
        else if (b < 47)  s_cur.button = 0x0200;   /* B: shoot (held = charged, Super Strike) */
        else if (b < 55)  s_cur.button = 0x0400;   /* X */
        else if (b < 63)  s_cur.button = 0x0800;   /* Y */
        else if (b < 75) { s_cur.button = 0x0020; s_cur.triggerRight = 255; }   /* R: turbo */
        else if (b < 80)  s_cur.button = 0x0010;   /* Z: item */
        s_nextChangeUs = now + 150000 + (R::next(s_rng) % 1300000);
        const bool tap = (R::next(s_rng) % 2) == 0;
        s_releaseUs = now + (tap ? 80000 : 200000 + (R::next(s_rng) % 1800000));
    }
    NetplayInput in = s_cur;
    if (now >= s_releaseUs)
    {
        in.button = 0;
        in.triggerRight = 0;
    }
    return in;
}

void NetplayManager::CaptureLocalInput()
{
    if (PortAutotestActive())
    {
        m_capturedLocal = AutotestInput(m_localSlot);
        m_capturedLocal.remap = static_cast<u16>(PortCameraJoystickRemap() & 0xFFC0u);
        return;
    }

    /* The local player's controller or keyboard is always port 0 on their
       own PC, whichever slot they play as online.  (The old code read the
       pad of the player's slot, so Player 2 read port 1 — which only ever
       held netplay's own override — and never moved.) */
    PADStatus status[4];
    memset(status, 0, sizeof(status));
    PADRead(status);
    /* The same circle clamp the game's own pad read applies (VBlankPadUpdate).
       Without it the injected sticks reached up to 127 instead of ~56, so
       every player ran and dribbled as if the stick were pushed twice as far:
       the "weird ball animation" that only happened online. */
    PADClampCircle(status);
    if (status[0].err == 0)
        NetplayInputFromPAD(&m_capturedLocal, &status[0]);
    else
        m_capturedLocal = NeutralInput();
    /* The camera angle this player sees turns their stick (quantised a little
       so it does not change every frame for nothing). */
    m_capturedLocal.remap = static_cast<u16>(PortCameraJoystickRemap() & 0xFFC0u);

    /* Controllers reach every window, focused or not.  With two copies of the
       game on one PC both would play with the same controller, so only the
       focused window takes input (STRIKERS_NETPLAY_BACKGROUND_INPUT=1 turns
       this off). */
    static const bool s_backgroundInput = std::getenv("STRIKERS_NETPLAY_BACKGROUND_INPUT") != nullptr;
    if (!s_backgroundInput && SDL_GetKeyboardFocus() == nullptr)
    {
        u16 remap = m_capturedLocal.remap;
        m_capturedLocal = NeutralInput();
        m_capturedLocal.remap = remap;
    }
}

void NetplayManager::ReleaseVirtualPads()
{
    /* Only port 0, the one this PC's controller is read from.  Port 1 must
       stay "connected" for the whole session: the game decides which teams
       are human when a match loads, by which ports have a controller, and an
       empty port 1 made the away team computer-controlled. */
    PADClearVirtualStatus(0);
    PortSetPadRemapActive(0);   /* render-rate code uses the camera again */
}


void NetplayManager::BeginSimRng()
{
    if (m_inSimRng || PortNetplayDiag(2))
        return;
    m_inSimRng = true;
    /* Park the render-side generators... */
    netplay_mt_get_state(m_renderMt, &m_renderMti);
    m_renderNlSeed = nlDefaultSeed;
    /* ...and continue gameplay's own sequence (before the match's first
       frame there is none yet: frame 0 seeds it). */
    if (m_simRngValid)
    {
        netplay_mt_set_state(m_simMt, m_simMti);
        nlDefaultSeed = m_simNlSeed;
    }
}

void NetplayManager::EndSimRng()
{
    if (!m_inSimRng)
        return;
    m_inSimRng = false;
    if (m_rngSeeded)
    {
        netplay_mt_get_state(m_simMt, &m_simMti);
        m_simNlSeed = nlDefaultSeed;
        m_simRngValid = true;
    }
    netplay_mt_set_state(m_renderMt, m_renderMti);
    nlDefaultSeed = m_renderNlSeed;
}

/* ======================================================================== */
/*  Resync when play resumes                                                 */
/* ======================================================================== */

void NetplayManager::SendResync(u8 type, u32 value)
{
    m_resyncSendUs = NetplayGetTimeUs();
    NetplayResyncPacket rp;
    memset(&rp, 0, sizeof(rp));
    rp.epoch = (type == NETPLAY_PKT_RESYNC_GO && m_resyncPhase == RESYNC_NONE) ? m_doneEpoch : m_resyncEpoch;
    rp.value = value;
    SendGamePacket(&rp, sizeof(rp), type);
}

void NetplayManager::CopySavedState(SavedState& dst, const SavedState& src)
{
    if (src.data == nullptr)
        return;
    if (dst.data == nullptr || dst.capacity < src.size)
    {
        dst.data     = static_cast<u8*>(realloc(dst.data, src.size));
        dst.capacity = src.size;
    }
    memcpy(dst.data, src.data, src.size);
    dst.size     = src.size;
    dst.frame    = src.frame;
    dst.checksum = src.checksum;
}

void NetplayManager::OnLeaveGameplay(u32 lastSimulatedFrame)
{
    s32 f = static_cast<s32>(lastSimulatedFrame) + 1;
    if (f > m_noRollbackBefore)
        m_noRollbackBefore = f;
    if (m_firstMispredict >= 0 && m_firstMispredict < m_noRollbackBefore)
        m_firstMispredict = -1;
    if (m_breakStartUs == 0)
    {
        m_breakStartUs = NetplayGetTimeUs();
        fprintf(stderr, "[netplay] play stopped after frame %u (goal, replay or pause)\n", lastSimulatedFrame);
    }
}

void NetplayManager::OnEnterGameplay()
{
    if (m_state != NETPLAY_STATE_RUNNING)
        return;
    m_gameplayEnteredUs = NetplayGetTimeUs();
    /* The first time a match reaches gameplay the starting-state transfer
       handles it; every later return needs a resync. */
    if (m_lastSimFrame >= 0)
        m_resyncOnReturn = true;
}

void NetplayManager::BeginResync()
{
    m_resyncPhase         = RESYNC_WAITING;
    m_resyncEpoch         = static_cast<u8>(m_doneEpoch + 1);
    m_localReadyForResync = false;
    m_resyncLoaded        = false;
    m_resyncSendUs        = 0;
    m_countdownEndUs      = 0;
    m_initialStateReady   = false;
    m_initialStateAcked   = false;
    fprintf(stderr, "[netplay] resync %u: waiting for both players\n", m_resyncEpoch);
}

void NetplayManager::StepResync()
{
    /* After a goal, replay or pause.  Two things differ between the PCs by
       now: the replay switches on by each PC's own clock, so they stopped at
       different game frames, and the replays touch a little game state at
       render speed (flick history, ball forces).  So the host sends its state
       and frame number, the guest takes both, and a countdown starts both
       games together.  In lockstep everything outside the save state is still
       identical, so the guest becomes an exact copy of the host. */
    u64 now = NetplayGetTimeUs();

    if (!m_localReadyForResync)
    {
        m_localReadyForResync = true;
        u64 breakUs = m_lastFrameRunUs ? now - m_lastFrameRunUs : 0;
        m_countdownMs = (breakUs >= kLongBreakUs) ? kCountdownLongMs : kCountdownShortMs;
        SendResync(NETPLAY_PKT_RESYNC_READY, m_simFrame);
    }

    if (m_resyncPhase == RESYNC_WAITING)
    {
        if (m_localSlot != 0 || m_peerReadyEpoch != m_resyncEpoch)
            return;
        /* Host: both are back.  This game's state is the one both go on from. */
        m_resyncFrame = static_cast<s32>(m_simFrame);
        NetplayFlattenAnimBlends();
        SaveSnapshot(m_resyncState, m_resyncFrame - 1);
        WriteSyncDump(m_resyncEpoch);
        m_txState = &m_resyncState;
        m_txEpoch = m_resyncEpoch;
        m_txFrame = m_resyncFrame;
        m_initialStateReady = true;
        m_initialStateAcked = false;
        SendInitialState();
        m_resyncPhase = RESYNC_TRANSFER;
        fprintf(stderr, "[netplay] resync %u: sending state at frame %d (%u bytes)\n",
                m_resyncEpoch, m_resyncFrame, m_resyncState.size);
        return;
    }

    if (m_localSlot != 0 && !m_resyncLoaded && m_rxEpoch == m_resyncEpoch &&
        m_rxChunksNeeded != 0 && m_rxChunksHave == m_rxChunksNeeded)
    {
        /* Guest: take the host's state and frame number. */
        static SaveStateBuffer s_resyncBuf;
        s_resyncBuf.Reset();
        s_resyncBuf.Reserve(m_rxStateSize);
        s_resyncBuf.WriteBytes(m_rxState, m_rxStateSize);
        float savedAccumulator = FixedUpdateTask::mAccumulatedDeltaT;
        bool ok = LoadGameState(&s_resyncBuf);
        FixedUpdateTask::mAccumulatedDeltaT = savedAccumulator;
        VerifyLoadedState(m_rxState, m_rxStateSize, "resync state");
        WriteSyncDump(m_resyncEpoch);
        fprintf(stderr, "[netplay] resync %u: loaded the host's state at frame %d (this game was at %u, %s)\n",
                m_resyncEpoch, m_rxFrame, m_simFrame, ok ? "ok" : "FAILED");
        m_resyncFrame = m_rxFrame;
        m_simFrame    = static_cast<u32>(m_rxFrame);
        SaveSnapshot(m_resyncState, m_resyncFrame - 1);
        m_resyncLoaded = true;
    }

    if (m_resyncPhase == RESYNC_TRANSFER)
    {
        if (m_localSlot == 0 && m_initialStateAcked)
        {
            m_countdownEndUs = now + static_cast<u64>(m_countdownMs) * 1000;
            m_resyncPhase    = RESYNC_COUNTDOWN;
            SendResync(NETPLAY_PKT_RESYNC_GO, m_countdownMs);
            fprintf(stderr, "[netplay] resync %u: guest has the state, countdown %u ms\n",
                    m_resyncEpoch, m_countdownMs);
        }
        return;
    }

    if (m_resyncPhase == RESYNC_COUNTDOWN)
    {
        if (m_localSlot != 0 && !m_resyncLoaded)
            return;
        if (now >= m_countdownEndUs)
            FinishResync();
    }
}

void NetplayManager::FinishResync()
{
    /* Both games are now at frame F with the same state.  Inputs start over
       from F: anything queued from before the break was for frames one of the
       games may never have run. */
    const s32 F = m_resyncFrame;

    for (int i = 0; i < NETPLAY_INPUT_RING_SIZE; i++)
    {
        m_localInputs[i].frame      = -1;
        m_remoteInputs[i].frame     = -1;
        m_usedRemoteInputs[i].frame = -1;
    }
    for (int d = 0; d < m_inputDelay; d++)
    {
        m_localInputs[Ring(F + d)].frame = F + d;
        m_localInputs[Ring(F + d)].input = NeutralInput();
    }
    m_localInputLatest = F + m_inputDelay - 1;
    m_peerAckedFrame   = F - 1;
    m_remoteLatest     = F - 1;
    m_remoteConfirmed  = F - 1;
    m_lastSimFrame     = F - 1;
    m_firstMispredict  = -1;
    m_simFrame         = static_cast<u32>(F);

    for (int i = 0; i < NETPLAY_STATE_BUFFER_SIZE; i++)
        m_savedStates[i].frame = -2;
    CopySavedState(m_savedStates[StateRing(F - 1)], m_resyncState);
    m_savedStates[StateRing(F - 1)].frame = F - 1;

    m_localFrameAdvantage  = 0;
    m_remoteFrameAdvantage = 0;
    m_advantageAvg         = 0.0f;
    m_framesToStall        = 0;
    m_lastStallFrame       = -1000;
    m_lastSyncCheckFrame   = F;

    for (int i = 0; i < 16; i++)
    {
        m_localSums[i].frame  = -1;
        m_remoteSums[i].frame = -1;
    }
    m_segmentStartFrame = F;
    m_lastChecksumSent  = ((F + kChecksumInterval - 1) / kChecksumInterval) * kChecksumInterval - kChecksumInterval;
    m_noRollbackBefore  = F;
    m_desyncFrame       = -1;   /* a fresh start: report the next desync too */

    m_doneEpoch           = m_resyncEpoch;
    m_resyncPhase         = RESYNC_NONE;
    ResetLeave();
    m_reloadState         = &m_resyncState;
    m_localReadyForResync = false;
    m_initialStateReady   = false;
    m_breakStartUs        = 0;
    m_lastRecvUs          = NetplayGetTimeUs();
    m_lastFrameRunUs      = m_lastRecvUs;
    fprintf(stderr, "[netplay] resync %u done: both playing from frame %d\n", m_doneEpoch, F);
}

float NetplayManager::GetCountdownSeconds() const
{
    if (m_resyncPhase != RESYNC_COUNTDOWN)
        return 0.0f;
    u64 now = NetplayGetTimeUs();
    return m_countdownEndUs > now ? static_cast<float>(m_countdownEndUs - now) / 1000000.0f : 0.0f;
}

void NetplayManager::MirrorLocalPadForMenus()
{
    /* Outside gameplay (replays, celebrations, pause menu) the guest's own
       controller also drives port 1, so the guest can skip and use menus. */
    if (m_state != NETPLAY_STATE_RUNNING || m_localSlot != 1)
        return;
    PADStatus status[4];
    memset(status, 0, sizeof(status));
    PADRead(status);
    if (status[0].err == 0)
        PADSetVirtualStatus(1, &status[0]);
}

void NetplayManager::ReapplyLastInputs()
{
    if (m_state == NETPLAY_STATE_RUNNING && m_lastSimFrame >= 0)
    {
        InputSlot saved = m_usedRemoteInputs[Ring(m_lastSimFrame)];
        InjectInputs(m_lastSimFrame);
        m_usedRemoteInputs[Ring(m_lastSimFrame)] = saved;
    }
}

/* ======================================================================== */
/*  Coordinated leave                                                        */
/* ======================================================================== */
/*  Goals, replays, cutscenes, the pause menu and the Super Strike effects all
    leave gameplay through nlTaskManager::SetNextState.  The call comes from
    render-rate code (the presentation scripts, the menus), so each PC made it
    after a different game frame: one game ran a frame or two more than the
    other, and everything not in the save state (cameras, effects, the
    presentation itself) went its own way.  Now the request is held back:

      - the game that asks proposes "last frame = the newest frame I have sent
        input for".  Without our input for later frames the opponent cannot
        run past it, so the proposal is always still reachable;
      - the other game answers with its own proposal, or with "none" if it has
        not asked yet (it then can never ask with a different frame);
      - both play to the later of the two proposals and change state there.

    A game that reaches the agreed frame without a request of its own (the
    opponent paused) holds there; if its own request comes it goes at once,
    otherwise after a moment it starts the usual resync and waits.          */

void NetplayManager::ResetLeave()
{
    m_leaveDecided     = false;
    m_leaveMyStop      = -1;
    m_leavePeerKnown   = false;
    m_leavePeerStop    = -1;
    m_leavePeerSeen    = -2;
    m_leaveState       = 0;
    m_leaveDone        = false;
    m_leaveHoldSinceUs = 0;
    m_leaveSendUs      = 0;
    m_leaveRecovery    = false;
}

bool NetplayManager::LeaveFinalKnown() const
{
    return m_leaveDecided && m_leavePeerKnown;
}

s32 NetplayManager::LeaveFinal() const
{
    return m_leaveMyStop > m_leavePeerStop ? m_leaveMyStop : m_leavePeerStop;
}

s32 NetplayManager::LeaveInputCap() const
{
    if (!m_leaveDecided)
        return 0x7FFFFFFF;
    if (m_leavePeerKnown)
        return LeaveFinal();
    return m_leaveMyStop;
}

void NetplayManager::SendLeave()
{
    NetplayLeavePacket lp;
    memset(&lp, 0, sizeof(lp));
    lp.epoch     = m_doneEpoch;
    lp.needReply = (!m_leavePeerKnown || m_leavePeerSeen != m_leaveMyStop) ? 1 : 0;
    lp.myStop    = m_leaveMyStop;
    lp.seenStop  = m_leavePeerKnown ? m_leavePeerStop : -2;
    lp.recovery  = m_leaveRecovery ? 1 : 0;
    SendGamePacket(&lp, sizeof(lp), NETPLAY_PKT_LEAVE);
    m_leaveSendUs = NetplayGetTimeUs();
}

void NetplayManager::HandleLeave(const NetplayLeavePacket* lp)
{
    if (m_state != NETPLAY_STATE_RUNNING || lp->epoch != m_doneEpoch)
        return;   /* from an earlier stop */

    if (!m_leavePeerKnown)
    {
        if (lp->myStop < 0 && !m_leaveDecided)
            return;   /* "none" can only answer a request of ours */
        m_leavePeerKnown = true;
        m_leavePeerStop  = lp->myStop;
        if (!m_leaveDecided)
        {
            m_leaveDecided = true;
            m_leaveMyStop  = -1;
            fprintf(stderr, "[netplay] opponent stops play after frame %d (this game was at frame %d)\n",
                    lp->myStop, m_lastSimFrame);
        }
        fprintf(stderr, "[netplay] both games stop after frame %d (ours %d, theirs %d)\n",
                LeaveFinal(), m_leaveMyStop, m_leavePeerStop);
    }
    if (lp->recovery && !m_leaveRecovery)
    {
        m_leaveRecovery = true;
        fprintf(stderr, "[netplay] the host is repairing a desync: stopping to resync\n");
    }
    m_leavePeerSeen = lp->seenStop;
    if (lp->needReply)
        SendLeave();
}

bool NetplayManager::InterceptStateChange(u32 currState, u32 nextState)
{
    if (m_applyingState || m_state != NETPLAY_STATE_RUNNING || m_maxRollback != 0)
        return false;
    if (currState == 2 && nextState == 2 && m_leaveDecided && !m_leaveDone && m_leaveState == 1)
    {
        /* Asked to leave, then changed its mind (paused and unpaused at once). */
        fprintf(stderr, "[netplay] game no longer wants to leave gameplay\n");
        m_leaveState = 0;
        return false;
    }
    if (currState != 2 || nextState == 2 || m_lastSimFrame < 0)
        return false;
    if (m_resyncPhase != RESYNC_NONE)
        return false;
    if (nlTaskManager::m_pInstance == nullptr || nlTaskManager::m_pInstance->m_PendingState != 2)
        return false;

    m_leaveState = nextState;
    if (m_leaveDone)
    {
        /* Already holding at the agreed frame: go now. */
        fprintf(stderr, "[netplay] leaving gameplay now (state 0x%X) at the agreed frame %d\n",
                nextState, m_lastSimFrame);
        OnLeaveGameplay(static_cast<u32>(m_lastSimFrame));
        return false;
    }
    if (!m_leaveDecided)
    {
        m_leaveDecided = true;
        m_leaveMyStop  = m_localInputLatest > m_lastSimFrame ? m_localInputLatest : m_lastSimFrame;
        fprintf(stderr, "[netplay] game asks to leave gameplay (state 0x%X) after frame %d; proposing to stop after frame %d\n",
                nextState, m_lastSimFrame, m_leaveMyStop);
        SendLeave();
    }
    return true;
}

void NetplayManager::RequestRecoveryStop(s32 desyncFrame)
{
    /* The two games drifted apart.  Rather than play on in two different
       matches until the next goal, both stop a few frames from now, the host's
       state is copied over and play goes on: a short hitch.  At most once
       every 1.5 s (a game left apart longer drifts into a goal on one PC
       only), and only when no other stop is under way. */
    if (m_localSlot != 0 || m_state != NETPLAY_STATE_RUNNING || m_maxRollback != 0)
        return;
    if (m_leaveDecided || m_resyncPhase != RESYNC_NONE || m_lastSimFrame < 0)
        return;
    const u64 now = NetplayGetTimeUs();
    if (m_lastRecoveryUs != 0 && now - m_lastRecoveryUs < 1500000ULL)
        return;
    m_lastRecoveryUs = now;
    m_leaveDecided   = true;
    m_leaveRecovery  = true;
    m_leaveState     = 0;
    m_leaveMyStop    = m_localInputLatest > m_lastSimFrame ? m_localInputLatest : m_lastSimFrame;
    fprintf(stderr, "[netplay] games differ since frame %d: stopping after frame %d to copy the host's game over\n",
            desyncFrame, m_leaveMyStop);
    SendLeave();
}

bool NetplayManager::StepLeave(s32 frame)
{
    if (!m_leaveDecided)
        return false;
    if (!m_leavePeerKnown)
        return frame > m_leaveMyStop;   /* wait for the answer */

    const s32 fin = LeaveFinal();
    if (frame <= fin)
        return false;

    u64 now = NetplayGetTimeUs();
    if (!m_leaveDone)
    {
        m_leaveDone        = true;
        m_leaveHoldSinceUs = now;
        OnLeaveGameplay(static_cast<u32>(fin));
        if (m_leaveState != 0)
        {
            fprintf(stderr, "[netplay] reached the agreed frame %d: leaving gameplay (state 0x%X)\n",
                    fin, m_leaveState);
            m_applyingState = true;
            nlTaskManager::SetNextState(m_leaveState);
            m_applyingState = false;
        }
        else
        {
            fprintf(stderr, "[netplay] reached the agreed frame %d: holding while the opponent is away\n", fin);
        }
        return true;
    }

    if (m_leaveState == 0)
        SkipWanted();   /* holding while the opponent watches a replay: A or B skips it there */
    if (m_leaveState == 0 && m_resyncPhase == RESYNC_NONE &&
        (m_leaveRecovery || now - m_leaveHoldSinceUs > kLeaveHoldUs))
    {
        fprintf(stderr, "[netplay] this game stays in gameplay; getting ready to resume\n");
        BeginResync();
    }
    return true;
}

bool NetplayManager::IsOpponentWaiting() const
{
    if (m_state != NETPLAY_STATE_RUNNING)
        return false;
    if (m_lastSimFrame < 0)
    {
        /* Match start: the guest sends inputs once it is in gameplay; the host
           sends its starting state once it is. */
        if (m_localSlot == 0)
            return m_remoteLatest >= 0;
        return m_rxEpoch == 0 && m_rxChunksNeeded != 0 && m_rxChunksHave == m_rxChunksNeeded;
    }
    return m_peerReadyEpoch == static_cast<u8>(m_doneEpoch + 1) && !m_localReadyForResync;
}

void NetplayManager::LocalSkip()
{
    if (m_state != NETPLAY_STATE_RUNNING)
        return;
    if (m_localSkipSends == 0 || m_localSkipEpoch != m_doneEpoch)
        fprintf(stderr, "[netplay] skipped the replay/cutscene: telling the opponent\n");
    m_localSkipEpoch  = m_doneEpoch;
    m_localSkipSends  = 5;
    m_localSkipSendUs = 0;
}

bool NetplayManager::SkipWanted()
{
    if (m_state != NETPLAY_STATE_RUNNING)
        return false;

    /* A or B (or START) on this PC's controller, focused window only. */
    PADStatus status[4];
    memset(status, 0, sizeof(status));
    PADRead(status);
    u16 buttons = (status[0].err == 0) ? status[0].button : 0;
    if (SDL_GetKeyboardFocus() == nullptr && std::getenv("STRIKERS_NETPLAY_BACKGROUND_INPUT") == nullptr)
        buttons = 0;
    /* Only a fresh press counts: a B still held from the shot that scored
       must not skip the replay the moment it starts. */
    const u64 now = NetplayGetTimeUs();
    if (now - m_skipLastCallUs > 150000)
        m_skipPrevButtons = buttons;
    m_skipLastCallUs = now;
    const u16 skipButtons = 0x0100 /* A */ | 0x0200 /* B */ | 0x1000 /* START */;
    const bool pressed = (buttons & skipButtons & ~m_skipPrevButtons) != 0;
    m_skipPrevButtons = buttons;
    if (pressed)
    {
        LocalSkip();
        return true;
    }

    if (m_peerSkip && m_peerSkipEpoch == m_doneEpoch)
        return true;
    return IsOpponentWaiting();
}

bool NetplayManager::IsWaitingForOpponentBreak() const
{
    return m_state == NETPLAY_STATE_RUNNING && m_leaveDone && m_leaveState == 0 && !m_leaveRecovery;
}

void NetplayManager::WriteSyncDump(int epoch)
{
    /* Raw game objects at the moment both games should be identical (host:
       the state it sends; guest: right after loading it).  For finding game
       state the save does not cover. */
    char bin[64], txt[64];
    snprintf(bin, sizeof(bin), "syncdump-p%d-e%d.bin", m_localSlot + 1, epoch);
    snprintf(txt, sizeof(txt), "syncdump-p%d-e%d.txt", m_localSlot + 1, epoch);
    NetplayWriteSyncDump(bin, txt);

    /* Guest: the host gets a copy (written there as syncdump-p2-eN.bin). */
    if (m_localSlot != 0)
    {
        FILE* f = fopen(bin, "rb");
        if (f)
        {
            static u8 s_buf[NETPLAY_STATE_MAX_BYTES];
            size_t n = fread(s_buf, 1, sizeof(s_buf), f);
            fclose(f);
            if (m_dumpTxSize == 0)   /* a desync dump being sent goes first */
                QueueDumpForHost(1, static_cast<u32>(epoch), s_buf, static_cast<u32>(n));
        }
    }
}

void NetplayManager::VerifyLoadedState(const u8* expected, u32 size, const char* what)
{
    /* Save again right after loading: anything that comes out different from
       the host's bytes is a field the loader does not fully restore. */
    static SaveStateBuffer s_checkBuf;
    if (s_checkBuf.Size() == 0)
        s_checkBuf.Reserve(32 * 1024);
    SaveGameState(&s_checkBuf);
    const u8* d = s_checkBuf.Data();
    u32 n = static_cast<u32>(s_checkBuf.Size());
    if (n == size && memcmp(d, expected, n) == 0)
    {
        fprintf(stderr, "[netplay] %s: loaded copy checks out (%u bytes)\n", what, n);
        return;
    }
    fprintf(stderr, "[netplay] %s: after loading, this game's state still differs from the host's (%u vs %u bytes):\n",
            what, n, size);
    u32 m = n < size ? n : size;
    const char* lastName = nullptr;
    int shown = 0, diffs = 0;
    for (u32 i = 0; i < m; i++)
    {
        if (d[i] == expected[i])
            continue;
        diffs++;
        size_t start = 0;
        const char* name = NetplayStateFieldAt(i, &start);
        if (name != lastName && shown < 16)
        {
            fprintf(stderr, "[netplay]   %s (byte %u, +%u into the field): host %02X, here %02X\n",
                    name, i, static_cast<unsigned>(i - start), expected[i], d[i]);
            lastName = name;
            shown++;
        }
    }
    fprintf(stderr, "[netplay]   %d bytes differ in all\n", diffs);
}

/* ======================================================================== */
/*  Per-frame hooks                                                          */
/* ======================================================================== */

bool NetplayManager::PreUpdate(u32 simFrame)
{
    if (m_state != NETPLAY_STATE_RUNNING)
        return true;

    ReceivePackets();
    if (m_state != NETPLAY_STATE_RUNNING)
        return true;

    s32 frame = static_cast<s32>(simFrame);

    /* Right after a match starts (and after each goal) the game drops out of
       gameplay for a moment and comes back, at real-time speed: the guest ran
       two frames before that blip, the host none, and the two matches began
       differently.  So frames only run once gameplay has held for a second. */
    {
        u64 now = NetplayGetTimeUs();
        if (m_gameplayEnteredUs == 0)
            m_gameplayEnteredUs = now;
        if (m_lastSimFrame < 0 && now - m_gameplayEnteredUs < kSettleUs)
            return false;
    }

    if (frame == 0 && !m_holdTimesReset)
    {
        /* Button hold times start the match at zero on both PCs, whatever was
           held in the menus. */
        PortPadResetFixedHoldTimes();
        m_holdTimesReset = true;
    }

    if (frame == 0 && m_localSlot == 0 && !m_rngSeeded)
    {
        /* Host: this is the starting state of the match.  Send it. */
        SeedRngForMatch();
        m_rngSeeded = true;
        SaveSnapshot(m_preFrameZeroState, -1);
        WriteSyncDump(0);
        m_reloadState = &m_preFrameZeroState;   /* loaded again at step 3b */
        m_txState = &m_preFrameZeroState;
        m_txEpoch = 0;
        m_txFrame = 0;
        m_initialStateReady = true;
        m_initialStateAcked = false;
        SendInitialState();
        fprintf(stderr, "[netplay] sending starting state for match %u (%u bytes)\n",
                m_matchId, m_preFrameZeroState.size);
    }
    else if (frame == 0 && m_localSlot != 0 && !m_initialStateLoaded)
    {
        /* Guest: the two games loaded the match on their own, with their own
           random numbers and menu timing, so they never start out equal.
           Wait for the host's starting state and load it. */
        if (m_waitStateSinceUs == 0)
            m_waitStateSinceUs = NetplayGetTimeUs();
        if (m_rxEpoch != 0 || m_rxChunksNeeded == 0 || m_rxChunksHave != m_rxChunksNeeded)
        {
            SendInputs();
            return false;
        }

        static SaveStateBuffer s_initBuf;
        s_initBuf.Reset();
        s_initBuf.Reserve(m_rxStateSize);
        s_initBuf.WriteBytes(m_rxState, m_rxStateSize);
        float savedAccumulator = FixedUpdateTask::mAccumulatedDeltaT;
        bool ok = LoadGameState(&s_initBuf);
        FixedUpdateTask::mAccumulatedDeltaT = savedAccumulator;
        VerifyLoadedState(m_rxState, m_rxStateSize, "starting state");
        WriteSyncDump(0);
        fprintf(stderr, "[netplay] loaded the host's starting state (%u bytes, %s, waited %llu ms)\n",
                m_rxStateSize, ok ? "ok" : "FAILED",
                static_cast<unsigned long long>((NetplayGetTimeUs() - m_waitStateSinceUs) / 1000));

        m_initialStateLoaded = true;
        m_rngSeeded = true;
        SaveSnapshot(m_preFrameZeroState, -1);
        m_reloadState = &m_preFrameZeroState;
    }

    /* 0. Back from a goal, replay or pause: wait for the opponent, take the
          host's state, count down. */
    if (m_resyncOnReturn)
    {
        m_resyncOnReturn = false;
        if (m_resyncPhase == RESYNC_NONE)
            BeginResync();
    }
    if (m_resyncPhase != RESYNC_NONE)
    {
        /* Waiting while the opponent watches a replay: A or B skips it there. */
        if (m_resyncPhase == RESYNC_WAITING)
            SkipWanted();
        StepResync();
        return false;
    }

    /* 0b. Leaving gameplay: both games play up to the agreed frame, then stop. */
    if (StepLeave(frame))
    {
        SendInputs();
        return false;
    }

    /* 1. Fix the past first. */
    m_lastRollbackSize = 0;
    if (m_firstMispredict >= 0)
        DoRollback();

    /* A resimulated frame can itself end gameplay (a goal that the corrected
       inputs now score): stop here, the game changes state next frame. */
    if (TaskManagerLeavingGameplay())
    {
        OnLeaveGameplay(static_cast<u32>(m_lastSimFrame));
        return false;
    }

    /* 2. Never run so far ahead of the confirmed inputs that a rollback could
          not find its base state.  Keep sending, so acks still flow. */
    if (frame > m_remoteConfirmed + m_maxRollback)
    {
        m_statWaitInput++;
        SendInputs();
        return false;
    }

    /* 3. Time sync: if we are ahead of the peer, skip a tick now and then.
          Not in slow motion (a Super Strike): a game frame then lasts up to
          ten times longer, so one skipped frame is a visible freeze, and the
          two games' slow motion ends at slightly different moments anyway
          (the camera that ends it runs per rendered frame). */
    {
        static float s_loggedScale = 1.0f;
        const float scale = FixedUpdateTask::mTimeScale;
        if ((scale < 0.99f) != (s_loggedScale < 0.99f))
            fprintf(stderr, "[netplay] slow motion %s at frame %d (x%.3f)\n",
                    scale < 0.99f ? "starts" : "ends", frame, scale);
        s_loggedScale = scale;
    }
    if (FixedUpdateTask::mTimeScale < 0.99f)
    {
        m_framesToStall = 0;
        m_advantageAvg = 0.0f;
        m_lastSyncCheckFrame = frame;
    }
    else if (frame - m_lastSyncCheckFrame >= kSyncCheckInterval)
    {
        m_lastSyncCheckFrame = frame;
        float recommended = (m_remoteFrameAdvantage - m_localFrameAdvantage) * 0.5f;
        m_advantageAvg = m_advantageAvg * 0.6f + recommended * 0.4f;
        if (m_advantageAvg >= 1.0f)
        {
            int stall = static_cast<int>(m_advantageAvg);
            m_framesToStall = stall > kMaxStallPerCheck ? kMaxStallPerCheck : stall;
        }
        else
        {
            m_framesToStall = 0;
        }
    }
    if (m_framesToStall > 0 && frame - m_lastStallFrame >= 3)
    {
        m_statWaitSync++;
        m_framesToStall--;
        m_lastStallFrame = frame;
        SendInputs();
        return false;
    }

    /* 3b. The first frame after a start or resync: both games load the agreed
          state again right now.  While they waited for each other (the
          opponent still in the intro or a replay, the countdown) render-rate
          code kept changing the game, by a different amount on each PC. */
    if (m_reloadState != nullptr)
    {
        const SavedState* rs = m_reloadState;
        m_reloadState = nullptr;
        if (rs->data != nullptr && rs->frame == frame - 1)
        {
            static SaveStateBuffer s_reloadBuf;
            s_reloadBuf.Reset();
            s_reloadBuf.Reserve(rs->size);
            s_reloadBuf.WriteBytes(rs->data, rs->size);
            float savedAccumulator = FixedUpdateTask::mAccumulatedDeltaT;
            bool ok = LoadGameState(&s_reloadBuf);
            FixedUpdateTask::mAccumulatedDeltaT = savedAccumulator;
            fprintf(stderr, "[netplay] reloaded the agreed state before frame %d (%s)\n", frame, ok ? "ok" : "FAILED");
        }
    }

    /* 4. Local input for this tick applies `inputDelay` frames from now. */
    s32 target = frame + m_inputDelay;
    if (m_leaveDecided && target > LeaveInputCap())
        target = LeaveInputCap();   /* the opponent must not run past the stop */
    for (s32 f = m_localInputLatest + 1; f <= target; f++)
    {
        InputSlot& slot = m_localInputs[Ring(f)];
        slot.frame = f;
        slot.input = m_capturedLocal;
    }
    if (target > m_localInputLatest)
        m_localInputLatest = target;

    SendInputs();
    InjectInputs(frame);
    m_lastSimFrame = frame;
    m_lastFrameRunUs = NetplayGetTimeUs();
    return true;
}

void NetplayManager::SaveSnapshot(SavedState& ss, s32 frame)
{
    static SaveStateBuffer s_saveBuf;
    if (s_saveBuf.Size() == 0)
        s_saveBuf.Reserve(32 * 1024);

    SaveGameState(&s_saveBuf);

    u32 size = static_cast<u32>(s_saveBuf.Size());
    if (ss.data == nullptr || ss.capacity < size)
    {
        ss.data     = static_cast<u8*>(realloc(ss.data, size));
        ss.capacity = size;
    }
    memcpy(ss.data, s_saveBuf.Data(), size);
    ss.size     = size;
    ss.frame    = frame;
    ss.checksum = s_saveBuf.Checksum();
}

void NetplayManager::PostUpdate(u32 simFrame)
{
    if (m_state != NETPLAY_STATE_RUNNING)
        return;

    s32 frame = static_cast<s32>(simFrame);
    SavedState& ss = m_savedStates[StateRing(frame)];
    SaveSnapshot(ss, frame);

    if (frame % kChecksumInterval == 0)
    {
        ChecksumEntry& e = m_localSums[(frame / kChecksumInterval) % 16];
        e.frame    = frame;
        e.checksum = ss.checksum;
        /* The goal celebration (between the ball crossing the line and the goal
           cutscene) is played by the presentation scripts at screen rate, so the
           players' celebration moves differ a little on each PC.  Nothing of it
           lasts: both games stop together for the cutscene and the resync makes
           them identical again.  So these frames are not compared. */
        if (NetplayIsGoalCelebration())
            e.checksum = 0xC0A1C0A1u;
        KeepChecksumState(frame, ss);
    }

    SendChecksums();
    CheckDesync();
}

/* ======================================================================== */
/*  Rollback                                                                 */
/* ======================================================================== */

int NetplayManager::DoRollback()
{
    s32 first = m_firstMispredict;
    s32 last  = m_lastSimFrame;
    m_firstMispredict = -1;

    if (first < 0 || first > last)
        return 0;

    if (first < m_noRollbackBefore)
    {
        /* Gameplay was left (goal, pause...) after that frame; the game's own
           state machine has moved on and cannot be rewound.  The resync when
           play resumes makes both games equal again. */
        fprintf(stderr, "[netplay] not rolling back to frame %d: play stopped after frame %d\n",
                first, m_noRollbackBefore - 1);
        m_lastRollbackSize = 0;
        return 0;
    }

    int count = static_cast<int>(last - first + 1);
    SavedState& base = (first == 0) ? m_preFrameZeroState
                                    : m_savedStates[StateRing(first - 1)];

    if (count > NETPLAY_STATE_BUFFER_SIZE - 1 || base.frame != first - 1 || base.data == nullptr)
    {
        fprintf(stderr, "[netplay] rollback impossible: no saved state for frame %d "
                        "(would resimulate %d frames)\n", first - 1, count);
        m_lastRollbackSize = 0;
        return 0;
    }

    static SaveStateBuffer s_loadBuf;
    s_loadBuf.Reset();
    s_loadBuf.Reserve(base.size);
    s_loadBuf.WriteBytes(base.data, base.size);

    /* The render-time accumulator is not simulation state. */
    float savedAccumulator = FixedUpdateTask::mAccumulatedDeltaT;
    LoadGameState(&s_loadBuf);
    FixedUpdateTask::mAccumulatedDeltaT = savedAccumulator;

    for (s32 f = first; f <= last; f++)
    {
        InjectInputs(f);

        UseFixedUpdatePad();
        PortPadStepFixedHoldTimes(g_fFixedUpdateTick);
        UpdatePlatPad(g_fFixedUpdateTick);
        cPadManager::Update(g_fFixedUpdateTick);
        FlickDetection::Update();
        FixedUpdateTask::CallFixedUpdateTasks();
        ClockManager::Update(g_fFixedUpdateTick);       /* as FixedUpdateTask does online */
        WorldManager::UpdateWorld(g_fFixedUpdateTick);

        SavedState& ss = m_savedStates[StateRing(f)];
        SaveSnapshot(ss, f);
        if (f % kChecksumInterval == 0)
        {
            ChecksumEntry& e = m_localSums[(f / kChecksumInterval) % 16];
            e.frame    = f;
            e.checksum = ss.checksum;
            KeepChecksumState(f, ss);
        }
    }

    m_lastRollbackSize = count;
    return count;
}

int NetplayManager::CheckAndPerformRollback(u32 currentFrame)
{
    (void)currentFrame;
    if (m_state != NETPLAY_STATE_RUNNING || m_firstMispredict < 0)
        return 0;
    return DoRollback();
}

/* Called by nlTaskManager::SetNextState. */
extern "C" int PortNetplayInterceptStateChange(unsigned int currState, unsigned int nextState)
{
    return NetplayManager::Instance().InterceptStateChange(currState, nextState) ? 1 : 0;
}

/* Presentation::DetectSkipPress: the opponent is in gameplay and waits for
   this game (still in the intro, a replay or a cutscene). */
extern "C" int PortNetplayOpponentWaiting(void)
{
    return NetplayManager::Instance().IsOpponentWaiting() ? 1 : 0;
}

/* Game code that must behave the same on both PCs while a session runs. */
extern "C" int PortNetplayRunning(void)
{
    return NetplayManager::Instance().IsActive() ? 1 : 0;
}

extern "C" int PortNetplaySkipWanted(void)
{
    return NetplayManager::Instance().SkipWanted() ? 1 : 0;
}

extern "C" void PortNetplayLocalSkip(void)
{
    NetplayManager::Instance().LocalSkip();
}

/* Presentation::WaitForCharacterDirection: this game holds its game frames
   while the opponent's game is in a cutscene or replay. */
extern "C" int PortNetplayHoldingForPeer(void)
{
    return NetplayManager::Instance().IsWaitingForOpponentBreak() ? 1 : 0;
}

/* PowerupBase::AwardPowerup: online, seed the item roll from the game frame,
   the team and the match's seed, so both PCs pick the same item wherever the
   award is reached from.  Returns 0 (seed untouched) offline. */
extern "C" int PortNetplayPowerupSeed(unsigned int* pSeed, int side)
{
    NetplayManager& np = NetplayManager::Instance();
    if (!np.IsActive() || pSeed == NULL)
        return 0;
    const u32 frame = np.GetSimFrame();
    if (!np.InSimRng())
        fprintf(stderr, "[netplay] power-up for team %d rolled outside a game frame (frame %u)\n",
                side, (unsigned)frame);
    u32 h = np.GetMatchSeed() ^ 0x9E3779B9u;
    h ^= frame * 0x85EBCA6Bu;
    h ^= (u32)(side + 1) * 0xC2B2AE35u;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    *pSeed = h ? h : 1u;
    return 1;
}
