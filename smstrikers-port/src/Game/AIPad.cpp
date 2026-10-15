#include "Game/AI/AIPad.h"
#include "Game/Camera/CameraMan.h"

static f32 g_fMovementDeadZone = 0.3f;
static f32 g_fCStickDeadZone = 0.5f;

cAIPad AIPadManager::mAIPads[4];

// PORT: the stick is turned by the camera's angle (m_aJoystickRemap), which the
// camera sets once per rendered frame.  Online, each game frame instead uses the
// angle the stick's owner saw when the input was made, carried in the input
// itself, so both PCs turn every stick the same way.
static u16 s_portRemap[4];
static bool s_portRemapOn = false;

extern "C" void PortSetPadRemap(int port, unsigned short remap)
{
    if (port >= 0 && port < 4)
        s_portRemap[port] = remap;
}

extern "C" unsigned short PortGetPadRemap(int port)
{
    return (port >= 0 && port < 4) ? s_portRemap[port] : 0;
}

extern "C" void PortSetPadRemapActive(int on)
{
    s_portRemapOn = on != 0;
}

extern "C" unsigned short PortCameraJoystickRemap(void)
{
    return cCameraManager::m_aJoystickRemap;
}

static inline u16 RemapFor(const cAIPad* pad)
{
    if (s_portRemapOn)
    {
        const long i = (long)(pad - AIPadManager::mAIPads);
        if (i >= 0 && i < 4)
            return s_portRemap[i];
    }
    return cCameraManager::m_aJoystickRemap;
}

/**
 * Offset/Address/Size: 0xFC | 0x8000582C | size: 0x20
 */
f32 cAIPad::GetMovementStickMagnitude()
{
    f32 mag = m_pGlobalPad->m_polarAnalogLeft.r;
    f32 dz = g_fMovementDeadZone;
    return (mag - dz) / (1.0f - dz);
}

/**
 * Offset/Address/Size: 0xDC | 0x8000580C | size: 0x20
 */
u16 cAIPad::GetMovementStickDirection()
{
    return (u16)(RemapFor(this) - 0x4000) + m_pGlobalPad->m_polarAnalogLeft.a;
}

/**
 * Offset/Address/Size: 0xBC | 0x800057EC | size: 0x20
 */
f32 cAIPad::GetCStickMovementStickMagnitude()
{
    f32 mag = m_pGlobalPad->m_polarAnalogRight.r;
    f32 dz = g_fCStickDeadZone;
    return (mag - dz) / (1.0f - dz);
}

/**
 * Offset/Address/Size: 0x9C | 0x800057CC | size: 0x20
 */
u16 cAIPad::GetCStickMovementStickDirection()
{
    return (u16)(RemapFor(this) - 0x4000) + m_pGlobalPad->m_polarAnalogRight.a;
}

u16 cAIPad::GetMovementStickDirectionNoRemap()
{
    return m_pGlobalPad->m_polarAnalogLeft.a;
}

/**
 * Offset/Address/Size: 0x54 | 0x80005784 | size: 0x48
 */
bool cAIPad::IsTurboPressed()
{
    return (m_pGlobalPad->GetPressure(0x14, true) > 0.2f);
}

/**
 * Offset/Address/Size: 0x0 | 0x80005730 | size: 0x54
 */
void AIPadManager::Startup()
{
    int i;
    for (i = 0; i < 4; ++i)
    {
        mAIPads[i].m_pGlobalPad = cPadManager::GetPad(i);
    }
}
