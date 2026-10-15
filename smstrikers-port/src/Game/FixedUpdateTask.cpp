#include "Game/FixedUpdateTask.h"

#include "Game/Ball.h"
#include "Game/BasicStadium.h"
#include "Game/CharacterTemplate.h"
#include "Game/Field.h"
#include "Game/FlickDetection.h"
#include "Game/Game.h"
#include "Game/Physics/Physics.h"
#include "Game/Physics/PhysicsAIBall.h"
#include "Game/Physics/PhysicsFakeBall.h"
#include "Game/Render/NetMesh.h"
#include "Game/Render/Presentation.h"
#include "Game/Render/SidelineExplodable.h"
#include "Game/ReplayManager.h"
#include "Game/Team.h"
#include "NL/platpad.h"
#include "Game/Sys/clock.h"
#include "Game/WorldManager.h"
#include "Game/Sys/eventman.h"
extern EventManager* g_pEventManager;

float g_fFixedUpdateTick = 0.02f;
extern PhysicsWorld* g_PhysicsWorld;

bool g_bRunSimAndRenderInLockStep;
float g_fSimulationTick = g_fFixedUpdateTick;

float FixedUpdateTask::mAccumulatedDeltaT;
float FixedUpdateTask::mSimulationTime;
float FixedUpdateTask::mfFrameLockTime;
float FixedUpdateTask::mTimeScale = 1.0f;

extern "C" float* PortSimTimeScalePtr(void)
{
    return &FixedUpdateTask::mTimeScale;
}

/**
 * Offset/Address/Size: 0x2D8 | 0x8016E608 | size: 0x30
 */
FixedUpdateTask::FixedUpdateTask()
{
    mAccumulatedDeltaT = g_fFixedUpdateTick;
    mSimulationTime = 0.f;
    mfFrameLockTime = 0.f;
}

/**
 * Offset/Address/Size: 0x2CC | 0x8016E5FC | size: 0xC
 */
const char* FixedUpdateTask::GetName()
{
    return "Game Fixed Update";
}

/**
 * Offset/Address/Size: 0x2C4 | 0x8016E5F4 | size: 0x8
 */
float FixedUpdateTask::GetPhysicsUpdateTick()
{
    return g_fSimulationTick;
}

/**
 * Offset/Address/Size: 0x280 | 0x8016E5B0 | size: 0x44
 */
void FixedUpdateTask::DecrementFrameLock(float fDeltaT)
{
    mfFrameLockTime -= fDeltaT;
    if (mfFrameLockTime < 0.f)
    {
        nlTaskManager::SetNextState(2);
        mfFrameLockTime = 0.f;
    }
}

#include "port/netplay/netplay.h"
#include "port/netplay/lobby.h"
#include "port/netplay/savestate.h"
#include <cstdlib>

extern "C" void PortRequestQuit(void);
extern "C" void PortPadSetFixedHoldTimesStepped(int on);
extern "C" void PortPadStepFixedHoldTimes(float dt);

static void CheckInitNetplay()
{
    static bool s_checked = false;
    if (s_checked) return;
    s_checked = true;

    const char* enable = std::getenv("STRIKERS_NETPLAY");
    if (!enable) enable = std::getenv("STRIKERS_NETPLAY_ENABLE");
    if (!enable) enable = std::getenv("STRIKERS_ENABLE");
    if (!enable || atoi(enable) == 0) return;

    const char* peer = std::getenv("STRIKERS_NETPLAY_PEER");
    if (!peer || !*peer) peer = std::getenv("STRIKERS_PEER");
    if (!peer || !*peer)
    {
        fprintf(stderr, "[netplay] Netplay is enabled but no peer address was set!\n");
        fprintf(stderr, "[netplay] Please set peer = <ip:port> in strikers.ini\n");
        return;
    }

    const char* portStr  = std::getenv("STRIKERS_NETPLAY_PORT");
    if (!portStr) portStr = std::getenv("STRIKERS_PORT");

    const char* slotStr  = std::getenv("STRIKERS_NETPLAY_SLOT");
    if (!slotStr) slotStr = std::getenv("STRIKERS_SLOT");

    const char* delayStr = std::getenv("STRIKERS_NETPLAY_DELAY");
    if (!delayStr) delayStr = std::getenv("STRIKERS_DELAY");

    u16 localPort  = portStr ? static_cast<u16>(atoi(portStr)) : 7100;
    int localSlot  = slotStr ? atoi(slotStr) : 0;
    int inputDelay = delayStr ? atoi(delayStr) : 2;

    fprintf(stderr, "[netplay] Initializing session: localPort=%d, peer=%s, slot=%d, delay=%d\n",
            localPort, peer, localSlot, inputDelay);

    NetplayManager::Instance().Connect(localPort, peer, localSlot, inputDelay);
}

/**
 * Offset/Address/Size: 0x0 | 0x8016E330 | size: 0x280
 */
