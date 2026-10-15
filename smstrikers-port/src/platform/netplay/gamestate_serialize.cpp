/*
 * gamestate_serialize.cpp — Save and restore the deterministic game state.
 *
 * This file implements SaveGameState() and LoadGameState() for rollback.
 *
 * The state we capture covers every element that affects the simulation:
 *   - Mersenne Twister RNG (mt[624] + mti)
 *   - nlDefaultSeed (NL random)
 *   - FixedUpdateTask timing accumulators
 *   - cGame match state (scores, timers, possession, game state machine)
 *   - cTeam state (score, items, meters, AI timers)
 *   - cBall state (position, velocity, ownership, timers)
 *   - All 10 character states (position, velocity, facing, animation, AI)
 *   - ODE physics bodies (11 bodies: 1 ball + 10 players)
 *   - Pad-derived state (FlickDetection ring buffers, PadStatus button times)
 *
 * Audio and rendering state are NOT captured — they recover naturally.
 *
 * Total snapshot size is approximately 17-20 KB per frame.
 */

#include "port/netplay/savestate.h"

#include "Game/Game.h"
#include "Game/GameInfo.h"
#include "Game/Ball.h"
#include "Game/Team.h"
#include "Game/Character.h"
#include "Game/CharacterTemplate.h"
#include "Game/Player.h"
#include "Game/AI/Fielder.h"
#include "Game/Goalie.h"
#include "Game/FixedUpdateTask.h"
#include "Game/Physics/Physics.h"
#include "Game/Physics/PhysicsObject.h"
#include "Game/Physics/PhysicsAIBall.h"
#include "Game/Pad/FlickDetection.h"
#include "Game/Core/mtRandom.h"
#include "Game/Sys/clock.h"
#include "Game/Render/NetMesh.h"
#include "Game/Formation.h"
#include "Game/Render/Bowser.h"
#include "Game/Render/ChainChomp.h"
#include "Game/Render/NPCManager.h"
#include "Game/BasicStadium.h"
#include "Game/FormationDefines.h"
#include "NL/nlMemory.h"
#include <new>
#include "NL/nlDLRing.h"
#include "Game/SAnim/pnSAnimController.h"
#include "Game/SAnim/pnBlender.h"
#include "Game/SAnim/pnSingleAxisBlender.h"
#include "Game/SAnim/pnFeather.h"
#include "NL/nlMath.h"
#include "NL/platpad.h"

#include <ode/ode.h>
#include <cstring>
#include <cstdint>

static void SaveClock(SaveStateBuffer* buf, Clock* clk)
{
    if (!clk)
    {
        buf->WriteU8(0);
        return;
    }
    buf->WriteU8(1);
    buf->WriteF32(clk->m_fTimeScale);
    buf->WriteF32(clk->m_fTimer);
    buf->WriteF32(clk->m_fEndTime);
    buf->WriteU32(clk->m_uActiveStates);
    buf->WriteU32(static_cast<u32>(clk->m_clockState));
}

static void LoadClock(SaveStateBuffer* buf, Clock* clk)
{
    u8 hasClk = buf->ReadU8();
    if (!hasClk || !clk)
        return;
    clk->m_fTimeScale = buf->ReadF32();
    clk->m_fTimer = buf->ReadF32();
    clk->m_fEndTime = buf->ReadF32();
    clk->m_uActiveStates = buf->ReadU32();
    clk->m_clockState = static_cast<eClockState>(buf->ReadU32());
}

/* External globals we need to save/restore. */
extern cGame*     g_pGame;
extern cTeam*     g_pTeams[2];
extern cBall*     g_pBall;
extern cCharacter* g_pCharacters[10];
extern PhysicsWorld* g_PhysicsWorld;
extern unsigned int nlDefaultSeed;

/* Access to Mersenne Twister internals — we add accessor functions. */
extern "C" {
unsigned long netplay_mt_get_state(unsigned long* out_mt, int* out_mti);
void netplay_mt_set_state(const unsigned long* in_mt, int in_mti);

/* Access to FlickDetection ring buffer internals. */
char netplay_flick_get_history(int pad, char* out_buf, int* out_size, int* out_end);
void netplay_flick_set_history(int pad, const char* in_buf, int in_size, int in_end);
}

/* AIPad.cpp: the camera angle that turns each port's stick online. */
extern "C" void PortSetPadRemap(int port, unsigned short remap);
extern "C" unsigned short PortGetPadRemap(int port);

/* Game.cpp helpers (see there for why). */
int  PortNetplayGetThoughts(unsigned long* out, int max);
void PortNetplaySetThoughts(const unsigned long* in, int count);
void PortNetplayDetachBallUnlessHeldBy(cPlayer* keep);
void PortNetplayAttachBallToOwner();

/* FixedUpdateTask statics. */
extern float g_fFixedUpdateTick;
extern float g_fSimulationTick;

/* ======================================================================== */
/*  State version — bump when adding/removing fields.                        */
/* ======================================================================== */

#define SAVESTATE_VERSION 12   /* 12: Bowser and the Chain Chomp, 11: every character's whole animation tree,  2: AI thoughts queue, 3: every running clock, 4: ball owner first, 5: fielder desire vars, 6: clocks by identity, 7: stick camera angles, 8: player update order, 9: goal nets and ball physics flags, 10: team formations */
#define SAVESTATE_MAGIC   0x53535356u  /* "SSSV" = Strikers SaveState Version */

/* ======================================================================== */
/*  Helper: save/restore an ODE rigid body                                   */
/* ======================================================================== */

static void SaveODEBody(SaveStateBuffer* buf, dBodyID body)
{
    if (!body)
    {
        buf->WriteU8(0);  /* no body */
        return;
    }
    buf->WriteU8(1);  /* has body */

    const dReal* pos = dBodyGetPosition(body);
    buf->WriteF32(static_cast<f32>(pos[0]));
    buf->WriteF32(static_cast<f32>(pos[1]));
    buf->WriteF32(static_cast<f32>(pos[2]));

    const dReal* quat = dBodyGetQuaternion(body);
    buf->WriteF32(static_cast<f32>(quat[0]));
    buf->WriteF32(static_cast<f32>(quat[1]));
    buf->WriteF32(static_cast<f32>(quat[2]));
    buf->WriteF32(static_cast<f32>(quat[3]));

    const dReal* lvel = dBodyGetLinearVel(body);
    buf->WriteF32(static_cast<f32>(lvel[0]));
    buf->WriteF32(static_cast<f32>(lvel[1]));
    buf->WriteF32(static_cast<f32>(lvel[2]));

    const dReal* avel = dBodyGetAngularVel(body);
    buf->WriteF32(static_cast<f32>(avel[0]));
    buf->WriteF32(static_cast<f32>(avel[1]));
    buf->WriteF32(static_cast<f32>(avel[2]));

    /* Force/torque accumulators are zeroed before each step, but save them
       to be thorough. */
    const dReal* force = dBodyGetForce(body);
    buf->WriteF32(static_cast<f32>(force[0]));
    buf->WriteF32(static_cast<f32>(force[1]));
    buf->WriteF32(static_cast<f32>(force[2]));

    const dReal* torque = dBodyGetTorque(body);
    buf->WriteF32(static_cast<f32>(torque[0]));
    buf->WriteF32(static_cast<f32>(torque[1]));
    buf->WriteF32(static_cast<f32>(torque[2]));

    buf->WriteU8(dBodyIsEnabled(body) ? 1 : 0);
}

static void LoadODEBody(SaveStateBuffer* buf, dBodyID body)
{
    u8 hasBody = buf->ReadU8();
    if (!hasBody)
        return;
    if (!body)
    {
        /* Saved with a body, none here: read past it so what follows stays in step. */
        for (int i = 0; i < 19; i++)
            (void)buf->ReadF32();
        (void)buf->ReadU8();
        return;
    }

    dReal pos[3], quat[4], lvel[3], avel[3], force[3], torque[3];

    pos[0] = buf->ReadF32(); pos[1] = buf->ReadF32(); pos[2] = buf->ReadF32();
    quat[0] = buf->ReadF32(); quat[1] = buf->ReadF32();
    quat[2] = buf->ReadF32(); quat[3] = buf->ReadF32();
    lvel[0] = buf->ReadF32(); lvel[1] = buf->ReadF32(); lvel[2] = buf->ReadF32();
    avel[0] = buf->ReadF32(); avel[1] = buf->ReadF32(); avel[2] = buf->ReadF32();
    force[0] = buf->ReadF32(); force[1] = buf->ReadF32(); force[2] = buf->ReadF32();
    torque[0] = buf->ReadF32(); torque[1] = buf->ReadF32(); torque[2] = buf->ReadF32();

    dBodySetPosition(body, pos[0], pos[1], pos[2]);
    dBodySetQuaternion(body, quat);
    dBodySetLinearVel(body, lvel[0], lvel[1], lvel[2]);
    dBodySetAngularVel(body, avel[0], avel[1], avel[2]);
    dBodySetForce(body, force[0], force[1], force[2]);
    dBodySetTorque(body, torque[0], torque[1], torque[2]);

    u8 enabled = buf->ReadU8();
    if (enabled)
        dBodyEnable(body);
    else
        dBodyDisable(body);
}

/* ======================================================================== */
/*  Helper: save/restore animation pose tree nodes                           */
/* ======================================================================== */

static void SavePoseNode(SaveStateBuffer* buf, cPoseNode* node)
{
    if (!node)
    {
        buf->WriteU8(0);
        return;
    }
    buf->WriteU8(1);
    int type = node->GetType();
    buf->WriteS32(type);
    buf->WriteS32(node->m_numChildren);

    if (type == 0) // cPN_Blender
    {
        cPN_Blender* b = static_cast<cPN_Blender*>(node);
        buf->WriteF32(b->m_fBlendTime);
        buf->WriteF32(b->m_fBlendDuration);
    }
    else if (type == 1) // cPN_Feather
    {
        cPN_Feather* f = static_cast<cPN_Feather*>(node);
        buf->WriteF32(f->m_fBlendTime);
        buf->WriteF32(f->m_fBlendDuration);
        buf->WriteU32(static_cast<u32>(f->m_eFeatherBlendMode));
    }
    else if (type == 2) // cPN_SAnimController
    {
        cPN_SAnimController* ctrl = static_cast<cPN_SAnimController*>(node);
        buf->WriteF32(ctrl->m_fTime);
        buf->WriteF32(ctrl->m_fPrevTime);
        buf->WriteU8(*(const u8*)&ctrl->m_bMirror);
        buf->WriteF32(ctrl->m_fPlaybackSpeedScale);
        buf->WriteF32(ctrl->m_fSynchronizedWeight);
    }
    else if (type == 3) // cPN_SingleAxisBlender
    {
        cPN_SingleAxisBlender* sab = static_cast<cPN_SingleAxisBlender*>(node);
        buf->WriteF32(sab->m_fSmoothedWeight);
        buf->WriteF32(sab->m_fDesiredWeight);
        buf->WriteF32(sab->m_fWeightSeek);
    }

    for (int i = 0; i < node->m_numChildren && i < 3; i++)
    {
        SavePoseNode(buf, node->m_children[i]);
    }
}

/* Loading a pose tree.  The saved tree is read whole first, then applied only
   if the live tree has exactly the same shape (node types and child counts).
   The old loader built a new cPN_Blender with an empty child slot whenever the
   types differed and hung it into the live tree, so the next animation update
   crashed in cPN_Blender::Update; it also restored half-matching trees, which
   blended unrelated animations.  If the character changed animation since the
   save, its live tree is now left alone. */
struct PoseRec
{
    int type;
    int numChildren;
    float f[5];
    u32 u;
    u8 b;
};

static const int kMaxPoseRecs = 128;

static void ReadPoseRecs(SaveStateBuffer* buf, PoseRec* recs, int* count, bool* overflow)
{
    u8 hasNode = buf->ReadU8();
    PoseRec r;
    memset(&r, 0, sizeof(r));
    r.type = -1;
    if (!hasNode)
    {
        if (*count < kMaxPoseRecs) recs[*count] = r; else *overflow = true;
        (*count)++;
        return;
    }
    r.type = buf->ReadS32();
    r.numChildren = buf->ReadS32();
    if (r.type == 0)
    {
        r.f[0] = buf->ReadF32();
        r.f[1] = buf->ReadF32();
    }
    else if (r.type == 1)
    {
        r.f[0] = buf->ReadF32();
        r.f[1] = buf->ReadF32();
        r.u = buf->ReadU32();
    }
    else if (r.type == 2)
    {
        r.f[0] = buf->ReadF32();
        r.f[1] = buf->ReadF32();
        r.b = buf->ReadU8();
        r.f[2] = buf->ReadF32();
        r.f[3] = buf->ReadF32();
    }
    else if (r.type == 3)
    {
        r.f[0] = buf->ReadF32();
        r.f[1] = buf->ReadF32();
        r.f[2] = buf->ReadF32();
    }
    if (*count < kMaxPoseRecs) recs[*count] = r; else *overflow = true;
    (*count)++;
    int kids = r.numChildren < 3 ? r.numChildren : 3;
    for (int i = 0; i < kids; i++)
        ReadPoseRecs(buf, recs, count, overflow);
}

static bool PoseShapeMatches(cPoseNode* node, const PoseRec* recs, int count, int* idx)
{
    if (*idx >= count)
        return false;
    const PoseRec& r = recs[(*idx)++];
    if (r.type < 0)
        return node == nullptr;
    if (node == nullptr || node->GetType() != r.type || node->m_numChildren != r.numChildren)
        return false;
    int kids = r.numChildren < 3 ? r.numChildren : 3;
    for (int i = 0; i < kids; i++)
    {
        if (!PoseShapeMatches(node->m_children[i], recs, count, idx))
            return false;
    }
    return true;
}

static void ApplyPoseRecs(cPoseNode* node, const PoseRec* recs, int* idx)
{
    const PoseRec& r = recs[(*idx)++];
    if (r.type < 0 || node == nullptr)
        return;
    if (r.type == 0)
    {
        cPN_Blender* b = static_cast<cPN_Blender*>(node);
        b->m_fBlendTime = r.f[0];
        b->m_fBlendDuration = r.f[1];
    }
    else if (r.type == 1)
    {
        cPN_Feather* f = static_cast<cPN_Feather*>(node);
        f->m_fBlendTime = r.f[0];
        f->m_fBlendDuration = r.f[1];
        f->m_eFeatherBlendMode = static_cast<eFeatherBlendMode>(r.u);
    }
    else if (r.type == 2)
    {
        cPN_SAnimController* ctrl = static_cast<cPN_SAnimController*>(node);
        ctrl->m_fTime = r.f[0];
        ctrl->m_fPrevTime = r.f[1];
        *(u8*)&ctrl->m_bMirror = r.b;
        ctrl->m_fPlaybackSpeedScale = r.f[2];
        ctrl->m_fSynchronizedWeight = r.f[3];
    }
    else if (r.type == 3)
    {
        cPN_SingleAxisBlender* sab = static_cast<cPN_SingleAxisBlender*>(node);
        sab->m_fSmoothedWeight = r.f[0];
        sab->m_fDesiredWeight = r.f[1];
        sab->m_fWeightSeek = r.f[2];
    }
    int kids = r.numChildren < 3 ? r.numChildren : 3;
    for (int i = 0; i < kids; i++)
        ApplyPoseRecs(node->m_children[i], recs, idx);
}

