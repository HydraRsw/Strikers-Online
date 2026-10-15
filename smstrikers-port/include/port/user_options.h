#ifndef PORT_USER_OPTIONS_H
#define PORT_USER_OPTIONS_H

// PORT: the game's own options (what its Options screen and memory card hold), read and set from
// the F1 menu: the sound volumes, and the match rules an online host picks for both players.
// Defined in src/Game/GameInfo.cpp.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortMatchRules
{
    int gameTimeSec;     // 120, 180, 240, 300, 600 or 900
    int skill;           // 1 rookie .. 4 legend (the computer-controlled teammates)
    int powerUps;        // items on the field
    int shoot2Score;     // Super Strikes
    int bowser;          // Bowser attacks
    int infiniteItems;   // cheat
    int stunnedGoalies;  // cheat ("glass-jaw goalies")
    int customItems;     // cheat: 0 off, 1 explosive, 2 freezing, 3 shells, 4 giant, 5 enhancement
} PortMatchRules;

// 0 when the game has not set up its options yet.
int PortGetMatchRules(PortMatchRules* out);
void PortSetMatchRules(const PortMatchRules* in);

// The game's three volumes, 0..10 each, as its Options > Audio screen shows them.
int PortGetGameVolumes(int* music, int* sfx, int* voice);
void PortSetGameVolumes(int music, int sfx, int voice);

#ifdef __cplusplus
}
#endif

#endif