void FixedUpdateTask::Run(float dt)
{
    CheckInitNetplay();
    NetplayLobby::Instance().Update();
    NetplayManager::Instance().Update();
    bool netplayTouchedPads = false;
    // Hold times of the fixed-update pad follow game frames for the whole online session.
    PortPadSetFixedHoldTimesStepped(NetplayManager::Instance().IsActive() && !PortNetplayDiag(4) ? 1 : 0);

    // PORT: netplay must know when gameplay resumes (after a goal, replay or pause) so both
    // games can resync, and outside gameplay the guest's controller drives its port too.
    {
        static u32 s_prevTaskState = 0;
        const u32 curTaskState = nlTaskManager::m_pInstance->m_CurrState;
        NetplayManager& np = NetplayManager::Instance();
        if (np.IsActive())
        {
            if (curTaskState == 2 && s_prevTaskState != 2)
                np.OnEnterGameplay();
            if (curTaskState != 2)
                np.MirrorLocalPadForMenus();
        }
        s_prevTaskState = curTaskState;
    }

    if (std::getenv("STRIKERS_TEST_ROLLBACK") != nullptr)
    {
        Presentation::Instance().mSkipPressed = true;
        static FILE* s_diag = nullptr;
        if (!s_diag) s_diag = fopen("rollback_diag.log", "w");
        if (s_diag)
        {
            fprintf(s_diag, "FixedUpdateTask::Run: state=%u\n",
                    nlTaskManager::m_pInstance ? nlTaskManager::m_pInstance->m_CurrState : 0);
            fflush(s_diag);
        }
    }

    if (nlTaskManager::m_pInstance->m_CurrState == 2)
    {
        static int s_autoTestFrames = 0;
        static bool s_autoTestDone = false;
        if (!s_autoTestDone && std::getenv("STRIKERS_TEST_ROLLBACK") != nullptr)
        {
            s_autoTestFrames++;
            if (s_autoTestFrames >= 10)
            {
                s_autoTestDone = true;
                char testResult5[4096];
                char testResult10[4096];
                fprintf(stderr, "\n============================================================\n");
                fprintf(stderr, "[rollback-test] RUNNING AUTOMATED ROLLBACK SELF-TESTS...\n");
                bool ok5 = NetplayTestRollbackDeterminism(5, testResult5, sizeof(testResult5));
                fprintf(stderr, "[rollback-test] 5-frame test: %s\n", testResult5);
                bool ok10 = NetplayTestRollbackDeterminism(10, testResult10, sizeof(testResult10));
                fprintf(stderr, "[rollback-test] 10-frame test: %s\n", testResult10);
                fprintf(stderr, "============================================================\n\n");
                fflush(stderr);

                FILE* fResult = fopen("rollback_test_result.txt", "w");
                if (fResult)
                {
                    fprintf(fResult, "5-FRAME TEST: %s\n10-FRAME TEST: %s\n", testResult5, testResult10);
                    fclose(fResult);
                }

                if (std::getenv("STRIKERS_TEST_ROLLBACK_EXIT") != nullptr)
                {
                    PortRequestQuit();
                }
            }
        }

        float simulationTick;

        mAccumulatedDeltaT += dt * mTimeScale;

        NetplayManager& netplay = NetplayManager::Instance();
        const bool netplayActive = netplay.IsActive();
        bool tickedThisFrame = false;
        if (netplayActive)
        {
            netplayTouchedPads = true;
            netplay.CaptureLocalInput();
            netplay.BeginSimRng();
        }

        while (g_bRunSimAndRenderInLockStep || mAccumulatedDeltaT >= g_fFixedUpdateTick)
        {
            u32 netFrame = 0;
            if (netplayActive)
            {
                netFrame = netplay.GetSimFrame();
                if (!netplay.PreUpdate(netFrame))
                {
                    /* Waiting for the peer, or slowing down so it can catch up.
                       Drop this tick's time instead of banking it; banked time
                       would make us burst ahead again as soon as we resume. */
                    mAccumulatedDeltaT -= g_fFixedUpdateTick;
                    if (mAccumulatedDeltaT < 0.0f)
                        mAccumulatedDeltaT = 0.0f;
                    if (mAccumulatedDeltaT > g_fFixedUpdateTick)
                        mAccumulatedDeltaT = g_fFixedUpdateTick;
                    break;
                }
            }

            UseFixedUpdatePad();
            if (netplayActive && !PortNetplayDiag(4))
                PortPadStepFixedHoldTimes(g_fFixedUpdateTick);   // see platpad.cpp

            UpdatePlatPad(simulationTick = g_fFixedUpdateTick);
            cPadManager::Update(simulationTick);
            FlickDetection::Update();

            mAccumulatedDeltaT -= g_fFixedUpdateTick;
            if (g_bRunSimAndRenderInLockStep)
            {
                mAccumulatedDeltaT = 0.0f;
            }

            // PORT: the goalie's and AI's ball-flight prediction is cached between
            // calls, and render-rate code (cameras) also extends that cache, at a
            // different moment on each PC.  Online every game frame predicts afresh.
            if (netplayActive)
                FakeBallWorld::InvalidateBallCache();
            CallFixedUpdateTasks();
            if (netplayActive)
            {
                if (!PortNetplayDiag(0))
                    ClockManager::Update(g_fFixedUpdateTick);       // see ClockUpdateTask in main.cpp
                if (!PortNetplayDiag(1))
                    WorldManager::UpdateWorld(g_fFixedUpdateTick);  // see WorldUpdateTask
                // PORT: the game's events (ball touched, pass coming, shot...) are handled
                // once per rendered frame by DispatchEventsTask, after however many game
                // frames that rendered frame ran, and outside gameplay's random numbers.
                // An AI player reacting to one then rolled its random timing from the
                // render-side generator, different on each PC.  Online, every game frame
                // handles its own events right away.
                g_pEventManager->DispatchEvents();
            }
            tickedThisFrame = true;

            if (netplayActive)
            {
                netplay.PostUpdate(netFrame);
                netplay.AdvanceSimFrame();

                // This frame ended gameplay (goal, end of half...): the state changes at the
                // next frame, so simulate no further, and never roll back across it.
                if (nlTaskManager::m_pInstance->m_PendingState != 2)
                {
                    netplay.OnLeaveGameplay(netFrame);
                    break;
                }
            }

            if (g_bRunSimAndRenderInLockStep)
            {
                break;
            }
        }

        if (netplayActive)
            netplay.EndSimRng();
        if (netplayActive && !tickedThisFrame)
            netplay.ReapplyLastInputs();
    }

    UseDefaultPad();
    UpdatePlatPad(dt);
    cPadManager::Update(dt);
    // PORT: the flick history is gameplay state (and in the netplay save state); updating it
    // once more per rendered frame made it depend on each PC's frame rate. During an online
    // session only game frames update it, also through goal replays and menus, which each PC
    // shows for its own length of time.
    if (!NetplayManager::Instance().IsActive() || PortNetplayDiag(3))
        FlickDetection::Update();

    /* Netplay overrides both pads during a tick; take them off so next
       frame's CaptureLocalInput reads this PC's real controller. */
    if (netplayTouchedPads)
        NetplayManager::Instance().ReleaseVirtualPads();
}