/* Returns false when the live tree has another shape and was left alone. */
static bool LoadPoseNode(SaveStateBuffer* buf, cPoseNode** nodePtr)
{
    static PoseRec s_recs[kMaxPoseRecs];
    int count = 0;
    bool overflow = false;
    ReadPoseRecs(buf, s_recs, &count, &overflow);   /* always consumes the bytes */
    if (overflow || nodePtr == nullptr)
        return false;
    int idx = 0;
    if (!PoseShapeMatches(*nodePtr, s_recs, count, &idx) || idx != count)
        return false;
    idx = 0;
    ApplyPoseRecs(*nodePtr, s_recs, &idx);
    return true;
}

/* A character whose animation tree is a single animation (no cross-fade
   under way). */
static bool PoseTreeIsSingle(cCharacter* ch)
{
    if (!ch || !ch->m_pAILayer || !ch->m_pAILayer[0])
        return true;
    return ch->m_pAILayer[0]->GetType() == 2 && ch->m_pAILayer[0]->m_numChildren == 0;
}

/* Ends any cross-fade between animations at once: the character plays just
   its current animation, at the same point.  A fade's tree can't be rebuilt
   from a saved state on the other PC, so the host settles them before the
   state both games resume from is taken. */
static void FlattenPoseTree(cCharacter* ch)
{
    if (!ch || !ch->m_pAILayer || PoseTreeIsSingle(ch) || !ch->m_pCurrentAnimController)
        return;
    cPN_SAnimController* cur = ch->m_pCurrentAnimController;
    const f32 t = cur->m_fTime;
    const f32 pt = cur->m_fPrevTime;
    const u8 mirror = *(const u8*)&cur->m_bMirror;
    const f32 speed = cur->m_fPlaybackSpeedScale;
    ch->SetAnimState(ch->m_eAnimID, false, 0.0f, false, false);
    cPN_SAnimController* now = ch->m_pCurrentAnimController;
    if (now)
    {
        now->m_fTime = t;
        now->m_fPrevTime = pt;
        *(u8*)&now->m_bMirror = mirror;
        now->m_fPlaybackSpeedScale = speed;
    }
}

void NetplayFlattenAnimBlends()
{
    int n = 0;
    for (int i = 0; i < 10; i++)
    {
        cCharacter* ch = g_pCharacters[i];
        if (ch && !PoseTreeIsSingle(ch))
        {
            FlattenPoseTree(ch);
            n++;
        }
    }
    if (n)
        fprintf(stderr, "[netplay] finished %d animation cross-fade(s) before the shared state\n", n);
}

/* ======================================================================== */
/*  Helper: save/restore a cCharacter (common base fields)                   */
/* ======================================================================== */

static void SaveCharacterBase(SaveStateBuffer* buf, cCharacter* ch)
{
    /* Position and movement. */
    buf->WriteBytes(&ch->m_v3Position, sizeof(nlVector3));
    buf->WriteBytes(&ch->m_v3PrevPosition, sizeof(nlVector3));
    buf->WriteBytes(&ch->m_v3Velocity, sizeof(nlVector3));
    buf->WriteU32(static_cast<u32>(ch->m_eMovementState));
    buf->WriteU16(ch->m_aDesiredFacingDirection);
    buf->WriteU16(ch->m_aActualFacingDirection);
    buf->WriteU16(ch->m_aPrevFacingDirection);
    buf->WriteU16(ch->m_aDesiredMovementDirection);
    buf->WriteU16(ch->m_aActualMovementDirection);
    buf->WriteF32(ch->m_fAccel);
    buf->WriteF32(ch->m_fDecel);
    buf->WriteF32(ch->m_fDesiredSpeed);
    buf->WriteF32(ch->m_fActualSpeed);
    buf->WriteF32(ch->m_fLeanAmount);
    buf->WriteS32(ch->m_eAnimID);
    buf->WriteF32(ch->m_Dirt);

    /* Animation timing and adjustments. */
    buf->WriteF32(ch->m_fAnimAdjustBeginTime);
    buf->WriteF32(ch->m_fAnimAdjustEndTime);
    buf->WriteF32(ch->m_fDirectionSeekSpeed);
    buf->WriteF32(ch->m_fDirectionSeekFalloff);
    buf->WriteS16(ch->m_nAnimTurnAdjust);
    buf->WriteBytes(&ch->m_v3AnimMoveAdjust, sizeof(nlVector3));
    buf->WriteU8(*(const u8*)&ch->m_bFromAnimBlended);

    /* PORT: the whole animation tree, cross-fades included: the pose moves
       the character, so a tree left out made a game copied over from the
       other PC drift away again a few frames later. */
    {
        cPoseNode* aiRoot = ch->m_pAILayer ? *ch->m_pAILayer : nullptr;
        SavePoseNode(buf, aiRoot);
    }

    if (ch->m_pCurrentAnimController)
    {
        buf->WriteU8(1);
        buf->WriteF32(ch->m_pCurrentAnimController->m_fTime);
        buf->WriteF32(ch->m_pCurrentAnimController->m_fPrevTime);
        buf->WriteU8(*(const u8*)&ch->m_pCurrentAnimController->m_bMirror);
        buf->WriteF32(ch->m_pCurrentAnimController->m_fPlaybackSpeedScale);
    }
    else
    {
        buf->WriteU8(0);
    }

    /* ODE physics body. */
    if (ch->m_pPhysicsCharacter)
    {
        buf->WriteU8(1);
        /* The PhysicsCharacter wraps a PhysicsColumn which has a dBodyID.
           We access it through the base PhysicsObject. */
        PhysicsObject* physObj = reinterpret_cast<PhysicsObject*>(ch->m_pPhysicsCharacter);
        SaveODEBody(buf, physObj->m_bodyID);
        buf->WriteBytes(&physObj->m_position, sizeof(nlVector3));
        buf->WriteBytes(&physObj->m_linearVelocity, sizeof(nlVector3));
    }
    else
    {
        buf->WriteU8(0);
    }
}

static void LoadCharacterBase(SaveStateBuffer* buf, cCharacter* ch)
{
    buf->ReadBytes(&ch->m_v3Position, sizeof(nlVector3));
    buf->ReadBytes(&ch->m_v3PrevPosition, sizeof(nlVector3));
    buf->ReadBytes(&ch->m_v3Velocity, sizeof(nlVector3));
    ch->m_eMovementState = static_cast<eMovementState>(buf->ReadU32());
    ch->m_aDesiredFacingDirection = buf->ReadU16();
    ch->m_aActualFacingDirection = buf->ReadU16();
    ch->m_aPrevFacingDirection = buf->ReadU16();
    ch->m_aDesiredMovementDirection = buf->ReadU16();
    ch->m_aActualMovementDirection = buf->ReadU16();
    ch->m_fAccel = buf->ReadF32();
    ch->m_fDecel = buf->ReadF32();
    ch->m_fDesiredSpeed = buf->ReadF32();
    ch->m_fActualSpeed = buf->ReadF32();
    ch->m_fLeanAmount = buf->ReadF32();
    s32 savedAnimID = buf->ReadS32();
    ch->m_Dirt = buf->ReadF32();

    ch->m_fAnimAdjustBeginTime = buf->ReadF32();
    ch->m_fAnimAdjustEndTime = buf->ReadF32();
    ch->m_fDirectionSeekSpeed = buf->ReadF32();
    ch->m_fDirectionSeekFalloff = buf->ReadF32();
    ch->m_nAnimTurnAdjust = buf->ReadS16();
    buf->ReadBytes(&ch->m_v3AnimMoveAdjust, sizeof(nlVector3));
    *(u8*)&ch->m_bFromAnimBlended = buf->ReadU8();

    if (ch->m_eAnimID != savedAnimID)
    {
        ch->SetAnimState(savedAnimID, false, 0.0f, false, false);
    }

    {
        cPoseNode** aiRootPtr = ch->m_pAILayer ? &ch->m_pAILayer[0] : nullptr;
        if (!LoadPoseNode(buf, aiRootPtr) && aiRootPtr != nullptr)
        {
            /* Another shape here (mid cross-fade on one PC only): play just
               the saved animation; its timing is restored below. */
            ch->SetAnimState(savedAnimID, false, 0.0f, false, false);
        }
    }

    u8 hasAnimCtrl = buf->ReadU8();
    if (hasAnimCtrl)
    {
        f32 animTime = buf->ReadF32();
        f32 animPrevTime = buf->ReadF32();
        u8 mirror = buf->ReadU8();
        f32 playbackSpeed = buf->ReadF32();

        if (ch->m_pCurrentAnimController)
        {
            ch->m_pCurrentAnimController->m_fTime = animTime;
            ch->m_pCurrentAnimController->m_fPrevTime = animPrevTime;
            *(u8*)&ch->m_pCurrentAnimController->m_bMirror = mirror;
            ch->m_pCurrentAnimController->m_fPlaybackSpeedScale = playbackSpeed;
        }
    }
    else
    {
        ch->m_eAnimID = savedAnimID;
    }

    u8 hasPhys = buf->ReadU8();
    if (hasPhys && ch->m_pPhysicsCharacter)
    {
        PhysicsObject* physObj = reinterpret_cast<PhysicsObject*>(ch->m_pPhysicsCharacter);
        LoadODEBody(buf, physObj->m_bodyID);
        buf->ReadBytes(&physObj->m_position, sizeof(nlVector3));
        buf->ReadBytes(&physObj->m_linearVelocity, sizeof(nlVector3));
    }
}

/* ======================================================================== */
/*  Helper: convert cPlayer* pointer to index (0..9 or -1)                   */
/* ======================================================================== */

static s8 PlayerToIndex(cPlayer* p)
{
    if (!p) return -1;
    for (int i = 0; i < 10; i++)
    {
        if (reinterpret_cast<cPlayer*>(g_pCharacters[i]) == p)
            return static_cast<s8>(i);
    }
    return -1;
}

static cPlayer* IndexToPlayer(s8 idx)
{
    if (idx < 0 || idx >= 10) return nullptr;
    return reinterpret_cast<cPlayer*>(g_pCharacters[idx]);
}

/* ======================================================================== */
/*  Field Tracker for debugging desyncs                                      */
/* ======================================================================== */

struct TrackedField {
    const char* name;
    size_t offset;
};
static TrackedField s_trackedFields[256];
static int s_trackedFieldCount = 0;

static void TrackField(const char* name, size_t off)
{
    if (s_trackedFieldCount < 256)
    {
        s_trackedFields[s_trackedFieldCount].name = name;
        s_trackedFields[s_trackedFieldCount].offset = off;
        s_trackedFieldCount++;
    }
}

static const char* GetTrackedFieldAt(size_t off, size_t* outFieldStart)
{
    const char* bestName = "Unknown";
    size_t bestOff = 0;
    for (int i = 0; i < s_trackedFieldCount; i++)
    {
        if (s_trackedFields[i].offset <= off)
        {
            bestName = s_trackedFields[i].name;
            bestOff = s_trackedFields[i].offset;
        }
    }
    if (outFieldStart) *outFieldStart = bestOff;
    return bestName;
}

/* "offset name" for every field of the last SaveGameState, so a desync dump
   can say which field differs. */
void NetplayWriteStateFieldMap(FILE* f)
{
    if (!f) return;
    for (int i = 0; i < s_trackedFieldCount; i++)
        fprintf(f, "%zu %s\n", s_trackedFields[i].offset, s_trackedFields[i].name);
}


/* ---- Fielder desire working variables (v5) ---- */
/* bools are copied as raw bytes: one holding garbage (0x20) must come out
   the same on the other PC, not normalised to 1. */
static void SaveFielderDesireVars(SaveStateBuffer* buf, cFielder* f)
{
    buf->WriteBytes(&f->m_DesireCommonVars.tMiscTimer, sizeof(Timer));
    buf->WriteBytes(&f->m_DesireCommonVars.tAge, sizeof(Timer));
    buf->WriteF32(f->m_DesireCommonVars.fMisc);
    buf->WriteS8(PlayerToIndex(f->m_DesireCommonVars.pSBC));
    buf->WriteS8(PlayerToIndex(f->m_DesireCommonVars.pBallOwner));
    buf->WriteBytes(&f->m_DesireCommonVars.v3DesiredPosition, sizeof(nlVector3));
    buf->WriteU8(*(const u8*)&f->m_DesireCommonVars.bInPosition);
    buf->WriteU32(static_cast<u32>(f->m_DesireCommonVars.turboRequest));

    buf->WriteU8(*(const u8*)&f->m_DesireGetOpenVars.bDidWave);

    buf->WriteU16(f->m_DesireOneTimerVars.aDesiredFacingDirection);
    buf->WriteBytes(&f->m_DesireOneTimerVars.v3DesiredPosition, sizeof(nlVector3));
    buf->WriteF32(f->m_DesireOneTimerVars.fDesiredTime);
    buf->WriteBytes(&f->m_DesireOneTimerVars.v3BallPosition, sizeof(nlVector3));
    buf->WriteS32(f->m_DesireOneTimerVars.nOneTimerAnim);
    buf->WriteF32(f->m_DesireOneTimerVars.fOneTimerAnimTime);
    buf->WriteU8(*(const u8*)&f->m_DesireOneTimerVars.bIsChipShot);
    buf->WriteU8(*(const u8*)&f->m_DesireOneTimerVars.bVolleyPassReceive);

    buf->WriteBytes(&f->m_DesireReceivePassSharedVars.v3DesiredPosition, sizeof(nlVector3));
    buf->WriteU16(f->m_DesireReceivePassSharedVars.aDesiredFacingDirection);
    buf->WriteF32(f->m_DesireReceivePassSharedVars.fDesiredTime);
    buf->WriteBytes(&f->m_DesireReceivePassSharedVars.v3BallPosition, sizeof(nlVector3));
    buf->WriteS32(f->m_DesireReceivePassSharedVars.nReceivePassAnim);
    buf->WriteF32(f->m_DesireReceivePassSharedVars.fReceivePassAnimTime);
    buf->WriteS32(f->m_DesireReceivePassSharedVars.iAttemptOneTouchShot);
    buf->WriteU8(*(const u8*)&f->m_DesireReceivePassSharedVars.bFailedToInitOneTouchShot);
    buf->WriteU16(static_cast<u16>(f->m_DesireReceivePassSharedVars.iAttemptOneTouchPass));
    buf->WriteU8(*(const u8*)&f->m_DesireReceivePassSharedVars.bVolleyPassReceive);
    buf->WriteS8(PlayerToIndex(f->m_DesireReceivePassSharedVars.pOneTouchPassTarget));

    buf->WriteS8(PlayerToIndex(f->m_DesireSlideAttackVars.m_pSlideAttackTarget));
    buf->WriteU8(*(const u8*)&f->m_DesireWindupForShotVars.bIsBallAwayFromCarrier);

    buf->WriteBytes(&f->mtKickOffWaitTimer, sizeof(Timer));
    buf->WriteBytes(&f->m_tPowerupEffectTime, sizeof(Timer));
    buf->WriteS8(PlayerToIndex(f->m_pPowerupTarget));
    buf->WriteS32(f->m_nPowerupAnimID);
    buf->WriteBytes(&f->mtBombImpactTime, sizeof(Timer));
    buf->WriteBytes(&f->mv3BombImpactLocation, sizeof(nlVector3));
    buf->WriteF32(f->mfBombImpactRadius);
    buf->WriteU32(static_cast<u32>(f->m_ePenaltyCardStatus));
    buf->WriteF32(f->m_fDistanceToDesiredPosition);
    buf->WriteBytes(&f->m_v3AccumDesiredPos, sizeof(nlVector3));
    buf->WriteF32(f->m_fAccumDesiredPosWeight);
    buf->WriteU32(static_cast<u32>(f->meS2SResult));
    buf->WriteU8(*(const u8*)&f->mbCanKickoff);
    buf->WriteU8(*(const u8*)&f->mbWasHitByPowerupThisFrame);
    buf->WriteU32(static_cast<u32>(f->mThoughtHashCalcDesire));
    buf->WriteU32(static_cast<u32>(f->mThoughtHashInitRunToNet));
    buf->WriteU32(static_cast<u32>(f->mThoughtHashInitGetOpen));
    buf->WriteU32(static_cast<u32>(f->mThoughtHashInitWindupPass));
    buf->WriteU32(static_cast<u32>(f->mThoughtHashInitCutAndBreak));
}

