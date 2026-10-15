#include "Game/Pad/FlickDetection.h"

#include "Game/AI/AIPad.h"

namespace
{

static char FLICK_NONE = 0x20;

StaticRing<char> history[4];
char historyStorage[4][16] = { };

} // namespace

/**
 * Offset/Address/Size: 0x174 | 0x80192264 | size: 0x68
 */
void FlickDetection::Initialize()
{
    for (int i = 0; i < 4; i++)
    {
        history[i].mStorage = &historyStorage[i][0];
        history[i].mCapacity = 16;
        history[i].mSize = 0;
        history[i].mEnd = 0;
    }
}

/**
 * Offset/Address/Size: 0x0 | 0x801920F0 | size: 0x174
 */
void FlickDetection::Update()
{
    for (int i = 0; i < 4; i++)
    {
        cAIPad& pad = AIPadManager::mAIPads[i];
        StaticRing<char>& ring = history[i];

        float magnitude = pad.GetCStickMovementStickMagnitude();

        if (magnitude > 0.6f)
        {
            u16 direction = pad.GetCStickMovementStickDirection();

            char flickValue = 0x72;
            if (direction >= 0x2000 && direction < 0x6000)
            {
                flickValue = 0x75;
            }
            else if (direction >= 0x6000 && direction < 0xA000)
            {
                flickValue = 0x6c;
            }
            else if (direction >= 0xA000 && direction < 0xE000)
            {
                flickValue = 0x64;
            }

            ring.mStorage[ring.mEnd] = flickValue;
            ring.mEnd = (ring.mEnd + 1) % ring.mCapacity;

            ring.mSize += 1;
            if (ring.mSize > ring.mCapacity)
            {
                ring.mSize = ring.mCapacity;
            }
        }
        else
        {
            ring.mStorage[ring.mEnd] = FLICK_NONE;
            ring.mEnd = (ring.mEnd + 1) % ring.mCapacity;

            ring.mSize += 1;
            if (ring.mSize > ring.mCapacity)
            {
                ring.mSize = ring.mCapacity;
            }
        }
    }
}

/* PORT: Netplay accessors — allow rollback to save/restore flick detection state. */
#include <cstring>

extern "C" char netplay_flick_get_history(int pad, char* out_buf, int* out_size, int* out_end)
{
    if (pad < 0 || pad >= 4) return 0;
    memcpy(out_buf, historyStorage[pad], 16);
    *out_size = history[pad].mSize;
    *out_end  = history[pad].mEnd;
    return 1;
}

extern "C" void netplay_flick_set_history(int pad, const char* in_buf, int in_size, int in_end)
{
    if (pad < 0 || pad >= 4) return;
    memcpy(historyStorage[pad], in_buf, 16);
    history[pad].mSize = in_size;
    history[pad].mEnd  = in_end;
}