void FixedUpdateTask::AIUpdateTask(float fDeltaT)
{
    g_pGame->PreUpdate(fDeltaT);
    g_pGame->Update(fDeltaT);
}

void FixedUpdateTask::PrePhysicsAITask(float fDeltaT)
{
    int i;
    for (i = 0; i < 10; i++)
    {
        g_pCharacters[i]->PrePhysicsUpdate(fDeltaT);
    }
}

void FixedUpdateTask::PostPhysicsAITask(float fDeltaT)
{
    int i;
    for (i = 0; i < 10; i++)
    {
        g_pCharacters[i]->PostPhysicsUpdate();
    }
    g_pBall->PostPhysicsUpdate(fDeltaT);
}

void FixedUpdateTask::CallFixedUpdateTasks()
{
    mSimulationTime += g_fSimulationTick;
    AIUpdateTask(g_fSimulationTick);
    BasicStadium::GetCurrentStadium()->mpNPCManager->UpdateAINPCs(g_fSimulationTick);
    PrePhysicsAITask(g_fSimulationTick);
    PhysicsUpdate(g_PhysicsWorld, g_fSimulationTick);
    PostPhysicsAITask(g_fSimulationTick);

    if (NetMesh::s_bAnimatedNetMeshEnabled)
    {
        bool i = true;
        float goalieX = (float)fabs(g_pTeams[0]->GetGoalie()->m_v3Position.x);
        if (goalieX > cField::GetGoalLineX(1U))
        {
        }
        else
        {
            goalieX = (float)fabs(g_pTeams[1]->GetGoalie()->m_v3Position.x);
            if (goalieX > cField::GetGoalLineX(1U))
            {
            }
            else
            {
                i = false;
            }
        }

        cBall* pBall = g_pBall;
        PhysicsAIBall* pPhysicsBall = pBall->m_pPhysicsBall;
        NetMesh::spPositiveXNetMesh->Update(g_fSimulationTick, pBall->m_v3Position, pBall->m_v3PrevPosition, i, pPhysicsBall);
        pPhysicsBall = (pBall = g_pBall)->m_pPhysicsBall;
        NetMesh::spNegativeXNetMesh->Update(g_fSimulationTick, pBall->m_v3Position, pBall->m_v3PrevPosition, i, pPhysicsBall);
    }

    SidelineExplodableManager::Update(g_fSimulationTick);
    ReplayManager::Instance()->GrabSnapshot();
}