static void LoadFielderDesireVars(SaveStateBuffer* buf, cFielder* f)
{
    buf->ReadBytes(&f->m_DesireCommonVars.tMiscTimer, sizeof(Timer));
    buf->ReadBytes(&f->m_DesireCommonVars.tAge, sizeof(Timer));
    f->m_DesireCommonVars.fMisc = buf->ReadF32();
    f->m_DesireCommonVars.pSBC = IndexToPlayer(buf->ReadS8());
    f->m_DesireCommonVars.pBallOwner = IndexToPlayer(buf->ReadS8());
    buf->ReadBytes(&f->m_DesireCommonVars.v3DesiredPosition, sizeof(nlVector3));
    *(u8*)&f->m_DesireCommonVars.bInPosition = buf->ReadU8();
    f->m_DesireCommonVars.turboRequest = static_cast<eTurboRequest>(buf->ReadU32());

    *(u8*)&f->m_DesireGetOpenVars.bDidWave = buf->ReadU8();

    f->m_DesireOneTimerVars.aDesiredFacingDirection = buf->ReadU16();
    buf->ReadBytes(&f->m_DesireOneTimerVars.v3DesiredPosition, sizeof(nlVector3));
    f->m_DesireOneTimerVars.fDesiredTime = buf->ReadF32();
    buf->ReadBytes(&f->m_DesireOneTimerVars.v3BallPosition, sizeof(nlVector3));
    f->m_DesireOneTimerVars.nOneTimerAnim = buf->ReadS32();
    f->m_DesireOneTimerVars.fOneTimerAnimTime = buf->ReadF32();
    *(u8*)&f->m_DesireOneTimerVars.bIsChipShot = buf->ReadU8();
    *(u8*)&f->m_DesireOneTimerVars.bVolleyPassReceive = buf->ReadU8();

    buf->ReadBytes(&f->m_DesireReceivePassSharedVars.v3DesiredPosition, sizeof(nlVector3));
    f->m_DesireReceivePassSharedVars.aDesiredFacingDirection = buf->ReadU16();
    f->m_DesireReceivePassSharedVars.fDesiredTime = buf->ReadF32();
    buf->ReadBytes(&f->m_DesireReceivePassSharedVars.v3BallPosition, sizeof(nlVector3));
    f->m_DesireReceivePassSharedVars.nReceivePassAnim = buf->ReadS32();
    f->m_DesireReceivePassSharedVars.fReceivePassAnimTime = buf->ReadF32();
    f->m_DesireReceivePassSharedVars.iAttemptOneTouchShot = buf->ReadS32();
    *(u8*)&f->m_DesireReceivePassSharedVars.bFailedToInitOneTouchShot = buf->ReadU8();
    f->m_DesireReceivePassSharedVars.iAttemptOneTouchPass = static_cast<signed short>(buf->ReadU16());
    *(u8*)&f->m_DesireReceivePassSharedVars.bVolleyPassReceive = buf->ReadU8();
    f->m_DesireReceivePassSharedVars.pOneTouchPassTarget = IndexToPlayer(buf->ReadS8());

    f->m_DesireSlideAttackVars.m_pSlideAttackTarget = reinterpret_cast<cFielder*>(IndexToPlayer(buf->ReadS8()));
    *(u8*)&f->m_DesireWindupForShotVars.bIsBallAwayFromCarrier = buf->ReadU8();

    buf->ReadBytes(&f->mtKickOffWaitTimer, sizeof(Timer));
    buf->ReadBytes(&f->m_tPowerupEffectTime, sizeof(Timer));
    f->m_pPowerupTarget = reinterpret_cast<cFielder*>(IndexToPlayer(buf->ReadS8()));
    f->m_nPowerupAnimID = buf->ReadS32();
    buf->ReadBytes(&f->mtBombImpactTime, sizeof(Timer));
    buf->ReadBytes(&f->mv3BombImpactLocation, sizeof(nlVector3));
    f->mfBombImpactRadius = buf->ReadF32();
    f->m_ePenaltyCardStatus = static_cast<ePenaltyCardStatus>(buf->ReadU32());
    f->m_fDistanceToDesiredPosition = buf->ReadF32();
    buf->ReadBytes(&f->m_v3AccumDesiredPos, sizeof(nlVector3));
    f->m_fAccumDesiredPosWeight = buf->ReadF32();
    f->meS2SResult = static_cast<eShootToScoreResult>(buf->ReadU32());
    *(u8*)&f->mbCanKickoff = buf->ReadU8();
    *(u8*)&f->mbWasHitByPowerupThisFrame = buf->ReadU8();
    f->mThoughtHashCalcDesire = buf->ReadU32();
    f->mThoughtHashInitRunToNet = buf->ReadU32();
    f->mThoughtHashInitGetOpen = buf->ReadU32();
    f->mThoughtHashInitWindupPass = buf->ReadU32();
    f->mThoughtHashInitCutAndBreak = buf->ReadU32();
}


/* ---- Running clocks, matched by identity (v6) ----
   The two PCs started their clocks in different orders, so restoring "the
   n-th running clock" gave one player's think timer to another player.  Each
   clock is now identified by its callback and its parameters (a player, the
   game, the ball, a team or a small number), and the running list is rebuilt
   in the host's order: clocks the host has running are started, clocks it
   does not are stopped. */
static s32 ClockCallbackKey(const Clock* c)
{
    if (c->m_callback == NULL)
        return 0;
    return static_cast<s32>(reinterpret_cast<intptr_t>(reinterpret_cast<void*>(c->m_callback))
                            - reinterpret_cast<intptr_t>(reinterpret_cast<void*>(&ClockManager::Update)));
}

static u32 ClockParamKey(uintptr_t p)
{
    if (p == 0)
        return 0;
    for (int i = 0; i < 10; i++)
        if (p == reinterpret_cast<uintptr_t>(g_pCharacters[i]))
            return 0x80000100u + i;
    if (p == reinterpret_cast<uintptr_t>(g_pGame))
        return 0x80000200u;
    if (p == reinterpret_cast<uintptr_t>(g_pBall))
        return 0x80000201u;
    for (int t = 0; t < 2; t++)
        if (p == reinterpret_cast<uintptr_t>(g_pTeams[t]))
            return 0x80000210u + t;
    if (p < 0x10000)
        return static_cast<u32>(p);
    return 0xFFFFFFFFu;   /* some other object: not comparable between PCs */
}

static int CollectRing(Clock* head, Clock** out, int at, int max)
{
    Clock* c = head;
    while (c != NULL && at < max)
    {
        out[at++] = c;
        c = c->m_next;
        if (c == head)
            break;
    }
    return at;
}

static void LoadRunningClocks(SaveStateBuffer* buf)
{
    struct Entry { s32 cb; u32 p1, p2; f32 scale, timer, end; u32 act, state; };
    static Entry entries[256];
    u16 count = buf->ReadU16();
    for (u16 i = 0; i < count; i++)
    {
        Entry e;
        e.cb    = buf->ReadS32();
        e.p1    = buf->ReadU32();
        e.p2    = buf->ReadU32();
        e.scale = buf->ReadF32();
        e.timer = buf->ReadF32();
        e.end   = buf->ReadF32();
        e.act   = buf->ReadU32();
        e.state = buf->ReadU32();
        if (i < 256)
            entries[i] = e;
    }
    if (count > 256)
        count = 256;

    /* Every clock this game has: running ones first, so they win a tie. */
    static Clock* pool[2048];
    static u8     poolRing[2048];   /* 0 active, 1 pending, 2 inactive */
    static bool   used[2048];
    int n = 0, a;
    a = CollectRing(ClockManager::m_activeList, pool, n, 2048);        for (; n < a; n++) poolRing[n] = 0;
    a = CollectRing(ClockManager::m_pendingActiveList, pool, n, 2048); for (; n < a; n++) poolRing[n] = 1;
    a = CollectRing(ClockManager::m_inactiveList, pool, n, 2048);      for (; n < a; n++) poolRing[n] = 2;
    memset(used, 0, sizeof(bool) * n);

    static Clock* order[256];
    int matched = 0, missing = 0;
    for (u16 i = 0; i < count; i++)
    {
        const Entry& e = entries[i];
        Clock* found = NULL;
        for (int k = 0; k < n; k++)
        {
            if (used[k])
                continue;
            Clock* c = pool[k];
            if (ClockCallbackKey(c) == e.cb && ClockParamKey(c->m_uParam1) == e.p1
                && ClockParamKey(c->m_uParam2) == e.p2)
            {
                used[k] = true;
                found = c;
                /* Take it off whatever list it is on. */
                Clock** ring = poolRing[k] == 0 ? &ClockManager::m_activeList
                             : poolRing[k] == 1 ? &ClockManager::m_pendingActiveList
                                                : &ClockManager::m_inactiveList;
                nlDLRingRemove<Clock>(ring, c);
                break;
            }
        }
        order[i] = found;
        if (found)
        {
            found->m_fTimeScale    = e.scale;
            found->m_fTimer        = e.timer;
            found->m_fEndTime      = e.end;
            found->m_uActiveStates = e.act;
            found->m_clockState    = static_cast<eClockState>(e.state);
            matched++;
        }
        else
        {
            missing++;
        }
    }

    /* Whatever still runs here but not on the host stops.  (A clock the host
       has finished keeps the DONE state LoadClock gave it.) */
    int stopped = 0;
    while (ClockManager::m_activeList != NULL)
    {
        Clock* c = ClockManager::m_activeList;
        nlDLRingRemove<Clock>(&ClockManager::m_activeList, c);
        if (c->m_clockState == CLOCK_ON || c->m_clockState == CLOCK_PAUSED)
            c->m_clockState = CLOCK_OFF;
        nlDLRingAddEnd<Clock>(&ClockManager::m_inactiveList, c);
        stopped++;
    }
    while (ClockManager::m_pendingActiveList != NULL)
    {
        Clock* c = ClockManager::m_pendingActiveList;
        nlDLRingRemove<Clock>(&ClockManager::m_pendingActiveList, c);
        if (c->m_clockState == CLOCK_ON || c->m_clockState == CLOCK_PAUSED)
            c->m_clockState = CLOCK_OFF;
        nlDLRingAddEnd<Clock>(&ClockManager::m_inactiveList, c);
        stopped++;
    }

    /* Rebuild the running list in the host's order.  The list pointer is the
       last clock and the walk starts at the one after it, so the first saved
       clock is added last. */
    for (u16 i = 1; i < count; i++)
        if (order[i])
            nlDLRingAddEnd<Clock>(&ClockManager::m_activeList, order[i]);
    if (count > 0 && order[0])
        nlDLRingAddEnd<Clock>(&ClockManager::m_activeList, order[0]);

    if (missing || stopped)
        fprintf(stderr, "[netplay] clocks: %d restored, %d of the host's not found here, %d stopped\n",
                matched, missing, stopped);
}

/* ---- Raw object dump for finding state the save does not cover ----
   Written by the host when it sends a state and by the guest right after
   loading it; diffing the two shows every byte that still differs. */
extern "C" size_t PortReadableBytes(const void* p, size_t n);

static void DumpRecord(FILE* f, const char* name, const void* p, u32 size)
{
    char tag[48];
    memset(tag, 0, sizeof(tag));
    snprintf(tag, sizeof(tag), "%s", name);
    fwrite(tag, 1, sizeof(tag), f);
    u32 sz = p ? size : 0;
    fwrite(&sz, 4, 1, f);
    if (sz)
        fwrite(p, 1, sz, f);
}

/* The object itself, then 384 bytes at every pointer stored in it (the
   animation, AI, physics and controller objects hanging off it). */
static void DumpObjDeep(FILE* f, const char* name, const void* p, u32 size)
{
    DumpRecord(f, name, p, size);
    if (p == NULL)
        return;
    const u8* base = static_cast<const u8*>(p);
    for (u32 off = 0; off + sizeof(void*) <= size; off += sizeof(void*))
    {
        uintptr_t v;
        memcpy(&v, base + off, sizeof(v));
        char sub[48];
        snprintf(sub, sizeof(sub), "%s+0x%x", name, off);
        const void* q = reinterpret_cast<const void*>(v);
        size_t n = 0;
        if (v >= 0x10000 && (v & 7) == 0 && v < (static_cast<uintptr_t>(1) << 47) && q != p)
            n = PortReadableBytes(q, 384);
        DumpRecord(f, sub, n ? q : NULL, static_cast<u32>(n));
    }
}

