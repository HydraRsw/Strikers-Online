#ifndef PORT_UPDATER_H
#define PORT_UPDATER_H

// PORT: in-game updates. The release carries updater.ps1 (and UPDATE.bat for it), which fetches
// the newest GitHub release and installs it over the game's folder; this asks it, in the background,
// whether there is one, and starts it. Windows only; elsewhere nothing is available.

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PortUpdateState
{
    PORT_UPDATE_UNAVAILABLE = -1,   // no updater.ps1 beside the game (a development build)
    PORT_UPDATE_IDLE = 0,
    PORT_UPDATE_CHECKING,
    PORT_UPDATE_UP_TO_DATE,
    PORT_UPDATE_AVAILABLE,
    PORT_UPDATE_ERROR,
} PortUpdateState;

// This build's version (version.txt beside the game), or "development build".
const char* PortUpdaterVersion(void);

// Ask GitHub, in the background, for the newest release. Ignored while a check is running.
void PortUpdaterCheck(void);

// Where the check is. Call once per frame; it notices the check finishing.
PortUpdateState PortUpdaterPoll(void);

const char* PortUpdaterLatest(void);   // the newest version's tag, once known
const char* PortUpdaterNotes(void);    // its release notes
const char* PortUpdaterPage(void);     // its GitHub page
const char* PortUpdaterError(void);    // why the check failed

// Start the updater in its own window: it waits for the game to close, installs the newest
// release and starts the game again. Returns 0 if it could not be started. The caller quits.
int PortUpdaterLaunch(void);

#ifdef __cplusplus
}
#endif

#endif
