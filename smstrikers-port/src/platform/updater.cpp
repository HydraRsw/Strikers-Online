// In-game updates: see include/port/updater.h.
//
// The work is done by updater.ps1, which ships beside the game. This file only runs it: hidden and
// in the background to ask whether there is a newer release (it answers in update-check.txt), and
// in a window of its own to install one. The game never downloads or writes its own files.
#include "port/updater.h"

#include <cstdio>
#include <cstring>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_timer.h>

namespace
{

char s_base[1024];
char s_version[64];
bool s_baseRead = false;

PortUpdateState s_state = PORT_UPDATE_IDLE;
char s_latest[64];
char s_page[512];
char s_error[256];
char s_notes[4096];
Uint64 s_startedMs = 0;

const char* base()
{
    if (!s_baseRead)
    {
        s_baseRead = true;
        const char* b = SDL_GetBasePath();
        std::snprintf(s_base, sizeof s_base, "%s", b != nullptr ? b : "");

        // version.txt: the release's tag, one line.
        char path[1200];
        std::snprintf(path, sizeof path, "%sversion.txt", s_base);
        if (FILE* f = std::fopen(path, "r"))
        {
            if (std::fgets(s_version, sizeof s_version, f) == nullptr)
                s_version[0] = '\0';
            std::fclose(f);
            s_version[std::strcspn(s_version, "\r\n")] = '\0';
        }
    }
    return s_base;
}

bool file_exists(const char* path)
{
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
        return false;
    std::fclose(f);
    return true;
}

void path_of(char* out, size_t cap, const char* name) { std::snprintf(out, cap, "%s%s", base(), name); }

// Reads update-check.txt: key=value lines.
void read_answer()
{
    char path[1200];
    path_of(path, sizeof path, "update-check.txt");
    s_latest[0] = s_page[0] = s_error[0] = '\0';
    FILE* f = std::fopen(path, "r");
    if (f == nullptr)
    {
        s_state = PORT_UPDATE_ERROR;
        std::snprintf(s_error, sizeof s_error, "The update check gave no answer.");
        return;
    }
    char line[1024];
    while (std::fgets(line, sizeof line, f) != nullptr)
    {
        line[std::strcspn(line, "\r\n")] = '\0';
        char* eq = std::strchr(line, '=');
        if (eq == nullptr)
            continue;
        *eq = '\0';
        const char* v = eq + 1;
        if (std::strcmp(line, "latest") == 0)     std::snprintf(s_latest, sizeof s_latest, "%s", v);
        else if (std::strcmp(line, "page") == 0)  std::snprintf(s_page, sizeof s_page, "%s", v);
        else if (std::strcmp(line, "error") == 0) std::snprintf(s_error, sizeof s_error, "%s", v);
    }
    std::fclose(f);

    s_notes[0] = '\0';
    path_of(path, sizeof path, "update-check-notes.txt");
    if (FILE* n = std::fopen(path, "rb"))
    {
        const size_t got = std::fread(s_notes, 1, sizeof s_notes - 1, n);
        s_notes[got] = '\0';
        std::fclose(n);
    }

    if (s_latest[0] == '\0')
    {
        s_state = PORT_UPDATE_ERROR;
        if (s_error[0] == '\0')
            std::snprintf(s_error, sizeof s_error, "GitHub did not say which version is the newest.");
        return;
    }
    s_state = std::strcmp(s_latest, s_version) == 0 ? PORT_UPDATE_UP_TO_DATE : PORT_UPDATE_AVAILABLE;
}

}   // namespace

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{

HANDLE s_check = nullptr;

// Runs powershell on updater.ps1 with the given arguments.
bool run_updater(const char* args, bool hidden, HANDLE* outProcess)
{
    char script[1200];
    path_of(script, sizeof script, "updater.ps1");
    char cmd[2600];
    std::snprintf(cmd, sizeof cmd,
                  "powershell.exe -NoProfile -ExecutionPolicy Bypass%s -File \"%s\" %s",
                  hidden ? " -NonInteractive -WindowStyle Hidden" : "", script, args);

    wchar_t wcmd[2600];
    wchar_t wdir[1100];
    if (MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 2600) == 0 ||
        MultiByteToWideChar(CP_UTF8, 0, base(), -1, wdir, 1100) == 0)
        return false;

    STARTUPINFOW si;
    std::memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof pi);
    const DWORD flags = hidden ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE;
    if (!CreateProcessW(nullptr, wcmd, nullptr, nullptr, FALSE, flags, nullptr, wdir[0] ? wdir : nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    if (outProcess != nullptr)
        *outProcess = pi.hProcess;
    else
        CloseHandle(pi.hProcess);
    return true;
}

}   // namespace

extern "C" void PortUpdaterCheck(void)
{
    char script[1200];
    path_of(script, sizeof script, "updater.ps1");
    if (!file_exists(script))
    {
        s_state = PORT_UPDATE_UNAVAILABLE;
        return;
    }
    if (s_state == PORT_UPDATE_CHECKING)
        return;

    char out[1200];
    path_of(out, sizeof out, "update-check.txt");
    std::remove(out);
    char args[1400];
    std::snprintf(args, sizeof args, "-CheckOnly -Out \"%s\"", out);
    if (!run_updater(args, true, &s_check))
    {
        s_state = PORT_UPDATE_ERROR;
        std::snprintf(s_error, sizeof s_error, "Could not start PowerShell to check for updates.");
        return;
    }
    s_state = PORT_UPDATE_CHECKING;
    s_startedMs = SDL_GetTicks();
}

extern "C" PortUpdateState PortUpdaterPoll(void)
{
    if (s_state != PORT_UPDATE_CHECKING || s_check == nullptr)
        return s_state;
    if (WaitForSingleObject(s_check, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(s_check);
        s_check = nullptr;
        read_answer();
    }
    else if (SDL_GetTicks() - s_startedMs > 30000)
    {
        TerminateProcess(s_check, 1);
        CloseHandle(s_check);
        s_check = nullptr;
        s_state = PORT_UPDATE_ERROR;
        std::snprintf(s_error, sizeof s_error, "GitHub did not answer in time.");
    }
    return s_state;
}

extern "C" int PortUpdaterLaunch(void)
{
    char script[1200];
    path_of(script, sizeof script, "updater.ps1");
    if (!file_exists(script))
        return 0;
    return run_updater("-Yes -WaitForGame -Launch", false, nullptr) ? 1 : 0;
}

#else   // no updater outside Windows

extern "C" void PortUpdaterCheck(void) { s_state = PORT_UPDATE_UNAVAILABLE; }
extern "C" PortUpdateState PortUpdaterPoll(void) { return s_state; }
extern "C" int PortUpdaterLaunch(void) { return 0; }

#endif

extern "C" const char* PortUpdaterVersion(void)
{
    base();
    return s_version[0] != '\0' ? s_version : "development build";
}
extern "C" const char* PortUpdaterLatest(void) { return s_latest; }
extern "C" const char* PortUpdaterNotes(void) { return s_notes; }
extern "C" const char* PortUpdaterPage(void) { return s_page; }
extern "C" const char* PortUpdaterError(void) { return s_error; }