void NetplayWriteSyncDump(const char* binPath, const char* txtPath)
{
    FILE* f = fopen(binPath, "wb");
    if (f)
    {
        DumpObjDeep(f, "cGame", g_pGame, sizeof(cGame));
        DumpObjDeep(f, "cBall", g_pBall, sizeof(cBall));
        DumpObjDeep(f, "cTeam0", g_pTeams[0], sizeof(cTeam));
        DumpObjDeep(f, "cTeam1", g_pTeams[1], sizeof(cTeam));
        for (int i = 0; i < 10; i++)
        {
            char name[32];
            snprintf(name, sizeof(name), i < 8 ? "Fielder%d" : "Goalie%d", i);
            DumpObjDeep(f, name, g_pCharacters[i], i < 8 ? sizeof(cFielder) : sizeof(Goalie));
        }
        fclose(f);
    }
    f = fopen(txtPath, "w");
    if (f)
    {
        fprintf(f, "# objects: cGame %p ball %p team0 %p team1 %p\n",
                (void*)g_pGame, (void*)g_pBall, (void*)g_pTeams[0], (void*)g_pTeams[1]);
        for (int i = 0; i < 10; i++)
            fprintf(f, "# char %d %p\n", i, (void*)g_pCharacters[i]);
        static Clock* list[2048];
        const char* names[3] = { "running", "pending", "stopped" };
        Clock* heads[3] = { ClockManager::m_activeList, ClockManager::m_pendingActiveList, ClockManager::m_inactiveList };
        for (int r = 0; r < 3; r++)
        {
            int n = CollectRing(heads[r], list, 0, 2048);
            fprintf(f, "# %s clocks: %d\n", names[r], n);
            for (int k = 0; k < n; k++)
            {
                Clock* c = list[k];
                fprintf(f, "%s cb=%d p1=%08X p2=%08X scale=%g timer=%g end=%g act=%lX state=%d\n",
                        names[r], ClockCallbackKey(c), ClockParamKey(c->m_uParam1), ClockParamKey(c->m_uParam2),
                        c->m_fTimeScale, c->m_fTimer, c->m_fEndTime, (unsigned long)c->m_uActiveStates,
                        (int)c->m_clockState);
            }
        }
        fclose(f);
    }
}


/* ---- Goal nets (v9) ----
   The nets are cloth simulated in the game frames, and push the ball when it
   is in them.  The goal replay moves them too, for as long as each PC shows
   it, so after a skipped replay the two games had different nets and one of
   them pushed the ball at the next kickoff. */
static void SaveNetMesh(SaveStateBuffer* buf, const NetMesh* m)
{
    if (m == NULL || !m->mbInitialized || m->m_v3Position == NULL || m->m_NumParticles <= 0)
    {
        buf->WriteS32(-1);
        return;
    }
    const int n = m->m_NumParticles;
    buf->WriteS32(n);
    buf->WriteBytes(m->m_v3Position, sizeof(nlVector3) * n);
    buf->WriteBytes(m->m_v3PrevPosition, sizeof(nlVector3) * n);
    if (m->m_v3Accel)
        buf->WriteBytes(m->m_v3Accel, sizeof(nlVector3) * n);
    else
        for (int i = 0; i < n * 3; i++)
            buf->WriteF32(0.0f);
    buf->WriteS32(m->m_numAffectedParticles);
    buf->WriteS32(m->m_iClosestParticle);
    buf->WriteU8(*(const u8*)&m->mbFirstUpdate);
    buf->WriteF32(m->m_fBallPenetrationDepth);
    buf->WriteU8(m->m_bPenetratingFixedParticle);
    buf->WriteBytes(&m->m_v3BallPenetrationNormal, sizeof(nlVector3));
    buf->WriteU8(*(const u8*)&m->mbIsActive);
    buf->WriteU8(*(const u8*)&m->mbBallIsInsideNet);
    buf->WriteF32(m->mfMotion);
    buf->WriteF32(m->mJolt);
}

static void LoadNetMesh(SaveStateBuffer* buf, NetMesh* m)
{
    const int n = buf->ReadS32();
    if (n < 0)
        return;
    const bool apply = m != NULL && m->mbInitialized && m->m_v3Position != NULL && m->m_NumParticles == n;
    if (!apply)
    {
        /* Not the same net here: skip it. */
        static nlVector3 s_skip[256];
        for (int part = 0; part < 3; part++)
            for (int left = n; left > 0; left -= 256)
                buf->ReadBytes(s_skip, sizeof(nlVector3) * (left < 256 ? left : 256));
        u8 rest[4 + 4 + 1 + 4 + 1 + 12 + 1 + 1 + 4 + 4];
        buf->ReadBytes(rest, sizeof(rest));
        return;
    }
    buf->ReadBytes(m->m_v3Position, sizeof(nlVector3) * n);
    buf->ReadBytes(m->m_v3PrevPosition, sizeof(nlVector3) * n);
    if (m->m_v3Accel)
        buf->ReadBytes(m->m_v3Accel, sizeof(nlVector3) * n);
    else
        for (int i = 0; i < n * 3; i++)
            buf->ReadF32();
    m->m_numAffectedParticles = buf->ReadS32();
    m->m_iClosestParticle = buf->ReadS32();
    *(u8*)&m->mbFirstUpdate = buf->ReadU8();
    m->m_fBallPenetrationDepth = buf->ReadF32();
    m->m_bPenetratingFixedParticle = buf->ReadU8();
    buf->ReadBytes(&m->m_v3BallPenetrationNormal, sizeof(nlVector3));
    *(u8*)&m->mbIsActive = buf->ReadU8();
    *(u8*)&m->mbBallIsInsideNet = buf->ReadU8();
    m->mfMotion = buf->ReadF32();
    m->mJolt = buf->ReadF32();
}

/* ---- Ball physics flags (v9) ---- */
static void SaveBallPhysicsExtra(SaveStateBuffer* buf)
{
    PhysicsAIBall* b = (g_pBall != NULL) ? g_pBall->m_pPhysicsBall : NULL;
    buf->WriteU8(b != NULL ? 1 : 0);
    if (b == NULL)
        return;
    buf->WriteBytes(&b->m_v3TiltForce, sizeof(nlVector3));
    buf->WriteU8(*(const u8*)&b->m_bUseTiltForce);
    buf->WriteU8(*(const u8*)&b->m_bIsSupportedByGround);
    buf->WriteU8(*(const u8*)&b->m_bUseAngularVel);
    buf->WriteU8(*(const u8*)&b->m_bUseMagnusEffect);
    buf->WriteF32(b->m_fSpinTimer);
    buf->WriteBytes(&b->m_v3PrevPosition, sizeof(nlVector3));
    buf->WriteU32(b->m_goalieContactFramesAgo);
    buf->WriteU32(b->m_goalPostContactFramesAgo);
    buf->WriteU8(*(const u8*)&b->mbIsInsideNet);
    buf->WriteU8(*(const u8*)&b->mbGoalPlaneContact);
    buf->WriteU8(*(const u8*)&b->mbBallSpeedBelowSweepTestThreshold);
}

static void LoadBallPhysicsExtra(SaveStateBuffer* buf)
{
    if (buf->ReadU8() == 0)
        return;
    nlVector3 tilt, prev;
    buf->ReadBytes(&tilt, sizeof(tilt));
    u8 useTilt = buf->ReadU8(), supported = buf->ReadU8(), useAng = buf->ReadU8(), magnus = buf->ReadU8();
    f32 spin = buf->ReadF32();
    buf->ReadBytes(&prev, sizeof(prev));
    u32 goalie = buf->ReadU32(), post = buf->ReadU32();
    u8 inside = buf->ReadU8(), plane = buf->ReadU8(), slow = buf->ReadU8();
    PhysicsAIBall* b = (g_pBall != NULL) ? g_pBall->m_pPhysicsBall : NULL;
    if (b == NULL)
        return;
    b->m_v3TiltForce = tilt;
    *(u8*)&b->m_bUseTiltForce = useTilt;
    *(u8*)&b->m_bIsSupportedByGround = supported;
    *(u8*)&b->m_bUseAngularVel = useAng;
    *(u8*)&b->m_bUseMagnusEffect = magnus;
    b->m_fSpinTimer = spin;
    b->m_v3PrevPosition = prev;
    b->m_goalieContactFramesAgo = goalie;
    b->m_goalPostContactFramesAgo = post;
    *(u8*)&b->mbIsInsideNet = inside;
    *(u8*)&b->mbGoalPlaneContact = plane;
    *(u8*)&b->mbBallSpeedBelowSweepTestThreshold = slow;
}


/* ---- Team formations (v10) ----
   Each team re-chooses its formation every ~10 s of game time and re-sorts
   who stands where every ~0.5 s, with timers and choices that were not in the
   save state: a few seconds after every load the two PCs re-chose at
   different moments, and one AI player got a different job. */
static void SaveEvalData(SaveStateBuffer* buf, const FormationEval* e)
{
    buf->WriteBytes(&e->m_SortTimer, sizeof(Timer));
    for (int i = 0; i < 4; i++)
        buf->WriteU32(e->m_iFielderFormationPos[i]);
    buf->WriteS8(PlayerToIndex(e->m_pKeyPlayer));
}

static void LoadEvalData(SaveStateBuffer* buf, FormationEval* e)
{
    Timer t;
    buf->ReadBytes(&t, sizeof(Timer));
    u32 pos[4];
    for (int i = 0; i < 4; i++)
        pos[i] = buf->ReadU32();
    s8 key = buf->ReadS8();
    if (e == NULL)
        return;
    e->m_SortTimer = t;
    for (int i = 0; i < 4; i++)
        e->m_iFielderFormationPos[i] = pos[i];
    e->m_pKeyPlayer = IndexToPlayer(key);
}

static void SaveFormations(SaveStateBuffer* buf, const cTeam* team)
{
    const FormationManager* fm = team ? team->m_pFormationManager : NULL;
    buf->WriteU8(fm ? 1 : 0);
    if (!fm)
        return;
    buf->WriteBytes(&fm->m_tSelectFormationsTimer, sizeof(Timer));
    buf->WriteBytes(&fm->m_v2AIFielderCenter, sizeof(nlVector2));
    for (int i = 0; i < 4; i++)
    {
        buf->WriteU8(*(const u8*)&fm->m_CachedPositions[i].bCacheIsValid);
        buf->WriteU8(*(const u8*)&fm->m_CachedPositions[i].bInPosition);
        buf->WriteBytes(&fm->m_CachedPositions[i].vPosition, sizeof(nlVector3));
    }
    for (int t = 0; t < 3; t++)
    {
        const FormationEval* e = fm->m_pFormations[t];
        if (e == NULL)
        {
            buf->WriteS32(-2);
            continue;
        }
        buf->WriteS32(e->m_pFormationSpec ? e->m_pFormationSpec->m_ID : -1);
        if (t == FTYPE_BALLPOSITION)
        {
            const FormationBallPosition* b = static_cast<const FormationBallPosition*>(e);
            buf->WriteS32(b->m_pFormationSet ? b->m_pFormationSet->m_ID : -1);
        }
        SaveEvalData(buf, e);
        if (t == FTYPE_BALLPOSITION)
        {
            const FormationBallPosition* nx = static_cast<const FormationBallPosition*>(e)->m_pNextClosestFormation;
            if (nx == NULL)
                buf->WriteS32(-2);
            else
            {
                buf->WriteS32(nx->m_pFormationSpec ? nx->m_pFormationSpec->m_ID : -1);
                SaveEvalData(buf, nx);
            }
        }
    }
}

static void LoadFormations(SaveStateBuffer* buf, cTeam* team)
{
    if (buf->ReadU8() == 0)
        return;
    FormationManager* fm = team ? team->m_pFormationManager : NULL;
    Timer sel;
    nlVector2 center;
    buf->ReadBytes(&sel, sizeof(Timer));
    buf->ReadBytes(&center, sizeof(nlVector2));
    CachedPosition cache[4];
    for (int i = 0; i < 4; i++)
    {
        *(u8*)&cache[i].bCacheIsValid = buf->ReadU8();
        *(u8*)&cache[i].bInPosition = buf->ReadU8();
        buf->ReadBytes(&cache[i].vPosition, sizeof(nlVector3));
    }
    if (fm)
    {
        fm->m_tSelectFormationsTimer = sel;
        fm->m_v2AIFielderCenter = center;
        for (int i = 0; i < 4; i++)
            fm->m_CachedPositions[i] = cache[i];
    }
    for (int t = 0; t < 3; t++)
    {
        const s32 specId = buf->ReadS32();
        if (specId == -2)
        {
            if (fm && fm->m_pFormations[t] != NULL)
            {
                if (t == FTYPE_BALLPOSITION)
                    fm->SetNewFormationEval((eFormationType)t, FSET_NONE);
                else
                    fm->SetNewFormationEval((eFormationType)t, FORMATION_NONE);
            }
            continue;
        }
        s32 setId = -1;
        if (t == FTYPE_BALLPOSITION)
            setId = buf->ReadS32();
        FormationEval* e = NULL;
        if (fm)
        {
            if (t == FTYPE_BALLPOSITION)
            {
                FormationBallPosition* b = static_cast<FormationBallPosition*>(fm->m_pFormations[t]);
                if (setId >= 0 && (b == NULL || b->m_pFormationSet == NULL || b->m_pFormationSet->m_ID != setId))
                    fm->SetNewFormationEval((eFormationType)t, (eFormationSet)setId);
                e = fm->m_pFormations[t];
                if (e)
                    e->m_pFormationSpec = specId >= 0 ? FormationManager::GetFormationSpec((eFormation)specId) : NULL;
            }
            else
            {
                FormationEval* cur = fm->m_pFormations[t];
                if (specId >= 0 && (cur == NULL || cur->m_pFormationSpec == NULL || cur->m_pFormationSpec->m_ID != specId))
                    fm->SetNewFormationEval((eFormationType)t, (eFormation)specId);
                e = fm->m_pFormations[t];
            }
        }
        LoadEvalData(buf, e);
        if (t == FTYPE_BALLPOSITION)
        {
            const s32 nextId = buf->ReadS32();
            FormationBallPosition* b = static_cast<FormationBallPosition*>(e);
            if (nextId == -2)
            {
                if (b && b->m_pNextClosestFormation)
                {
                    delete b->m_pNextClosestFormation;
                    b->m_pNextClosestFormation = NULL;
                }
                continue;
            }
            const FormationSpec* nextSpec = nextId >= 0 ? FormationManager::GetFormationSpec((eFormation)nextId) : NULL;
            if (b && b->m_pNextClosestFormation == NULL)
                b->m_pNextClosestFormation = new (nlMalloc(sizeof(FormationBallPosition), 8, false))
                    FormationBallPosition(fm, FTYPE_BALLPOSITION, nextSpec);
            if (b)
                b->m_pNextClosestFormation->m_pFormationSpec = nextSpec;
            LoadEvalData(buf, b ? b->m_pNextClosestFormation : NULL);
        }
    }
}

bool NetplayIsGoalCelebration()
{
    return g_pGame != NULL && g_pGame->m_eGameState == GS_POST_GOAL;
}

const char* NetplayStateFieldAt(size_t off, size_t* fieldStart)
{
    return GetTrackedFieldAt(off, fieldStart);
}

/* ======================================================================== */
/*  Bowser and the Chain Chomp                                               */
/* ======================================================================== */

/* PORT: the stadium's two monsters act on the players and the ball, so a game copied over from the
   other PC (a desync repair, or the state both resume from) must have them where that PC had them. */
static NPCManager* CurrentNPCs()
{
    BasicStadium* stadium = BasicStadium::GetCurrentStadium();
    return stadium != nullptr ? stadium->mpNPCManager : nullptr;
}

static void SaveMovableNPC(SaveStateBuffer* buf, SkinAnimatedMovableNPC* npc)
{
    buf->WriteBytes(&npc->mWorldMatrix, sizeof(npc->mWorldMatrix));
    buf->WriteU8(npc->mbIsVisible ? 1 : 0);
    SavePoseNode(buf, npc->mpPoseTree);
    buf->WriteBytes(&npc->mv3Position, sizeof(nlVector3));
    buf->WriteBytes(&npc->mv3Velocity, sizeof(nlVector3));
    buf->WriteU16(npc->maFacingDirection);
    buf->WriteU16(npc->maDesiredFacingDirection);
    buf->WriteF32(npc->mfDesiredSpeed);
    PhysicsObject* phys = reinterpret_cast<PhysicsObject*>(npc->mpPhysObj);
    buf->WriteU8(phys != nullptr ? 1 : 0);
    if (phys != nullptr)
    {
        SaveODEBody(buf, phys->m_bodyID);
        buf->WriteBytes(&phys->m_position, sizeof(nlVector3));
        buf->WriteBytes(&phys->m_linearVelocity, sizeof(nlVector3));
    }
}

static void LoadMovableNPC(SaveStateBuffer* buf, SkinAnimatedMovableNPC* npc)
{
    buf->ReadBytes(&npc->mWorldMatrix, sizeof(npc->mWorldMatrix));
    npc->mbIsVisible = buf->ReadU8() != 0;
    LoadPoseNode(buf, &npc->mpPoseTree);   /* applied when the shapes match */
    buf->ReadBytes(&npc->mv3Position, sizeof(nlVector3));
    buf->ReadBytes(&npc->mv3Velocity, sizeof(nlVector3));
    npc->maFacingDirection = buf->ReadU16();
    npc->maDesiredFacingDirection = buf->ReadU16();
    npc->mfDesiredSpeed = buf->ReadF32();
    PhysicsObject* phys = reinterpret_cast<PhysicsObject*>(npc->mpPhysObj);
    if (buf->ReadU8() != 0)
    {
        if (phys != nullptr)
        {
            LoadODEBody(buf, phys->m_bodyID);
            buf->ReadBytes(&phys->m_position, sizeof(nlVector3));
            buf->ReadBytes(&phys->m_linearVelocity, sizeof(nlVector3));
        }
        else
        {
            /* Nothing to put it in: read past it. */
            LoadODEBody(buf, nullptr);
            nlVector3 tmp;
            buf->ReadBytes(&tmp, sizeof tmp);
            buf->ReadBytes(&tmp, sizeof tmp);
        }
    }
}

static void SaveNPCs(SaveStateBuffer* buf)
{
    NPCManager* npcs = CurrentNPCs();
    Bowser* bowser = npcs != nullptr ? npcs->mpBowser : nullptr;
    ChainChomp* chomp = npcs != nullptr ? npcs->mpChainChomp : nullptr;
    buf->WriteF32(Bowser::mfYAxisTilt);

    buf->WriteU8(bowser != nullptr ? 1 : 0);
    if (bowser != nullptr)
    {
        SaveMovableNPC(buf, bowser);
        buf->WriteS8(PlayerToIndex(reinterpret_cast<cPlayer*>(bowser->mpTarget)));
        buf->WriteBytes(&bowser->mv3TargetPos, sizeof(nlVector3));
        buf->WriteS32(static_cast<s32>(bowser->meBowserState));
        buf->WriteBytes(&bowser->mtStateTimer, sizeof(Timer));
        buf->WriteBytes(&bowser->mtActiveTimer, sizeof(Timer));
        buf->WriteS32(bowser->mAnimID);
        buf->WriteU8(bowser->mbDoTilt ? 1 : 0);
        buf->WriteS32(static_cast<s32>(bowser->mAttackType));
        buf->WriteS32(bowser->mStompStage);
        buf->WriteU8(bowser->mbFirstTime ? 1 : 0);
        buf->WriteU8(bowser->mbAlive ? 1 : 0);
        buf->WriteU8(bowser->mbResetPending ? 1 : 0);
    }

    buf->WriteU8(chomp != nullptr ? 1 : 0);
    if (chomp != nullptr)
    {
        SaveMovableNPC(buf, chomp);
        buf->WriteS8(PlayerToIndex(reinterpret_cast<cPlayer*>(chomp->mpTarget)));
        buf->WriteS32(static_cast<s32>(chomp->meChainChompState));
        buf->WriteBytes(&chomp->mtStateTimer, sizeof(Timer));
        buf->WriteS8(PlayerToIndex(reinterpret_cast<cPlayer*>(chomp->mpThrower)));
        buf->WriteS32(chomp->mnThrowerPadID);
    }
}

static void LoadNPCs(SaveStateBuffer* buf)
{
    NPCManager* npcs = CurrentNPCs();
    Bowser* bowser = npcs != nullptr ? npcs->mpBowser : nullptr;
    ChainChomp* chomp = npcs != nullptr ? npcs->mpChainChomp : nullptr;
    const float tilt = buf->ReadF32();
    if (tilt != Bowser::mfYAxisTilt)
        Bowser::SetTiltParameters(tilt);   /* the pitch tilt, and the force it puts on the ball */

    if (buf->ReadU8() != 0)
    {
        /* Both games load the same stadium, so both have him or neither does. */
        if (bowser == nullptr)
            return;
        LoadMovableNPC(buf, bowser);
        bowser->mpTarget = reinterpret_cast<cFielder*>(IndexToPlayer(buf->ReadS8()));
        buf->ReadBytes(&bowser->mv3TargetPos, sizeof(nlVector3));
        bowser->meBowserState = static_cast<eBowserState>(buf->ReadS32());
        buf->ReadBytes(&bowser->mtStateTimer, sizeof(Timer));
        buf->ReadBytes(&bowser->mtActiveTimer, sizeof(Timer));
        bowser->mAnimID = buf->ReadS32();
        bowser->mbDoTilt = buf->ReadU8() != 0;
        bowser->mAttackType = static_cast<eBowserAttackType>(buf->ReadS32());
        bowser->mStompStage = buf->ReadS32();
        bowser->mbFirstTime = buf->ReadU8() != 0;
        bowser->mbAlive = buf->ReadU8() != 0;
        bowser->mbResetPending = buf->ReadU8() != 0;
    }

    if (buf->ReadU8() != 0)
    {
        if (chomp == nullptr)
            return;
        LoadMovableNPC(buf, chomp);
        chomp->mpTarget = reinterpret_cast<cFielder*>(IndexToPlayer(buf->ReadS8()));
        chomp->meChainChompState = static_cast<eChainChompState>(buf->ReadS32());
        buf->ReadBytes(&chomp->mtStateTimer, sizeof(Timer));
        chomp->mpThrower = reinterpret_cast<cFielder*>(IndexToPlayer(buf->ReadS8()));
        chomp->mnThrowerPadID = buf->ReadS32();
    }
}

bool SaveGameState(SaveStateBuffer* buf)
{
    buf->Reset();
    s_trackedFieldCount = 0;

    /* Header. */
    TrackField("Header", buf->Size());
    buf->WriteU32(SAVESTATE_MAGIC);
    buf->WriteU32(SAVESTATE_VERSION);
    /* Who holds the ball, first, so LoadGameState knows before it changes anything. */
    buf->WriteS8(g_pBall ? PlayerToIndex(g_pBall->m_pOwner) : -1);

    /* ---- RNG state ---- */
    TrackField("RNG", buf->Size());
    {
        unsigned long mt_state[624];
        int mti_state;
        netplay_mt_get_state(mt_state, &mti_state);
        buf->WriteArray(mt_state, 624);
        buf->WriteS32(mti_state);
        buf->WriteU32(nlDefaultSeed);
    }

    /* ---- FixedUpdateTask timing ---- */
    TrackField("Timing", buf->Size());
    /* mAccumulatedDeltaT is real (render) time waiting to be simulated, not
       simulation state: restoring it on rollback skipped or doubled ticks, and
       it differs between the two PCs, which would break desync checksums. */
    buf->WriteF32(0.0f);
    buf->WriteF32(FixedUpdateTask::mSimulationTime);
    buf->WriteF32(FixedUpdateTask::mfFrameLockTime);
    /* PORT: the slow-motion factor only sets how often game frames run, never what they compute,
       and render-rate code (cameras, presentations) puts it back to 1 at a different moment on
       each PC: left out of the comparison, and each game keeps its own. */
    buf->WriteF32(1.0f);

    /* ---- cGame state ---- */
    TrackField("cGame", buf->Size());
    if (g_pGame)
    {
        buf->WriteU8(1);
        buf->WriteU32(static_cast<u32>(g_pGame->m_eGameState));
        buf->WriteU8(*(const u8*)&g_pGame->m_bBallInNet);
        buf->WriteS32(g_pGame->m_nLastTeamToScore);
        buf->WriteF32(g_pGame->m_fGameDuration);
        buf->WriteU8(*(const u8*)&g_pGame->mbCaptainShotToScoreOn);
        buf->WriteU8(*(const u8*)&g_pGame->mIsPure);
        buf->WriteU8(*(const u8*)&g_pGame->mInSuddenDeath);
        buf->WriteF32(g_pGame->mfCheatTilt);
        buf->WriteS32(g_pGame->mThoughtsAllowedThisUpdate);
        buf->WriteS8(PlayerToIndex(g_pGame->m_pScorer));
        buf->WriteS8(PlayerToIndex(g_pGame->m_pAssister));
        buf->WriteS8(PlayerToIndex(g_pGame->m_pTeamTouch[0]));
        buf->WriteS8(PlayerToIndex(g_pGame->m_pTeamTouch[1]));

        /* Clock states — save the critical fields. */
        SaveClock(buf, g_pGame->m_pGameClock);
        SaveClock(buf, g_pGame->m_pPostResetClock);
        SaveClock(buf, g_pGame->m_pPostGameDoneClock);

        /* Bowser timer. */
        buf->WriteBytes(&g_pGame->mBowserTimer, sizeof(Timer));
    }
    else
    {
        buf->WriteU8(0);
    }

    /* ---- Teams ---- */
    TrackField("Teams", buf->Size());
    for (int t = 0; t < 2; t++)
    {
        cTeam* team = g_pTeams[t];
        if (!team) { buf->WriteU8(0); continue; }
        buf->WriteU8(1);

        buf->WriteS32(team->m_nScore);
        buf->WriteS32(team->m_nCurrentPowerUp);
        buf->WriteBytes(team->m_ePowerupList, sizeof(team->m_ePowerupList));
        buf->WriteF32(team->mfPowerupMeter);
        buf->WriteF32(team->mfPowerupTimer);
        buf->WriteU32(static_cast<u32>(team->mpCurrentSituation));
        buf->WriteU32(static_cast<u32>(team->meCurrentTeamStyle));
        buf->WriteU8(*(const u8*)&team->mbHasToggledPowerup);
        buf->WriteBytes(team->mfBallInterceptTimes, sizeof(team->mfBallInterceptTimes));
        buf->WriteBytes(&team->mtTeamStyleTimer, sizeof(Timer));
        buf->WriteBytes(&team->mtMarkTimer, sizeof(Timer));
        buf->WriteBytes(&team->mtRoleTimer, sizeof(Timer));
        buf->WriteBytes(&team->mtDefensiveZoneTimer, sizeof(Timer));
        buf->WriteBytes(&team->mtBallInterceptTimer, sizeof(Timer));
    }

    /* ---- Ball ---- */
    TrackField("Ball", buf->Size());
    if (g_pBall)
    {
        buf->WriteU8(1);

        buf->WriteBytes(&g_pBall->m_v3Position, sizeof(nlVector3));
        buf->WriteBytes(&g_pBall->m_v3PrevPosition, sizeof(nlVector3));
        buf->WriteBytes(&g_pBall->m_v3Velocity, sizeof(nlVector3));
        buf->WriteBytes(&g_pBall->m_qOrientation, sizeof(nlQuaternion));
        buf->WriteBytes(&g_pBall->m_v3PassIntercept, sizeof(nlVector3));
        buf->WriteBytes(&g_pBall->m_v3ShotTarget, sizeof(nlVector3));
        buf->WriteBytes(&g_pBall->m_v3ShotOrigin, sizeof(nlVector3));

        buf->WriteU32(g_pBall->m_bBallPathChangeCount);
        buf->WriteU32(g_pBall->m_bBallDeflectCount);
        buf->WriteBytes(&g_pBall->m_tShotTimer, sizeof(Timer));
        buf->WriteBytes(&g_pBall->m_tNoPickupTimer, sizeof(Timer));
        buf->WriteBytes(&g_pBall->m_tPassTargetTimer, sizeof(Timer));
        buf->WriteBytes(&g_pBall->m_tBuzzerBeaterTimer, sizeof(Timer));
        buf->WriteF32(g_pBall->m_fTotalPassTime);

        buf->WriteS8(PlayerToIndex(g_pBall->m_pOwner));
        buf->WriteS8(PlayerToIndex(g_pBall->m_pPrevOwner));
        buf->WriteS8(PlayerToIndex(g_pBall->m_pLastTouch));
        buf->WriteS8(PlayerToIndex(g_pBall->m_pPassTarget));
        buf->WriteS8(PlayerToIndex(g_pBall->m_pShooter));

        buf->WriteU32(g_pBall->m_uGoalType);
        buf->WriteU8(*(const u8*)&g_pBall->mbIsPerfectShot);
        buf->WriteU8(*(const u8*)&g_pBall->mbHyperSTS);
        buf->WriteU8(*(const u8*)&g_pBall->mbCanDamage);

        /* Ball physics body. */
        if (g_pBall->m_pPhysicsBall)
        {
            buf->WriteU8(1);
            PhysicsObject* physObj = reinterpret_cast<PhysicsObject*>(g_pBall->m_pPhysicsBall);
            SaveODEBody(buf, physObj->m_bodyID);
            buf->WriteBytes(&physObj->m_position, sizeof(nlVector3));
            buf->WriteBytes(&physObj->m_linearVelocity, sizeof(nlVector3));
        }
        else
            buf->WriteU8(0);
    }
    else
    {
        buf->WriteU8(0);
    }

    /* ---- Characters (10 total) ---- */
    for (int i = 0; i < 10; i++)
    {
        cCharacter* ch = g_pCharacters[i];
        if (!ch) { buf->WriteU8(0); continue; }
        buf->WriteU8(1);

        static char s_chBaseTag[10][32];
        static char s_chPlayerTag[10][32];
        static char s_chSpecTag[10][32];
        snprintf(s_chBaseTag[i], 32, "Char_%d_Base", i);
        snprintf(s_chPlayerTag[i], 32, "Char_%d_Player", i);
        snprintf(s_chSpecTag[i], 32, "Char_%d_Spec", i);

        TrackField(s_chBaseTag[i], buf->Size());
        SaveCharacterBase(buf, ch);

        /* cPlayer fields (common to Fielder and Goalie). */
        TrackField(s_chPlayerTag[i], buf->Size());
        cPlayer* player = reinterpret_cast<cPlayer*>(ch);
        buf->WriteS32(player->m_ID);
        buf->WriteU8(*(const u8*)&player->m_bIsContactingWall);
        buf->WriteU32(static_cast<u32>(player->m_ePositionSeekState));
        buf->WriteBytes(&player->m_v3AIPosition, sizeof(nlVector3));
        buf->WriteU32(static_cast<u32>(player->m_eBallRotationMode));
        buf->WriteU8(*(const u8*)&player->m_ResetBaseBallOrientation);
        buf->WriteBytes(&player->m_BaseBallOrientation, sizeof(nlQuaternion));
        buf->WriteBytes(&player->m_tBallPossessionTimer, sizeof(Timer));
        buf->WriteBytes(&player->m_tBallUnPossessionTimer, sizeof(Timer));
        buf->WriteBytes(&player->m_tNoPickupTimer, sizeof(Timer));
        buf->WriteBytes(&player->m_tNoPickupPassInterceptTimer, sizeof(Timer));
        buf->WriteF32(player->m_fShotStrengthTime);
        buf->WriteBytes(&player->m_tSlideAttackTimer, sizeof(Timer));
        buf->WriteBytes(&player->m_tLooseBallPassTimer, sizeof(Timer));
        buf->WriteBytes(&player->m_tInactivityTimer, sizeof(Timer));
        buf->WriteBytes(player->m_tSwapControllerTimer, sizeof(Timer) * 4);
        buf->WriteU8(*(const u8*)&player->m_bCanTestController);
        buf->WriteU32(static_cast<u32>(player->m_eLastPadAction));
        buf->WriteU16(player->m_aSwapFacingDirection);
        buf->WriteBytes(&player->m_tSwapFacingTimer, sizeof(Timer));
        buf->WriteF32(player->m_UserControlledTime);

        /* Ball possession pointer as index. */
        buf->WriteS8(player->m_pBall ? 1 : 0);

        /* Determine if this is a Fielder (0..7) or Goalie (8, 9). */
        bool isGoalie = (i >= 8);
        buf->WriteU8(isGoalie ? 1 : 0);

        TrackField(s_chSpecTag[i], buf->Size());

        if (!isGoalie)
        {
            /* Save Fielder-specific state. */
            cFielder* fielder = reinterpret_cast<cFielder*>(ch);

            /* Action state variables — serialize individual fields cleanly */
            if (i == 0) TrackField("C0_DesireState", buf->Size());
            buf->WriteU32(static_cast<u32>(fielder->m_eFielderDesireState));
            if (i == 0) TrackField("C0_PrevDesire", buf->Size());
            buf->WriteU32(static_cast<u32>(fielder->m_ePrevFielderDesireState));
            buf->WriteS32(fielder->m_eDesireSubState);
            if (i == 0) TrackField("C0_DesireDuration", buf->Size());
            buf->WriteBytes(&fielder->m_tDesireDuration, sizeof(Timer));
            buf->WriteF32(fielder->m_fDesireConfidence);

            if (i == 0) TrackField("C0_DekeVars", buf->Size());
            buf->WriteBytes(&fielder->mActionDekeVars, sizeof(fielder->mActionDekeVars));
            if (i == 0) TrackField("C0_ElecVars", buf->Size());
            buf->WriteF32(fielder->mActionElectrocutionVars.electrocutionTime);
            if (i == 0) TrackField("C0_HitReactVars", buf->Size());
            buf->WriteU8(*(const u8*)&fielder->mActionHitReactActionVars.bDoFrameLock);
            if (i == 0) TrackField("C0_ShotVars", buf->Size());
            buf->WriteU8(*(const u8*)&fielder->mActionShotVars.bIsShootToScore);
            buf->WriteU8(*(const u8*)&fielder->mActionShotVars.bIsChipShot);
            if (i == 0) TrackField("C0_RunningVars", buf->Size());
            buf->WriteU32(static_cast<u32>(fielder->mActionRunningVars.eLastStrafeDirection));
            buf->WriteU8(*(const u8*)&fielder->mActionRunningVars.bFirstCycleOfTurbo);
            if (i == 0) TrackField("C0_SlideAttackVars", buf->Size());
            buf->WriteU32(static_cast<u32>(fielder->mActionSlideAttackVars.eSlideAttackState));
            buf->WriteU8(*(const u8*)&fielder->mActionSlideAttackVars.bAttackSucceeded);
            buf->WriteU8(*(const u8*)&fielder->mActionSlideAttackVars.bIsBButtonReset);
            buf->WriteU8(*(const u8*)&fielder->mActionSlideAttackVars.bWasStarMushroomUsedDuring);

            /* ShootToScore POD fields */
            if (i == 0) TrackField("C0_ShootToScoreVars", buf->Size());
            buf->WriteF32(fielder->mActionShootToScoreVars.fFrameButtonDownTime2);
            buf->WriteF32(fielder->mActionShootToScoreVars.fFrameButtonDownTime1);
            buf->WriteF32(fielder->mActionShootToScoreVars.fGreenRegionWidth);
            buf->WriteF32(fielder->mActionShootToScoreVars.fShootToScoreActiveTime);
            buf->WriteF32(fielder->mActionShootToScoreVars.fMeterFractionTime);
            buf->WriteBytes(&fielder->mActionShootToScoreVars.v3MeterPosition, sizeof(nlVector3));
            buf->WriteU8(*(const u8*)&fielder->mActionShootToScoreVars.bShootWasPressed);
            buf->WriteU8(*(const u8*)&fielder->mActionShootToScoreVars.isCaptainSts);
            buf->WriteU8(*(const u8*)&fielder->mActionShootToScoreVars.isCurrentlyInvincible);
            buf->WriteU8(*(const u8*)&fielder->mActionShootToScoreVars.isInUnbreakablePart);
            buf->WriteF32(fielder->mActionShootToScoreVars.preCaptainStsPlaybackSpeed);
            buf->WriteF32(fielder->mActionShootToScoreVars.fCaptainYellowWidth);

            /* Powerup state. */
            buf->WriteU32(static_cast<u32>(fielder->m_ePowerup));
            buf->WriteS32(fielder->mnNumPowerups);

            /* Role and marking. */
            buf->WriteU32(static_cast<u32>(fielder->m_eRole));
            buf->WriteS8(PlayerToIndex(fielder->m_pMark));
            buf->WriteS8(PlayerToIndex(fielder->m_pMarker));
            buf->WriteBytes(&fielder->m_v3DesiredPosition, sizeof(nlVector3));

            /* Desire working variables (v5): the AI reads these on later frames. */
            if (i == 0) TrackField("C0_DesireCommonVars", buf->Size());
            SaveFielderDesireVars(buf, fielder);
        }
        else
        {
            /* Save Goalie-specific state. */
            Goalie* goalie = reinterpret_cast<Goalie*>(ch);

            buf->WriteU32(static_cast<u32>(goalie->mGoalieActionState));
            buf->WriteU32(static_cast<u32>(goalie->mPrevGoalieActionState));
            buf->WriteU32(static_cast<u32>(goalie->mUrgency));
            buf->WriteS8(PlayerToIndex(goalie->mpPassTarget));
            buf->WriteS8(PlayerToIndex(goalie->mpShooter));
            buf->WriteS32(goalie->mnSubstate);
            buf->WriteU32(static_cast<u32>(goalie->mMoveDirection));
            buf->WriteU32(static_cast<u32>(goalie->mCrouchType));
            buf->WriteF32(goalie->mfSwitchTime);
            buf->WriteU32(goalie->muSaveType);
            buf->WriteF32(goalie->mfWaitTime);
            buf->WriteF32(goalie->mfTimeTilSave);
            buf->WriteU8(*(const u8*)&goalie->mbPlayMiss);
            buf->WriteU8(*(const u8*)&goalie->mbShouldMiss);
            buf->WriteU8(*(const u8*)&goalie->mbStunEffectActive);
            buf->WriteU8(*(const u8*)&goalie->mbDoIntercept);
            buf->WriteU8(*(const u8*)&goalie->mbDoNavigate);
            buf->WriteU8(*(const u8*)&goalie->mbDoHeadTrack);
            buf->WriteU8(*(const u8*)&goalie->mbBallImpacted);
            buf->WriteU8(*(const u8*)&goalie->mbNoUserControl);
            buf->WriteU8(*(const u8*)&goalie->mbIsPosed);
            buf->WriteU8(*(const u8*)&goalie->mbPickedUp);
            buf->WriteBytes(&goalie->mv3LocalContactPosition, sizeof(nlVector3));
            buf->WriteBytes(&goalie->mv3LocalContactVelocity, sizeof(nlVector3));
            buf->WriteBytes(&goalie->mv3TargetPosition, sizeof(nlVector3));
            buf->WriteBytes(&goalie->mv3NavTarget, sizeof(nlVector3));
            buf->WriteBytes(&goalie->mv3LocalNavTarget, sizeof(nlVector3));
            buf->WriteS16(goalie->maLocalAngle);
            buf->WriteF32(goalie->mfTargetTime);
            buf->WriteF32(goalie->mfTargetDist);
            buf->WriteF32(goalie->mfSpeedScale);
            buf->WriteU32(goalie->muBallChangeCount);
            buf->WriteU32(goalie->muBallDeflectCount);
            buf->WriteBytes(&goalie->mFatigue, sizeof(GoalieFatigue));
            buf->WriteS32(goalie->mnOffplayPending);
            buf->WriteU8(*(const u8*)&Goalie::mbPosGoalieNetCheck);
            buf->WriteU8(*(const u8*)&Goalie::mbNegGoalieNetCheck);
            buf->WriteF32(Goalie::mfGoalieStepDist);
            buf->WriteF32(Goalie::mfGoalieStrafeDist);
            buf->WriteF32(Goalie::mfGoalieRunDist);
            buf->WriteF32(Goalie::mfGoalieUrgentDist);
        }
    }

    /* ---- FlickDetection ring buffers (4 pads × 16 chars) ---- */
    TrackField("FlickDetection", buf->Size());
    for (int i = 0; i < 4; i++)
    {
        char flickBuf[16];
        int flickSize, flickEnd;
        netplay_flick_get_history(i, flickBuf, &flickSize, &flickEnd);
        buf->WriteS32(flickSize);
        buf->WriteS32(flickEnd);
        buf->WriteBytes(flickBuf, 16);
    }

    /* ---- AI thoughts queue (whose turn it is to think) ---- */
    TrackField("Thoughts", buf->Size());
    {
        unsigned long thoughts[64];
        int n = PortNetplayGetThoughts(thoughts, 64);
        if (n > 64) n = 64;
        buf->WriteU16(static_cast<u16>(n));
        for (int i = 0; i < n; i++)
            buf->WriteU32(static_cast<u32>(thoughts[i]));
    }

    /* ---- Every running clock, in list order ---- */
    TrackField("Clocks", buf->Size());
    {
        /* The active list is a ring (nlDLRing): walk it once, back to the start. */
        u16 count = 0;
        Clock* first = ClockManager::m_activeList;
        for (Clock* c = first; c != NULL && count < 256; )
        {
            count++;
            c = c->m_next;
            if (c == first) break;
        }
        buf->WriteU16(count);
        u16 i = 0;
        for (Clock* c = first; c != NULL && i < count; c = c->m_next, i++)
        {
            buf->WriteS32(ClockCallbackKey(c));
            buf->WriteU32(ClockParamKey(c->m_uParam1));
            buf->WriteU32(ClockParamKey(c->m_uParam2));
            buf->WriteF32(c->m_fTimeScale);
            buf->WriteF32(c->m_fTimer);
            buf->WriteF32(c->m_fEndTime);
            buf->WriteU32(c->m_uActiveStates);
            buf->WriteU32(static_cast<u32>(c->m_clockState));
        }
    }

    /* ---- Camera angle each port's stick is turned by (v7) ---- */
    TrackField("StickAngles", buf->Size());
    buf->WriteU16(PortGetPadRemap(0));
    buf->WriteU16(PortGetPadRemap(1));

    /* ---- The order players update in (v8) ----
       cGame::RandomizePlayerUpdateOrder shuffles it at each kickoff with each
       PC's own random numbers; it decides which player thinks first and so
       who gets which random number. */
    TrackField("UpdateOrder", buf->Size());
    for (int i = 0; i < 10; i++)
        buf->WriteS8((g_pGame && g_pGame->m_pRandomPlayersArray)
                         ? PlayerToIndex(g_pGame->m_pRandomPlayersArray[i]) : -1);

    TrackField("BallPhysicsExtra", buf->Size());
    SaveBallPhysicsExtra(buf);
    TrackField("NetPositiveX", buf->Size());
    SaveNetMesh(buf, NetMesh::spPositiveXNetMesh);
    TrackField("NetNegativeX", buf->Size());
    SaveNetMesh(buf, NetMesh::spNegativeXNetMesh);
    TrackField("Formations", buf->Size());
    SaveFormations(buf, g_pTeams[0]);
    SaveFormations(buf, g_pTeams[1]);
    TrackField("NPCs", buf->Size());
    SaveNPCs(buf);

    return true;
}

/* ======================================================================== */
/*  LoadGameState                                                            */
/* ======================================================================== */

bool LoadGameState(SaveStateBuffer* buf)
{
    buf->SeekRead(0);

    /* Header check. */
    u32 magic = buf->ReadU32();
    u32 version = buf->ReadU32();
    if (magic != SAVESTATE_MAGIC || version != SAVESTATE_VERSION)
        return false;

    /* If someone else holds the ball physically than in the state being
       loaded, take it from them; it is attached to the loaded owner at the end.
       (Detaching and re-attaching every load reset the dribble each rollback.) */
    PortNetplayDetachBallUnlessHeldBy(IndexToPlayer(buf->ReadS8()));

    /* ---- RNG state ---- */
    {
        unsigned long mt_state[624];
        int mti_state;
        buf->ReadArray(mt_state, 624);
        mti_state = buf->ReadS32();
        netplay_mt_set_state(mt_state, mti_state);
        nlDefaultSeed = buf->ReadU32();
    }

    /* ---- FixedUpdateTask timing ---- */
    (void)buf->ReadF32();  /* was mAccumulatedDeltaT; see SaveGameState */
    FixedUpdateTask::mSimulationTime = buf->ReadF32();
    FixedUpdateTask::mfFrameLockTime = buf->ReadF32();
    (void)buf->ReadF32();   /* the slow-motion factor: see SaveGameState */

    /* ---- cGame state ---- */
    u8 hasGame = buf->ReadU8();
    if (hasGame && g_pGame)
    {
        g_pGame->m_eGameState = static_cast<eGameState>(buf->ReadU32());
        *(u8*)&g_pGame->m_bBallInNet = buf->ReadU8();
        g_pGame->m_nLastTeamToScore = buf->ReadS32();
        g_pGame->m_fGameDuration = buf->ReadF32();
        *(u8*)&g_pGame->mbCaptainShotToScoreOn = buf->ReadU8();
        *(u8*)&g_pGame->mIsPure = buf->ReadU8();
        *(u8*)&g_pGame->mInSuddenDeath = buf->ReadU8();
        g_pGame->mfCheatTilt = buf->ReadF32();
        g_pGame->mThoughtsAllowedThisUpdate = buf->ReadS32();
        g_pGame->m_pScorer = IndexToPlayer(buf->ReadS8());
        g_pGame->m_pAssister = IndexToPlayer(buf->ReadS8());
        g_pGame->m_pTeamTouch[0] = IndexToPlayer(buf->ReadS8());
        g_pGame->m_pTeamTouch[1] = IndexToPlayer(buf->ReadS8());

        LoadClock(buf, g_pGame->m_pGameClock);
        LoadClock(buf, g_pGame->m_pPostResetClock);
        LoadClock(buf, g_pGame->m_pPostGameDoneClock);

        buf->ReadBytes(&g_pGame->mBowserTimer, sizeof(Timer));
    }

    /* ---- Teams ---- */
    for (int t = 0; t < 2; t++)
    {
        u8 hasTeam = buf->ReadU8();
        if (!hasTeam) continue;
        cTeam* team = g_pTeams[t];
        if (!team) continue;

        team->m_nScore = buf->ReadS32();
        team->m_nCurrentPowerUp = buf->ReadS32();
        buf->ReadBytes(team->m_ePowerupList, sizeof(team->m_ePowerupList));
        team->mfPowerupMeter = buf->ReadF32();
        team->mfPowerupTimer = buf->ReadF32();
        team->mpCurrentSituation = static_cast<eSituation>(buf->ReadU32());
        team->meCurrentTeamStyle = static_cast<eTeamStyle>(buf->ReadU32());
        *(u8*)&team->mbHasToggledPowerup = buf->ReadU8();
        buf->ReadBytes(team->mfBallInterceptTimes, sizeof(team->mfBallInterceptTimes));
        buf->ReadBytes(&team->mtTeamStyleTimer, sizeof(Timer));
        buf->ReadBytes(&team->mtMarkTimer, sizeof(Timer));
        buf->ReadBytes(&team->mtRoleTimer, sizeof(Timer));
        buf->ReadBytes(&team->mtDefensiveZoneTimer, sizeof(Timer));
        buf->ReadBytes(&team->mtBallInterceptTimer, sizeof(Timer));
    }

    /* ---- Ball ---- */
    u8 hasBall = buf->ReadU8();
    if (hasBall && g_pBall)
    {
        buf->ReadBytes(&g_pBall->m_v3Position, sizeof(nlVector3));
        buf->ReadBytes(&g_pBall->m_v3PrevPosition, sizeof(nlVector3));
        buf->ReadBytes(&g_pBall->m_v3Velocity, sizeof(nlVector3));
        buf->ReadBytes(&g_pBall->m_qOrientation, sizeof(nlQuaternion));
        buf->ReadBytes(&g_pBall->m_v3PassIntercept, sizeof(nlVector3));
        buf->ReadBytes(&g_pBall->m_v3ShotTarget, sizeof(nlVector3));
        buf->ReadBytes(&g_pBall->m_v3ShotOrigin, sizeof(nlVector3));

        g_pBall->m_bBallPathChangeCount = buf->ReadU32();
        g_pBall->m_bBallDeflectCount = buf->ReadU32();
        buf->ReadBytes(&g_pBall->m_tShotTimer, sizeof(Timer));
        buf->ReadBytes(&g_pBall->m_tNoPickupTimer, sizeof(Timer));
        buf->ReadBytes(&g_pBall->m_tPassTargetTimer, sizeof(Timer));
        buf->ReadBytes(&g_pBall->m_tBuzzerBeaterTimer, sizeof(Timer));
        g_pBall->m_fTotalPassTime = buf->ReadF32();

        g_pBall->m_pOwner = IndexToPlayer(buf->ReadS8());
        g_pBall->m_pPrevOwner = IndexToPlayer(buf->ReadS8());
        g_pBall->m_pLastTouch = IndexToPlayer(buf->ReadS8());
        g_pBall->m_pPassTarget = IndexToPlayer(buf->ReadS8());
        g_pBall->m_pShooter = IndexToPlayer(buf->ReadS8());

        g_pBall->m_uGoalType = buf->ReadU32();
        *(u8*)&g_pBall->mbIsPerfectShot = buf->ReadU8();
        *(u8*)&g_pBall->mbHyperSTS = buf->ReadU8();
        *(u8*)&g_pBall->mbCanDamage = buf->ReadU8();

        u8 hasBallPhys = buf->ReadU8();
        if (hasBallPhys && g_pBall->m_pPhysicsBall)
        {
            PhysicsObject* physObj = reinterpret_cast<PhysicsObject*>(g_pBall->m_pPhysicsBall);
            LoadODEBody(buf, physObj->m_bodyID);
            buf->ReadBytes(&physObj->m_position, sizeof(nlVector3));
            buf->ReadBytes(&physObj->m_linearVelocity, sizeof(nlVector3));
        }
    }

    /* ---- Characters ---- */
    for (int i = 0; i < 10; i++)
    {
        u8 hasCh = buf->ReadU8();
        if (!hasCh) continue;
        cCharacter* ch = g_pCharacters[i];
        if (!ch) continue;

        LoadCharacterBase(buf, ch);

        cPlayer* player = reinterpret_cast<cPlayer*>(ch);
        player->m_ID = buf->ReadS32();
        *(u8*)&player->m_bIsContactingWall = buf->ReadU8();
        player->m_ePositionSeekState = static_cast<ePositionSeekState>(buf->ReadU32());
        buf->ReadBytes(&player->m_v3AIPosition, sizeof(nlVector3));
        player->m_eBallRotationMode = static_cast<eBallRotationMode>(buf->ReadU32());
        *(u8*)&player->m_ResetBaseBallOrientation = buf->ReadU8();
        buf->ReadBytes(&player->m_BaseBallOrientation, sizeof(nlQuaternion));
        buf->ReadBytes(&player->m_tBallPossessionTimer, sizeof(Timer));
        buf->ReadBytes(&player->m_tBallUnPossessionTimer, sizeof(Timer));
        buf->ReadBytes(&player->m_tNoPickupTimer, sizeof(Timer));
        buf->ReadBytes(&player->m_tNoPickupPassInterceptTimer, sizeof(Timer));
        player->m_fShotStrengthTime = buf->ReadF32();
        buf->ReadBytes(&player->m_tSlideAttackTimer, sizeof(Timer));
        buf->ReadBytes(&player->m_tLooseBallPassTimer, sizeof(Timer));
        buf->ReadBytes(&player->m_tInactivityTimer, sizeof(Timer));
        buf->ReadBytes(player->m_tSwapControllerTimer, sizeof(Timer) * 4);
        *(u8*)&player->m_bCanTestController = buf->ReadU8();
        player->m_eLastPadAction = static_cast<ePadActions>(buf->ReadU32());
        player->m_aSwapFacingDirection = buf->ReadU16();
        buf->ReadBytes(&player->m_tSwapFacingTimer, sizeof(Timer));
        player->m_UserControlledTime = buf->ReadF32();

        s8 hasBall = buf->ReadS8();
        player->m_pBall = hasBall ? g_pBall : nullptr;

        bool isGoalie = buf->ReadU8() != 0;

        if (!isGoalie)
        {
            cFielder* fielder = reinterpret_cast<cFielder*>(ch);

            fielder->m_eFielderDesireState = static_cast<eFielderDesireState>(buf->ReadU32());
            fielder->m_ePrevFielderDesireState = static_cast<eFielderDesireState>(buf->ReadU32());
            fielder->m_eDesireSubState = buf->ReadS32();
            buf->ReadBytes(&fielder->m_tDesireDuration, sizeof(Timer));
            fielder->m_fDesireConfidence = buf->ReadF32();

            buf->ReadBytes(&fielder->mActionDekeVars, sizeof(fielder->mActionDekeVars));
            fielder->mActionElectrocutionVars.electrocutionTime = buf->ReadF32();
            *(u8*)&fielder->mActionHitReactActionVars.bDoFrameLock = buf->ReadU8();
            *(u8*)&fielder->mActionShotVars.bIsShootToScore = buf->ReadU8();
            *(u8*)&fielder->mActionShotVars.bIsChipShot = buf->ReadU8();
            fielder->mActionRunningVars.eLastStrafeDirection = static_cast<eStrafeDirection>(buf->ReadU32());
            *(u8*)&fielder->mActionRunningVars.bFirstCycleOfTurbo = buf->ReadU8();
            fielder->mActionSlideAttackVars.eSlideAttackState = static_cast<eSlideAttackState>(buf->ReadU32());
            *(u8*)&fielder->mActionSlideAttackVars.bAttackSucceeded = buf->ReadU8();
            *(u8*)&fielder->mActionSlideAttackVars.bIsBButtonReset = buf->ReadU8();
            *(u8*)&fielder->mActionSlideAttackVars.bWasStarMushroomUsedDuring = buf->ReadU8();

            fielder->mActionShootToScoreVars.fFrameButtonDownTime2 = buf->ReadF32();
            fielder->mActionShootToScoreVars.fFrameButtonDownTime1 = buf->ReadF32();
            fielder->mActionShootToScoreVars.fGreenRegionWidth = buf->ReadF32();
            fielder->mActionShootToScoreVars.fShootToScoreActiveTime = buf->ReadF32();
            fielder->mActionShootToScoreVars.fMeterFractionTime = buf->ReadF32();
            buf->ReadBytes(&fielder->mActionShootToScoreVars.v3MeterPosition, sizeof(nlVector3));
            *(u8*)&fielder->mActionShootToScoreVars.bShootWasPressed = buf->ReadU8();
            *(u8*)&fielder->mActionShootToScoreVars.isCaptainSts = buf->ReadU8();
            *(u8*)&fielder->mActionShootToScoreVars.isCurrentlyInvincible = buf->ReadU8();
            *(u8*)&fielder->mActionShootToScoreVars.isInUnbreakablePart = buf->ReadU8();
            fielder->mActionShootToScoreVars.preCaptainStsPlaybackSpeed = buf->ReadF32();
            fielder->mActionShootToScoreVars.fCaptainYellowWidth = buf->ReadF32();

            fielder->m_ePowerup = static_cast<ePowerUpType>(buf->ReadU32());
            fielder->mnNumPowerups = buf->ReadS32();

            fielder->m_eRole = static_cast<eRole>(buf->ReadU32());
            fielder->m_pMark = reinterpret_cast<cFielder*>(IndexToPlayer(buf->ReadS8()));
            fielder->m_pMarker = reinterpret_cast<cFielder*>(IndexToPlayer(buf->ReadS8()));
            buf->ReadBytes(&fielder->m_v3DesiredPosition, sizeof(nlVector3));
            LoadFielderDesireVars(buf, fielder);
        }
        else
        {
            Goalie* goalie = reinterpret_cast<Goalie*>(ch);

            goalie->mGoalieActionState = static_cast<eGoalieActionState>(buf->ReadU32());
            goalie->mPrevGoalieActionState = static_cast<eGoalieActionState>(buf->ReadU32());
            goalie->mUrgency = static_cast<eUrgency>(buf->ReadU32());
            goalie->mpPassTarget = IndexToPlayer(buf->ReadS8());
            goalie->mpShooter = reinterpret_cast<cFielder*>(IndexToPlayer(buf->ReadS8()));
            goalie->mnSubstate = buf->ReadS32();
            goalie->mMoveDirection = static_cast<eGoalieMoveDirection>(buf->ReadU32());
            goalie->mCrouchType = static_cast<eGoalieCrouchType>(buf->ReadU32());
            goalie->mfSwitchTime = buf->ReadF32();
            goalie->muSaveType = buf->ReadU32();
            goalie->mfWaitTime = buf->ReadF32();
            goalie->mfTimeTilSave = buf->ReadF32();
            *(u8*)&goalie->mbPlayMiss = buf->ReadU8();
            *(u8*)&goalie->mbShouldMiss = buf->ReadU8();
            *(u8*)&goalie->mbStunEffectActive = buf->ReadU8();
            *(u8*)&goalie->mbDoIntercept = buf->ReadU8();
            *(u8*)&goalie->mbDoNavigate = buf->ReadU8();
            *(u8*)&goalie->mbDoHeadTrack = buf->ReadU8();
            *(u8*)&goalie->mbBallImpacted = buf->ReadU8();
            *(u8*)&goalie->mbNoUserControl = buf->ReadU8();
            *(u8*)&goalie->mbIsPosed = buf->ReadU8();
            *(u8*)&goalie->mbPickedUp = buf->ReadU8();
            buf->ReadBytes(&goalie->mv3LocalContactPosition, sizeof(nlVector3));
            buf->ReadBytes(&goalie->mv3LocalContactVelocity, sizeof(nlVector3));
            buf->ReadBytes(&goalie->mv3TargetPosition, sizeof(nlVector3));
            buf->ReadBytes(&goalie->mv3NavTarget, sizeof(nlVector3));
            buf->ReadBytes(&goalie->mv3LocalNavTarget, sizeof(nlVector3));
            goalie->maLocalAngle = buf->ReadS16();
            goalie->mfTargetTime = buf->ReadF32();
            goalie->mfTargetDist = buf->ReadF32();
            goalie->mfSpeedScale = buf->ReadF32();
            goalie->muBallChangeCount = buf->ReadU32();
            goalie->muBallDeflectCount = buf->ReadU32();
            buf->ReadBytes(&goalie->mFatigue, sizeof(GoalieFatigue));
            goalie->mnOffplayPending = static_cast<eGoalieOffplayType>(buf->ReadS32());
            *(u8*)&Goalie::mbPosGoalieNetCheck = buf->ReadU8();
            *(u8*)&Goalie::mbNegGoalieNetCheck = buf->ReadU8();
            Goalie::mfGoalieStepDist = buf->ReadF32();
            Goalie::mfGoalieStrafeDist = buf->ReadF32();
            Goalie::mfGoalieRunDist = buf->ReadF32();
            Goalie::mfGoalieUrgentDist = buf->ReadF32();
        }
    }

    /* ---- FlickDetection ---- */
    for (int i = 0; i < 4; i++)
    {
        int flickSize = buf->ReadS32();
        int flickEnd  = buf->ReadS32();
        char flickBuf[16];
        buf->ReadBytes(flickBuf, 16);
        netplay_flick_set_history(i, flickBuf, flickSize, flickEnd);
    }

    /* ---- AI thoughts queue ---- */
    {
        unsigned long thoughts[64];
        int n = buf->ReadU16();
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++)
            thoughts[i] = buf->ReadU32();
        PortNetplaySetThoughts(thoughts, n);
    }

    /* ---- Every running clock ---- */
    LoadRunningClocks(buf);

    PortSetPadRemap(0, buf->ReadU16());
    PortSetPadRemap(1, buf->ReadU16());

    {
        s8 order[10];
        for (int i = 0; i < 10; i++)
            order[i] = buf->ReadS8();
        if (g_pGame && g_pGame->m_pRandomPlayersArray)
            for (int i = 0; i < 10; i++)
                if (order[i] >= 0)
                    g_pGame->m_pRandomPlayersArray[i] = IndexToPlayer(order[i]);
    }

    LoadBallPhysicsExtra(buf);
    LoadNetMesh(buf, NetMesh::spPositiveXNetMesh);
    LoadNetMesh(buf, NetMesh::spNegativeXNetMesh);
    LoadFormations(buf, g_pTeams[0]);
    LoadFormations(buf, g_pTeams[1]);
    LoadNPCs(buf);

    /* The loaded owner holds the ball in the physics engine too. */
    PortNetplayAttachBallToOwner();

    return true;
}

/* ======================================================================== */
/*  Quick checksum for desync detection                                      */
/* ======================================================================== */

u32 ComputeGameStateChecksum()
{
    /* We reuse SaveGameState into a scratch buffer, then checksum it.
       This is ~20 KB and runs in under 1ms, so it's fine for per-frame use. */
    static SaveStateBuffer s_checksumBuf;
    if (s_checksumBuf.Size() == 0)
        s_checksumBuf.Reserve(32 * 1024);

    SaveGameState(&s_checksumBuf);
    return s_checksumBuf.Checksum();
}

/* ======================================================================== */
/*  Rollback Determinism Self-Test                                           */
/* ======================================================================== */

extern float g_fFixedUpdateTick;
extern void UseFixedUpdatePad();
extern void UpdatePlatPad(float);

static void LogGoalieDiag(const char* label, char* outBuf, size_t outSize)
{
    Goalie* g = reinterpret_cast<Goalie*>(g_pCharacters[8]);
    snprintf(outBuf, outSize,
        "[%s] act=%d prev=%d sub=%d urg=%d mdir=%d nav=%d intc=%d pos=(%.4f,%.4f,%.4f) vel=(%.4f,%.4f,%.4f) actFace=%u desFace=%u actMDir=%u desMDir=%u actSpd=%.4f desSpd=%.4f navTgt=(%.4f,%.4f,%.4f) locTgt=(%.4f,%.4f,%.4f) tgtPos=(%.4f,%.4f,%.4f) tgtDist=%.4f tgtTime=%.4f spdScale=%.4f aAnim=%d aTime=%.4f physPos=(%.4f,%.4f,%.4f) physVel=(%.4f,%.4f,%.4f)",
        label, (int)g->mGoalieActionState, (int)g->mPrevGoalieActionState, g->mnSubstate, (int)g->mUrgency, (int)g->mMoveDirection,
        (int)g->mbDoNavigate, (int)g->mbDoIntercept,
        g->m_v3Position.x, g->m_v3Position.y, g->m_v3Position.z,
        g->m_v3Velocity.x, g->m_v3Velocity.y, g->m_v3Velocity.z,
        (unsigned int)g->m_aActualFacingDirection, (unsigned int)g->m_aDesiredFacingDirection,
        (unsigned int)g->m_aActualMovementDirection, (unsigned int)g->m_aDesiredMovementDirection,
        g->m_fActualSpeed, g->m_fDesiredSpeed,
        g->mv3NavTarget.x, g->mv3NavTarget.y, g->mv3NavTarget.z,
        g->mv3LocalNavTarget.x, g->mv3LocalNavTarget.y, g->mv3LocalNavTarget.z,
        g->mv3TargetPosition.x, g->mv3TargetPosition.y, g->mv3TargetPosition.z,
        g->mfTargetDist, g->mfTargetTime, g->mfSpeedScale,
        g->m_eAnimID, g->m_pCurrentAnimController ? g->m_pCurrentAnimController->m_fTime : -1.0f,
        g->m_pPhysicsCharacter ? reinterpret_cast<PhysicsObject*>(g->m_pPhysicsCharacter)->m_position.x : 0.0f,
        g->m_pPhysicsCharacter ? reinterpret_cast<PhysicsObject*>(g->m_pPhysicsCharacter)->m_position.y : 0.0f,
        g->m_pPhysicsCharacter ? reinterpret_cast<PhysicsObject*>(g->m_pPhysicsCharacter)->m_position.z : 0.0f,
        g->m_pPhysicsCharacter ? reinterpret_cast<PhysicsObject*>(g->m_pPhysicsCharacter)->m_linearVelocity.x : 0.0f,
        g->m_pPhysicsCharacter ? reinterpret_cast<PhysicsObject*>(g->m_pPhysicsCharacter)->m_linearVelocity.y : 0.0f,
        g->m_pPhysicsCharacter ? reinterpret_cast<PhysicsObject*>(g->m_pPhysicsCharacter)->m_linearVelocity.z : 0.0f
    );
}

bool NetplayTestRollbackDeterminism(int framesToTest, char* outResult, size_t outResultSize)
{
    cGame* pGame = g_pGame;
    if (!pGame)
    {
        snprintf(outResult, outResultSize, "No active match. Please start a match first.");
        return false;
    }

    if (framesToTest <= 0) framesToTest = 5;
    if (framesToTest > 15) framesToTest = 15;

    FILE* fDiag = fopen("rollback_goalie_diag.txt", "w");
    char diagBuf[512];
    LogGoalieDiag("S0_init", diagBuf, sizeof(diagBuf));
    if (fDiag) { fprintf(fDiag, "%s\n", diagBuf); fflush(fDiag); }

    /* 1. Take snapshot S0 of current state */
    SaveStateBuffer s0;
    s0.Reserve(32 * 1024);
    if (!SaveGameState(&s0))
    {
        if (fDiag) fclose(fDiag);
        snprintf(outResult, outResultSize, "ERROR: Failed to save initial state S0.");
        return false;
    }
    u32 checksum0 = s0.Checksum();
    size_t size0  = s0.Size();

    /* 2. Step forward N frames with current inputs */
    for (int i = 0; i < framesToTest; i++)
    {
        UseFixedUpdatePad();
        UpdatePlatPad(g_fFixedUpdateTick);
        cPadManager::Update(g_fFixedUpdateTick);
        FlickDetection::Update();
        FixedUpdateTask::CallFixedUpdateTasks();
        char label[32]; snprintf(label, sizeof(label), "Run1_F%d", i + 1);
        LogGoalieDiag(label, diagBuf, sizeof(diagBuf));
        if (fDiag) { fprintf(fDiag, "%s\n", diagBuf); fflush(fDiag); }
    }

    /* 3. Take snapshot S_N (run 1) */
    SaveStateBuffer s_run1;
    s_run1.Reserve(32 * 1024);
    SaveGameState(&s_run1);
    u32 checksum1 = s_run1.Checksum();
    size_t size1  = s_run1.Size();

    /* 4. Restore snapshot S0 (Rewind / Rollback!) */
    s0.SeekRead(0);
    if (!LoadGameState(&s0))
    {
        if (fDiag) fclose(fDiag);
        snprintf(outResult, outResultSize, "ERROR: Failed to load state S0 during rewind.");
        return false;
    }
    LogGoalieDiag("S0_restored", diagBuf, sizeof(diagBuf));
    if (fDiag) { fprintf(fDiag, "%s\n", diagBuf); fflush(fDiag); }

    /* 5. Verify round-trip restoration of S0 */
    SaveStateBuffer s0_verify;
    s0_verify.Reserve(32 * 1024);
    SaveGameState(&s0_verify);
    if (s0_verify.Checksum() != checksum0 || s0_verify.Size() != size0)
    {
        if (fDiag) fclose(fDiag);
        size_t minS = s0.Size() < s0_verify.Size() ? s0.Size() : s0_verify.Size();
        size_t diffOff = 0;
        const u8* d0 = s0.Data();
        const u8* dv = s0_verify.Data();
        for (size_t i = 0; i < minS; i++)
        {
            if (d0[i] != dv[i])
            {
                diffOff = i;
                break;
            }
        }

        size_t fieldStart = 0;
        const char* fieldName = GetTrackedFieldAt(diffOff, &fieldStart);

        char hex0[128] = {0};
        char hexV[128] = {0};
        size_t start = diffOff > 8 ? diffOff - 8 : 0;
        size_t len = 16;
        if (start + len > minS) len = minS - start;
        for (size_t k = 0; k < len; k++)
        {
            snprintf(hex0 + k * 3, 4, "%02X ", d0[start + k]);
            snprintf(hexV + k * 3, 4, "%02X ", dv[start + k]);
        }

        char fieldDump[2048] = {0};
        size_t dlen = 0;
        size_t minTrack = diffOff > 100 ? diffOff - 100 : 0;
        size_t maxTrack = diffOff + 100;
        for (int k = 0; k < s_trackedFieldCount; k++)
        {
            if (s_trackedFields[k].offset >= minTrack && s_trackedFields[k].offset <= maxTrack)
            {
                dlen += snprintf(fieldDump + dlen, sizeof(fieldDump) - dlen,
                                 "  [%zu] %s\n", s_trackedFields[k].offset, s_trackedFields[k].name);
            }
        }

        snprintf(outResult, outResultSize,
                 "FAIL: S0 roundtrip mismatch at byte %zu [field '%s' +%zu]:\n  orig:   %s\n  loaded: %s\nField map:\n%s",
                 diffOff, fieldName, diffOff - fieldStart, hex0, hexV, fieldDump);
        return false;
    }

    /* 6. Step forward N frames again with the EXACT SAME inputs */
    for (int i = 0; i < framesToTest; i++)
    {
        UseFixedUpdatePad();
        UpdatePlatPad(g_fFixedUpdateTick);
        cPadManager::Update(g_fFixedUpdateTick);
        FlickDetection::Update();
        FixedUpdateTask::CallFixedUpdateTasks();
        char label[32]; snprintf(label, sizeof(label), "Run2_F%d", i + 1);
        LogGoalieDiag(label, diagBuf, sizeof(diagBuf));
        if (fDiag) { fprintf(fDiag, "%s\n", diagBuf); fflush(fDiag); }
    }
    if (fDiag) { fclose(fDiag); fDiag = NULL; }

    /* 7. Take snapshot S_N (run 2) */
    SaveStateBuffer s_run2;
    s_run2.Reserve(32 * 1024);
    SaveGameState(&s_run2);
    u32 checksum2 = s_run2.Checksum();
    size_t size2  = s_run2.Size();

    /* 8. Compare run 1 and run 2! */
    if (checksum1 != checksum2 || size1 != size2)
    {
        size_t minSize = size1 < size2 ? size1 : size2;
        size_t diffOffset = 0;
        bool foundDiff = false;
        const u8* d1 = s_run1.Data();
        const u8* d2 = s_run2.Data();
        for (size_t i = 0; i < minSize; i++)
        {
            if (d1[i] != d2[i])
            {
                diffOffset = i;
                foundDiff = true;
                break;
            }
        }
        if (foundDiff)
        {
            size_t fieldStart = 0;
            const char* fieldName = GetTrackedFieldAt(diffOffset, &fieldStart);

            char hex1[128] = {0};
            char hex2[128] = {0};
            size_t start = diffOffset > 8 ? diffOffset - 8 : 0;
            size_t len = 16;
            if (start + len > minSize) len = minSize - start;
            for (size_t k = 0; k < len; k++)
            {
                snprintf(hex1 + k * 3, 4, "%02X ", d1[start + k]);
                snprintf(hex2 + k * 3, 4, "%02X ", d2[start + k]);
            }

            char fieldDump[2048] = {0};
            size_t dlen = 0;
            size_t minTrack = diffOffset > 100 ? diffOffset - 100 : 0;
            size_t maxTrack = diffOffset + 100;
            for (int k = 0; k < s_trackedFieldCount; k++)
            {
                if (s_trackedFields[k].offset >= minTrack && s_trackedFields[k].offset <= maxTrack)
                {
                    dlen += snprintf(fieldDump + dlen, sizeof(fieldDump) - dlen,
                                     "  [%zu] %s\n", s_trackedFields[k].offset, s_trackedFields[k].name);
                }
            }

            snprintf(outResult, outResultSize,
                     "DIVERGENCE at byte %zu [field '%s' +%zu]: run1=0x%02X, run2=0x%02X (CS1=0x%08X, CS2=0x%08X, %d frames)\n  run1: %s\n  run2: %s\nField map:\n%s",
                     diffOffset, fieldName, diffOffset - fieldStart, d1[diffOffset], d2[diffOffset], checksum1, checksum2, framesToTest,
                     hex1, hex2, fieldDump);
        }
        else
        {
            snprintf(outResult, outResultSize,
                     "DIVERGENCE: Size mismatch (run1=%zu, run2=%zu, %d frames)",
                     size1, size2, framesToTest);
        }
        return false;
    }

    snprintf(outResult, outResultSize,
             "PERFECT! %d-frame rollback resimulation is 100%% deterministic (Checksum: 0x%08X, State size: %zu B)",
             framesToTest, checksum1, size1);
    return true;
}
