// The debug overlay and menu. See include/port/overlay.h for why it is ImGui, where it is drawn,
// and why game state is pushed here rather than pulled.

#include "port/overlay.h"
#include "port/aspect.h"
#include "port/control.h"
#include "port/audio.h"
#include "port/benchmark.h"
#include "port/config.h"
#include "port/framerate.h"
#include "port/host.h"
#include "port/input.h"
#include "port/launch.h"
#include "port/texture_packs.h"
#include "port/netplay/netplay.h"
#include "port/netplay/lobby.h"
#include "port/netplay/savestate.h"
#include "port/netplay/transport.h"
#include "port/user_options.h"
#include "port/updater.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cfloat>

#include <imgui.h>

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/pad.h>
#include <SDL3/SDL.h>

// Aurora's pool and texture accounting. Both are measurements of Aurora's own allocations that
// nothing outside Aurora could otherwise see, the staging pools are 378 MiB of this process and are
// not visible from the game side at all.
extern "C" {
void aurora_gfx_pool_stats(uint32_t* peakBytes, uint32_t* reservedBytes, size_t count);
void aurora_gfx_texture_stats(uint64_t* srcBytes, uint64_t* uploadedBytes, uint64_t* count);
void aurora_capture_frame(const char* path);

// The graphics resource arena, from glxMemory.cpp (rw_glxmemory_report).
int PortGfxArenaStats(PortGfxArena* out);

// STRIKERS_UNLOCK_ALL, settable (debughooks.cpp).
void PortSetUnlockAll(int on);
int  PortGetUnlockAll(void);
}

// The game's own debug switches. Every one of these shipped in the retail build and is set nowhere
// on the disc.
extern bool g_bRenderWorld;                 // GameRenderTask.cpp
extern bool g_bEnableGamecubePadMonkey;     // PadActions.cpp, random input
extern bool g_bRunSimAndRenderInLockStep;   // FixedUpdateTask.cpp
extern bool g_bFrameStatsOnScreen;          // BeginFrameTask.cpp
extern bool g_bFrameSmiler;                 // BeginFrameTask.cpp
extern bool g_bCoPlanarPerObject;           // DrawableModel.cpp
extern bool g_bDrawSafeFrame;               // BeginFrameTask.cpp  (was static)
extern int  g_nGridDisplaySpacing;          // BeginFrameTask.cpp  (was static)
extern unsigned char sbNoBallPickups;       // Player.cpp          (was static)
extern unsigned char g_bShadowBlobs;        // RenderShadow.cpp    (was static)
extern unsigned char g_bPreview;            // RenderShadow.cpp    (was static)
extern unsigned char g_bShadowBounds;       // RenderShadow.cpp    (was static)
extern unsigned char g_bClipToFrustum;      // world.cpp           (was static)
extern unsigned char g_bDrawBoundingSphere; // world.cpp           (was static)
extern unsigned char g_bFreezeFrustum;      // world.cpp           (was static)
extern unsigned char g_bDrawCullingInfo;    // world.cpp           (was static)
extern unsigned char g_bWhiteDiffuse;       // GameRenderTask.cpp  (was static)
extern unsigned char g_TexDetail;           // GameRenderTask.cpp  (was static)
extern unsigned char g_TexShadow;           // GameRenderTask.cpp  (was static)
extern unsigned char g_TexSelfIllum;        // GameRenderTask.cpp  (was static)
extern unsigned char g_TexGloss;            // GameRenderTask.cpp  (was static)
extern unsigned char g_bTexelDensity;       // GameRenderTask.cpp  (was static)
extern unsigned char g_bMemoryOnScreen;     // GameRenderTask.cpp  (was static)
extern bool g_bShadowVolumes;               // DrawableModel.cpp   (was static)
extern unsigned char g_bDrawBoundingBoxes;  // DrawableModel.cpp   (was static)
extern bool g_bEnableDrawableModel;         // DrawableModel.cpp   (was static)
extern bool g_bDrawPlanarShadows;           // DrawableModel.cpp   (was static)
extern bool g_bBallGlow;                    // DrawableModel.cpp   (was static)
extern bool g_bEnableDrawableSkinModel;     // DrawableSkinModel.cpp (was static)
extern int  g_nShowBones;                   // DrawableCharacter.cpp (was static)
extern unsigned char g_hudVisible;          // OverlayHandlerHUD.cpp (was static)
extern bool sbDisableCollisionDetection;    // Physics.cpp         (was static)
extern bool g_bAllowLighting;               // glxSend.cpp         (was static)
extern bool g_bAllowSpecular;               // glxSend.cpp         (was static)
extern unsigned char sbUseCheckerTexture;   // DrawableNetMesh.cpp (was static)

// This one stays: FixedUpdateTask.cpp defines it `extern "C"` (see rw_fixedupdate_timescale), so C
// linkage here is what matches the definition.
extern "C" {
float* PortSimTimeScalePtr(void);           // FixedUpdateTask.cpp, rw_fixedupdate_timescale
}

namespace
{

int s_enabled;
int s_read;

// PORT: the menu's fonts (PortOverlayPreFrame); null means ImGui's built-in one.
ImFont* g_fontBody = nullptr;
ImFont* g_fontBold = nullptr;
ImFont* g_fontHead = nullptr;
ImFont* g_fontTitle = nullptr;
ImFont* g_fontHuge = nullptr;
float g_fontBasePx = 13.0f;   // the size they were made at (ImGui's own is 13)

bool keyboard_pref_active();   // the Controls page's "the keyboard works too"

char s_scene[64] = "-";
char s_stadium[64] = "-";
float s_clock;
int s_scoreHome, s_scoreAway;

bool s_menuOpen;
bool s_quit;
PortDebugMatch s_match;
PortDebugSession s_session;
int s_forcedStadium = -2;   // -2: not yet read from the environment

struct PadSnapshot
{
    int err;
    unsigned int buttons;
    int stickX, stickY, substickX, substickY, triggerL, triggerR;
};
PadSnapshot s_pad[4];

// Commands, menu -> game.

const int kCommandCap = 32;
PortDebugCommand s_commands[kCommandCap];
int s_commandHead, s_commandCount;

void cmd(int op, int a = 0, int b = 0, int c = 0, float f0 = 0.0f, float f1 = 0.0f,
         float f2 = 0.0f, float f3 = 0.0f, const char* s = nullptr)
{
    PortDebugCommand k;
    std::memset(&k, 0, sizeof k);
    k.op = op;
    k.a = a;
    k.b = b;
    k.c = c;
    k.f[0] = f0;
    k.f[1] = f1;
    k.f[2] = f2;
    k.f[3] = f3;
    if (s != nullptr)
    {
        std::strncpy(k.s, s, sizeof k.s - 1);
    }
    PortDebugPushCommand(&k);
}

// clang-format off
const char* const kGoalieStates[] = {   // Goalie.h eGoalieActionState
    "move", "move w/ball", "save setup", "save reposition", "save",
    "miss chip shot", "dive recover", "sts setup", "sts", "sts recover",
    "sts attack setup", "sts attack", "pass", "pass intercept", "pre-crouch",
    "pursue carrier", "pursue pounce", "loose setup", "loose catch",
    "loose pickup", "loose bouncing", "loose rolling", "loose desperate",
    "offplay", "snap ball", "grab ball",
};
const char* const kFielderStates[] = {  // AI/Fielder.h eFielderActionState, from 0
    "deke", "electrocution", "hit", "hit react", "idle turn",
    "late onetimer", "loose pass", "loose shot", "onetimer", "onetouch pass",
    "pass", "post whistle", "receive pass", "running", "running w/ball",
    "turbo", "turbo turn", "shot", "SHOOT TO SCORE", "slide attack",
    "slide react", "bomb react", "shell react", "banana react", "sts hit react",
    "squish react", "slide fail react", "wait",
};
const char* const kDesires[] = {        // AI/ScriptAction.h eFielderDesireState
    "need desire", "cut and break", "deke", "get in position", "get open",
    "hit", "intercept ball", "mark", "protect ball", "run to net",
    "run upfield", "run downfield", "run to location", "pass", "shoot",
    "slide attack", "support def", "support off", "use powerup",
    "windup pass", "windup shot", "wait for thought", "USER", "finish action",
    "onetimer", "post whistle", "receive (idle)", "receive (run)", "wait",
};
const char* const kRoles[] = { "striker", "winger", "midfield", "defence" };  // eRole
const char* const kPowerups[] = {       // AI/Powerups.h ePowerUpType
    "green shell", "red shell", "spiny shell", "freeze shell", "banana",
    "bob-omb", "chain chomp", "mushroom", "star",
};
const char* const kGameStates[] = {     // Game.h eGameState, from 0
    "pre-game", "kickoff", "post goal", "end game", "gameplay", "overtime",
};
const char* const kCameraTypes[] = {    // Camera/BaseCamera.h eCameraType
    "debug (orbit)", "follow character", "follow ball", "replay", "animated",
    "kickoff", "top down", "gameplay", "matrix effect", "goal",
    "shoot to score", "anim viewer", "face closeup",
};
const char* const kDifficulties[] = {   // GameTweaks.h eDifficultyID, from 0
    "braindead", "easy", "medium", "hard", "very hard", "superhuman", "human",
};
const char* const kSkills[] = {         // DB/UserOptions.h eSkillLevel
    "training", "rookie", "professional", "superstar", "legend",
};
const char* const kCustomPowerups[] = { // DB/UserOptions.h CustomPowerups
    "off", "explosive", "freezing", "shells", "giant", "enhancement",
};
const char* const kSituations[] = { "offense", "defense", "loose" };          // eSituation
const char* const kStyles[] = { "aggressive", "moderate", "passive" };        // eTeamStyle
const char* const kUrgency[] = { "low", "med", "high" };                      // eUrgency
const char* const kCards[] = { "yellow", "yellow x2", "RED" };                // ePenaltyCardStatus, from 0
const char* const kShotMeter[] = { "-", "charging", "released", "sts charging", "sts transition", "sts released" };
const char* const kTeams[] = {          // Team.h eTeamID
    "daisy", "donkeykong", "luigi", "mario", "peach", "waluigi", "wario", "yoshi", "mystery",
};
const char* const kSidekicks[] = { "toad", "koopa", "hammerbro", "birdo" };   // eSidekickID
const char* const kStadiums[] = {       // DB/BasicGameInfo.h eStadiumID
    "mario_stadium", "peach_toad", "dk_daisy", "wario_stadium",
    "yoshi_stadium", "super_stadium", "forbidden_dome",
};
const char* const kGameModes[] = {      // GameInfo.h eGameModes
    "friendly", "mushroom cup", "flower cup", "star cup", "bowser cup",
    "super mushroom", "super flower", "super star", "super bowser",
    "tournament", "demo",
};
// clang-format on

template <size_t N>
const char* name_of(const char* const (&table)[N], int v)
{
    return (v >= 0 && v < (int)N) ? table[v] : "?";
}

const char* task_state_name(int s)
{
    switch (s)
    {
    case 0x1: return "pause / menu overlay";
    case 0x2: return "match";
    case 0x4: return "front end";
    case 0x10: return "auto replay";
    case 0x100: return "presentation / NIS";
    case 0x20000: return "debug replay";
    case 0x80000: return "health warning";
    default: return "?";
    }
}

// The front end's SceneList, for the stacks.
const char* scene_name(int s)
{
    switch (s)
    {
    case 2: return "title";
    case 3: return "main menu";
    case 4: case 5: case 6: case 7: return "choose sides";
    case 8: return "choose captains";
    case 9: return "stadium select";
    case 13: case 14: return "choose cup";
    case 15: case 16: return "cup captains";
    case 17: case 20: case 23: return "standings";
    case 27: return "popup";
    case 28: return "trophy room";
    case 38: return "options";
    case 39: return "legal";
    case 43: return "loading";
    case 49: return "quick options";
    case 50: return "vs transition";
    case 51: return "health warning";
    case 53: return "intro movie";
    case 54: return "credits";
    case 57: return "pause";
    case 58: return "choose sides (in game)";
    case 61: return "post game";
    case 67: return "HUD";
    case 68: return "in-game text";
    case 70: case 71: return "summary";
    case 72: return "goal overlay";
    case 74: return "demo overlay";
    case 75: return "winner";
    default: return "";
    }
}

void copy_into(char* dst, size_t cap, const char* src)
{
    if (src == nullptr)
        src = "-";
    size_t i = 0;
    for (; src[i] != '\0' && i < cap - 1; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

// The pools, in the order aurora_gfx_pool_stats reports them.
const char* const kPoolNames[] = { "vertex", "uniform", "index", "storage", "texUpload" };
constexpr size_t kPoolCount = sizeof(kPoolNames) / sizeof(kPoolNames[0]);

// The only way the menu opens or closes. Detach the keyboard from the pad while the menu has it:
// Aurora reads the keyboard itself through the bindings input.cpp installs, so without this every
// keystroke aimed at a text field also runs the player.
void set_menu_open(bool open)
{
    s_menuOpen = open;
    PADSetKeyboardActive(0, (!s_menuOpen && keyboard_pref_active()) ? TRUE : FALSE);
}

// A checkbox over a u8 flag. The game's switches are a mix of bool and u8, and ImGui wants a bool.
bool check_u8(const char* label, unsigned char* flag)
{
    bool v = *flag != 0;
    if (ImGui::Checkbox(label, &v))
    {
        *flag = v ? 1 : 0;
        return true;
    }
    return false;
}

// A checkbox that sends a command rather than writing a variable, for state that lives in the game.
bool check_cmd(const char* label, int current, int op, int a)
{
    bool v = current != 0;
    if (ImGui::Checkbox(label, &v))
    {
        cmd(op, a, v ? 1 : 0);
        return true;
    }
    return false;
}

void help(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// setenv is POSIX; Windows spells it _putenv_s, and clears with "".
void set_env(const char* var, const char* value)
{
#if defined(_WIN32)
    _putenv_s(var, value != nullptr ? value : "");
#else
    if (value != nullptr)
        setenv(var, value, 1);
    else
        unsetenv(var);
#endif
}

SDL_Window* main_window()
{
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* w = (windows != nullptr && count > 0) ? windows[0] : nullptr;
    SDL_free(windows);
    return w;
}

bool is_fullscreen()
{
    SDL_Window* w = main_window();
    return w != nullptr && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
}

void set_fullscreen(bool on)
{
    SDL_Window* w = main_window();
    if (w != nullptr)
        SDL_SetWindowFullscreen(w, on);
}

// Environment switches the generated tree reads on every use, so flipping them here takes effect at
// once.
struct LiveEnv
{
    const char* var;
    const char* what;
};
const LiveEnv kLiveEnv[] = {
    { "STRIKERS_LOG_SCENES", "every front-end screen entered and package loaded" },
    { "STRIKERS_LOG_MATCH", "match state, once a second" },
    { "STRIKERS_LOG_NIS", "cutscene player and presentation clock" },
    { "STRIKERS_LOG_BUNDLES", "every bundle file opened" },
    { "STRIKERS_PROBE_PAD", "pad state on change" },
    { "STRIKERS_DUMP_FEN", "the converted front-end scene graph, per package" },
};

}   // namespace

void PortOverlayInit(void)
{
    if (s_read)
        return;
    s_read = 1;
    const char* env = std::getenv("STRIKERS_OVERLAY");
    s_enabled = (env != nullptr && *env != '\0' && std::strcmp(env, "0") != 0);

    // STRIKERS_OVERLAY=menu starts with the menu open rather than the compact heads-up form.
    if (env != nullptr && (std::strcmp(env, "menu") == 0 || std::strcmp(env, "2") == 0))
        set_menu_open(true);
}

int PortOverlayEnabled(void)
{
    PortOverlayInit();
    return s_enabled;
}

// ---------------------------------------------------------------------------
// The menu's look: a dark stadium-night panel with the game's lime green as
// the accent, rounded shapes, and a proper UI font instead of ImGui's pixel
// one.  Set once, before the first ImGui frame (PortOverlayPreFrame).
// ---------------------------------------------------------------------------
static void apply_theme()
{
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 14.0f;
    st.ChildRounding = 12.0f;
    st.FrameRounding = 8.0f;
    st.PopupRounding = 10.0f;
    st.ScrollbarRounding = 8.0f;
    st.GrabRounding = 8.0f;
    st.TabRounding = 8.0f;
    st.WindowPadding = ImVec2(18.0f, 16.0f);
    st.FramePadding = ImVec2(12.0f, 7.0f);
    st.ItemSpacing = ImVec2(10.0f, 9.0f);
    st.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    st.CellPadding = ImVec2(8.0f, 6.0f);
    st.ScrollbarSize = 12.0f;
    st.GrabMinSize = 14.0f;
    st.WindowBorderSize = 1.0f;
    st.ChildBorderSize = 0.0f;
    st.FrameBorderSize = 0.0f;
    st.PopupBorderSize = 1.0f;
    st.SeparatorTextBorderSize = 2.0f;
    st.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    const ImVec4 lime(0.56f, 0.93f, 0.22f, 1.0f);
    const ImVec4 bg(0.055f, 0.067f, 0.11f, 0.96f);
    const ImVec4 panel(0.09f, 0.11f, 0.17f, 1.0f);
    const ImVec4 frame(0.13f, 0.16f, 0.24f, 1.0f);
    const ImVec4 frameHi(0.18f, 0.22f, 0.32f, 1.0f);
    const ImVec4 green(0.22f, 0.56f, 0.15f, 1.0f);

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.95f, 0.98f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.60f, 0.69f, 1.0f);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.10f, 0.16f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.08f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frameHi;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.27f, 0.38f, 1.0f);
    c[ImGuiCol_TitleBg] = panel;
    c[ImGuiCol_TitleBgActive] = panel;
    c[ImGuiCol_TitleBgCollapsed] = panel;
    c[ImGuiCol_MenuBarBg] = panel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.30f, 0.40f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.32f, 0.38f, 0.50f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.40f, 0.47f, 0.60f, 1.0f);
    c[ImGuiCol_CheckMark] = lime;
    c[ImGuiCol_SliderGrab] = lime;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.70f, 1.0f, 0.42f, 1.0f);
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = frameHi;
    c[ImGuiCol_ButtonActive] = green;
    c[ImGuiCol_Header] = ImVec4(lime.x, lime.y, lime.z, 0.20f);
    c[ImGuiCol_HeaderHovered] = ImVec4(lime.x, lime.y, lime.z, 0.30f);
    c[ImGuiCol_HeaderActive] = ImVec4(lime.x, lime.y, lime.z, 0.42f);
    c[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    c[ImGuiCol_SeparatorHovered] = ImVec4(lime.x, lime.y, lime.z, 0.5f);
    c[ImGuiCol_SeparatorActive] = lime;
    c[ImGuiCol_ResizeGrip] = ImVec4(lime.x, lime.y, lime.z, 0.15f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(lime.x, lime.y, lime.z, 0.45f);
    c[ImGuiCol_ResizeGripActive] = lime;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = frameHi;
    c[ImGuiCol_TabSelected] = frame;
    c[ImGuiCol_TabSelectedOverline] = lime;
    c[ImGuiCol_TabDimmed] = panel;
    c[ImGuiCol_TabDimmedSelected] = frame;
    c[ImGuiCol_PlotLines] = lime;
    c[ImGuiCol_PlotHistogram] = ImVec4(1.0f, 0.58f, 0.16f, 1.0f);
    c[ImGuiCol_TableHeaderBg] = panel;
    c[ImGuiCol_TableBorderStrong] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    c[ImGuiCol_TableBorderLight] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(lime.x, lime.y, lime.z, 0.35f);
    c[ImGuiCol_NavCursor] = lime;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

// The first font file of a list that exists.
static bool first_font(char* out, size_t cap, const char* dir, const char* const* names, int count)
{
    for (int i = 0; i < count; i++)
    {
        std::snprintf(out, cap, "%s%s", dir, names[i]);
        if (FILE* f = std::fopen(out, "rb"))
        {
            std::fclose(f);
            return true;
        }
    }
    return false;
}



extern "C" void PortOverlayPreFrame(void)
{
    static bool s_done = false;
    if (s_done || ImGui::GetCurrentContext() == nullptr)
        return;
    s_done = true;
    apply_theme();

    ImGuiIO& io = ImGui::GetIO();
    // The SDL renderer backend makes its font texture once and never again; once it has, the
    // atlas stays as it is (ImGui's own font, themed).
    if (io.BackendRendererName != nullptr && std::strstr(io.BackendRendererName, "sdlrenderer") != nullptr &&
        io.Fonts->TexID != 0)
        return;

    char dir[512] = "";
#if defined(_WIN32)
    const char* windir = std::getenv("WINDIR");
    std::snprintf(dir, sizeof dir, "%s\\Fonts\\", windir != nullptr ? windir : "C:\\Windows");
    const char* const kRegular[] = { "segoeui.ttf", "arial.ttf" };
    const char* const kBold[] = { "seguisb.ttf", "segoeuib.ttf", "arialbd.ttf" };
    const char* const kBlack[] = { "seguibl.ttf", "segoeuib.ttf", "arialbd.ttf" };
#else
    std::snprintf(dir, sizeof dir, "/usr/share/fonts/truetype/dejavu/");
    const char* const kRegular[] = { "DejaVuSans.ttf" };
    const char* const kBold[] = { "DejaVuSans-Bold.ttf" };
    const char* const kBlack[] = { "DejaVuSans-Bold.ttf" };
#endif
    char regular[600], bold[600], black[600];
    if (!first_font(regular, sizeof regular, dir, kRegular, (int)(sizeof kRegular / sizeof kRegular[0])))
        return;   // no system font: ImGui's own, themed
    if (!first_font(bold, sizeof bold, dir, kBold, (int)(sizeof kBold / sizeof kBold[0])))
        std::snprintf(bold, sizeof bold, "%s", regular);
    if (!first_font(black, sizeof black, dir, kBlack, (int)(sizeof kBlack / sizeof kBlack[0])))
        std::snprintf(black, sizeof black, "%s", bold);

    // Sized for the window the game opened with: about 1/56 of its height (19 px at 1080p).
    float base = 18.0f;
    if (SDL_Window* w = main_window())
    {
        int pw = 0, ph = 0;
        if (SDL_GetWindowSizeInPixels(w, &pw, &ph) && ph > 0)
            base = (float)ph / 56.0f;
    }
    base = base < 16.0f ? 16.0f : (base > 36.0f ? 36.0f : base);

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    static const ImWchar kDigitsAndLetters[] = { 0x0020, 0x007E, 0 };
    ImFont* body = io.Fonts->AddFontFromFileTTF(regular, base, &cfg);
    if (body == nullptr)
        return;
    g_fontBody = body;
    g_fontBold = io.Fonts->AddFontFromFileTTF(bold, base * 1.05f, &cfg);
    g_fontHead = io.Fonts->AddFontFromFileTTF(bold, base * 1.4f, &cfg);
    g_fontTitle = io.Fonts->AddFontFromFileTTF(black, base * 1.8f, &cfg);
    g_fontHuge = io.Fonts->AddFontFromFileTTF(black, base * 3.6f, &cfg, kDigitsAndLetters);
    io.FontDefault = body;
    g_fontBasePx = base;
    std::fprintf(stderr, "[overlay] menu font %s at %.0f px\n", regular, (double)base);
}

void PortOverlaySetScene(const char* name) { copy_into(s_scene, sizeof(s_scene), name); }
const char* PortOverlaySceneName(void) { return s_scene; }
void PortOverlaySetStadium(const char* name)
{
    copy_into(s_stadium, sizeof(s_stadium), name);
    // The summary needs this as much as the overlay does, and for a better reason: it is what says
    // whether two runs measured the same content.
    PortBenchSetLabel("stadium", s_stadium);
}

void PortOverlaySetMatch(float clock, int scoreHome, int scoreAway)
{
    s_clock = clock;
    s_scoreHome = scoreHome;
    s_scoreAway = scoreAway;
}

// The simulation rate from the snapshot pushes, one per fixed step; 50 is real time, sampled once a
// second.
static unsigned long s_simSteps;
static unsigned long long s_simWindowStart;
static double s_simStepsPerSec = 50.0;
static bool s_simRateKnown;

void PortDebugSetMatch(const PortDebugMatch* match)
{
    if (match != nullptr)
        s_match = *match;
    s_simSteps++;
}

// Once a frame: folds the count into a rate and, above real time, says so on stderr every five
// seconds. Below real time is normal, since the fixed step does not run during the intro,
// cutscenes, the pause menu or the front end.
static void sim_rate_tick()
{
    const unsigned long long now = port_monotonic_ns();
    if (s_simWindowStart == 0)
    {
        s_simWindowStart = now;
        s_simSteps = 0;
        return;
    }
    const unsigned long long elapsed = now - s_simWindowStart;
    if (elapsed < 1000000000ull)
        return;
    s_simStepsPerSec = (double)s_simSteps * 1e9 / (double)elapsed;
    s_simRateKnown = (s_simSteps > 0);
    s_simSteps = 0;
    s_simWindowStart = now;

    static unsigned long long s_lastWarn;
    if (s_simRateKnown && s_simStepsPerSec > 55.0 && now - s_lastWarn > 5000000000ull)
    {
        s_lastWarn = now;
        std::fprintf(stderr, "[sim] %.1f steps/s = %.2fx real time%s\n",
                     s_simStepsPerSec, s_simStepsPerSec / 50.0,
                     g_bRunSimAndRenderInLockStep ? " (sim and render in lockstep is on)" : "");
    }
}

const PortDebugMatch* PortDebugGetMatch(void) { return &s_match; }

void PortDebugSetSession(const PortDebugSession* session)
{
    if (session != nullptr)
        s_session = *session;
}

const PortDebugSession* PortDebugGetSession(void) { return &s_session; }

void PortDebugSetPad(int port, int err, unsigned int buttons, int stickX, int stickY,
                     int substickX, int substickY, int triggerL, int triggerR)
{
    if (port < 0 || port >= 4)
        return;
    PadSnapshot& p = s_pad[port];
    p.err = err;
    p.buttons = buttons;
    p.stickX = stickX;
    p.stickY = stickY;
    p.substickX = substickX;
    p.substickY = substickY;
    p.triggerL = triggerL;
    p.triggerR = triggerR;
}

int PortOverlayMenuOpen(void) { return s_menuOpen ? 1 : 0; }

void PortOverlayToggleMenu(void) { set_menu_open(!s_menuOpen); }

int PortQuitRequested(void) { return s_quit ? 1 : 0; }

void PortRequestQuit(void) { s_quit = true; }

int PortDebugForcedStadium(void)
{
    if (s_forcedStadium == -2)
    {
        // Seed from the environment once. The spelling is the one rw_gameinfo_forcestadium
        // documents: a world-file base name, or an index.
        s_forcedStadium = -1;
        const char* want = std::getenv("STRIKERS_FORCE_STADIUM");
        if (want != nullptr && *want != '\0')
        {
            for (int i = 0; i < 7; i++)
                if (std::strcmp(want, kStadiums[i]) == 0)
                    s_forcedStadium = i;
            if (s_forcedStadium < 0 && want[0] >= '0' && want[0] <= '6' && want[1] == '\0')
                s_forcedStadium = want[0] - '0';
        }
    }
    return s_forcedStadium;
}

void PortDebugSetForcedStadium(int index)
{
    s_forcedStadium = (index >= 0 && index < 7) ? index : -1;
}

// The present mode as last asked for; Aurora has no getter, so the System tab's checkbox and
// PDBG_SET_VSYNC own this between them.
static bool s_vsyncOn;
static bool s_vsyncRead;
static bool vsync_state()
{
    if (!s_vsyncRead)
    {
        s_vsyncRead = true;
        const char* e = std::getenv("STRIKERS_VSYNC");
        s_vsyncOn = e != nullptr && *e != '\0' && std::strtoul(e, nullptr, 10) != 0;
    }
    return s_vsyncOn;
}

static void set_vsync(bool on)
{
    vsync_state();
    s_vsyncOn = on;
    aurora_enable_vsync(on);
    double displayHz = 0.0;
    PortFrameLimitInfo(nullptr, &displayHz, nullptr, nullptr);
    PortSetDisplayRefresh(displayHz, aurora_present_waits_for_vblank() ? 1 : 0);
}

// The game's lockstep switch, with what it means said out loud: one 20 ms step per rendered frame
// is fps/50 times real time.
static void set_lockstep(bool on)
{
    g_bRunSimAndRenderInLockStep = on;
    double limitHz = 0.0;
    PortFrameLimitInfo(&limitHz, nullptr, nullptr, nullptr);
    if (on)
        std::fprintf(stderr, "[sim] lockstep on: one 20 ms step per rendered frame, "
                             "%.0f fps is %.1fx real time\n",
                     limitHz, limitHz > 0.0 ? limitHz / 50.0 : 0.0);
    else
        std::fprintf(stderr, "[sim] lockstep off: 50 steps/s, real time\n");
}

// The ops the platform owns, executed as the queue is drained so the game's dispatcher never sees
// them; non-zero if the command was one of these.
static int platform_command(const PortDebugCommand& c)
{
    switch (c.op)
    {
    case PDBG_SET_FRAME_LIMIT:
        PortSetFrameLimit((double)c.f[0]);
        // Force the derivation now, so the [limiter] line lands on the frame the command did.
        PortFrameLimitInfo(nullptr, nullptr, nullptr, nullptr);
        return 1;
    case PDBG_SET_VSYNC:
        set_vsync(c.a != 0);
        PortFrameLimitInfo(nullptr, nullptr, nullptr, nullptr);
        return 1;
    case PDBG_SET_LOCKSTEP:
        set_lockstep(c.a != 0);
        return 1;
    case PDBG_SET_DT_SNAP:
        PortSetTaskClockSnap(c.a);
        std::fprintf(stderr, "[limiter] task step snapping %s\n", c.a < 0 ? "follows STRIKERS_DT_SNAP" : c.a ? "on" : "off");
        return 1;
    case PDBG_SET_WINDOW:
    {
        SDL_Window* w = main_window();
        if (w == nullptr)
            return 1;
        if (c.c == 0 || c.c == 1)
            SDL_SetWindowFullscreen(w, c.c == 1);
        if (c.a > 0 && c.b > 0)
            SDL_SetWindowSize(w, c.a, c.b);
        SDL_SyncWindow(w);
        int ww = 0, wh = 0;
        SDL_GetWindowSize(w, &ww, &wh);
        std::fprintf(stderr, "[window] %dx%d fullscreen=%d render scale %.2f\n", ww, wh,
                     is_fullscreen() ? 1 : 0, (double)PortRenderScale());
        return 1;
    }
    default:
        return 0;
    }
}

int PortDebugPushCommand(const PortDebugCommand* c)
{
    if (c == nullptr || s_commandCount >= kCommandCap)
        return 0;
    s_commands[(s_commandHead + s_commandCount) % kCommandCap] = *c;
    s_commandCount++;
    return 1;
}

int PortDebugPopCommand(PortDebugCommand* out)
{
    while (out != nullptr && s_commandCount != 0)
    {
        *out = s_commands[s_commandHead];
        s_commandHead = (s_commandHead + 1) % kCommandCap;
        s_commandCount--;
        if (!platform_command(*out))
            return 1;
    }
    return 0;
}

// The hotkeys. All function keys, deliberately: Aurora reads the raw keyboard state for the pad
// bindings regardless of what the event pump does with an event, so a letter here would also press
// a button.
void PortOverlayHandleKey(int scancode, int down)
{
    if (!down)
        return;
    switch (scancode)
    {
    case SDL_SCANCODE_F1:
        PortOverlayToggleMenu();
        break;
    case SDL_SCANCODE_F2:
        PortOverlayInit();
        s_enabled = !s_enabled;
        break;
    case SDL_SCANCODE_F3:
    {
        // Freezing one PC's game would put the two out of step online.
        if (NetplayManager::Instance().IsActive())
            break;
        // Freeze and unfreeze the simulation: the game's own slow-motion at zero.
        float* scale = PortSimTimeScalePtr();
        *scale = (*scale == 0.0f) ? 1.0f : 0.0f;
        break;
    }
    case SDL_SCANCODE_F4:
    {
        if (NetplayManager::Instance().IsActive())
            break;
        float* scale = PortSimTimeScalePtr();
        *scale = (*scale == 0.25f) ? 1.0f : 0.25f;
        break;
    }
    case SDL_SCANCODE_F11:
        set_fullscreen(!is_fullscreen());
        break;
    default:
        break;
    }
}

namespace
{

void draw_frame_tab(const PortBenchLive& live)
{
    ImGui::Text("%.1f fps   frame %.2f ms", live.fps, live.frameMs);
    ImGui::Separator();

    // The three phases, in the same terms the benchmark reports them, so a number read off the
    // screen means the same thing as a number read out of the summary.
    ImGui::Text("busy    %6.3f ms   p95 %6.3f", live.busyMs, live.busyP95Ms);
    ImGui::Text("present %6.3f ms", live.presentMs);
    ImGui::Text("sleep   %6.3f ms  (idle)", live.sleepMs);

    double limitHz = 0.0;
    PortFrameLimitInfo(&limitHz, nullptr, nullptr, nullptr);
    const double field = limitHz > 0.0 ? 1000.0 / limitHz : 1000.0 / 59.94;
    if (live.busyP95Ms > 0.0)
        ImGui::Text("headroom %.1fx at p95 against %.2f ms", field / live.busyP95Ms, field);

    // The last 256 frames. A p95 says a frame was slow; this says whether it was one frame or a
    // pattern.
    static float busy[256], frame[256];
    const size_t n = PortBenchGetHistory(busy, frame, 256);
    if (n > 1)
    {
        float top = 0.0f;
        for (size_t i = 0; i < n; i++)
            if (frame[i] > top)
                top = frame[i];
        top = top < 20.0f ? 20.0f : top;
        ImGui::PlotLines("##frame", frame, (int)n, 0, "frame ms", 0.0f, top, ImVec2(-1.0f, 60.0f));
        ImGui::PlotLines("##busy", busy, (int)n, 0, "busy ms", 0.0f, top, ImVec2(-1.0f, 60.0f));
    }

    ImGui::Separator();
    ImGui::Text("worst   %6.3f ms at frame %lu", live.worstMs, live.worstFrame);
    ImGui::Text("scene   %s", s_scene);
    ImGui::Text("stadium %s", s_stadium);
    if (live.matchActive)
        ImGui::Text("match   %5.1fs   %d - %d", (double)s_clock, s_scoreHome, s_scoreAway);

    if (ImGui::CollapsingHeader("draw calls"))
    {
        const AuroraStats* st = aurora_get_stats();
        if (st != nullptr)
        {
            ImGui::Text("draws %u   merged %u", st->drawCallCount, st->mergedDrawCallCount);
            ImGui::Text("pipelines %u created, %u queued", st->createdPipelines,
                        st->queuedPipelines);
            ImGui::Text("this frame: vert %u KB  uniform %u KB  index %u KB  tex %u KB",
                        st->lastVertSize / 1024, st->lastUniformSize / 1024,
                        st->lastIndexSize / 1024, st->lastTextureUploadSize / 1024);
        }
    }

    // GPU memory, the largest single thing in the process. Aurora's pools are fixed-size
    // compile-time constants, and the gap between peak and reserved is what a reader acts on.
    if (ImGui::CollapsingHeader("gpu pools"))
    {
        uint32_t peak[kPoolCount] = {};
        uint32_t reserved[kPoolCount] = {};
        aurora_gfx_pool_stats(peak, reserved, kPoolCount);
        uint64_t peakTotal = 0, reservedTotal = 0;
        for (size_t i = 0; i < kPoolCount; i++)
        {
            peakTotal += peak[i];
            reservedTotal += reserved[i];
            ImGui::Text("%-10s %6.2f / %6.2f MiB", kPoolNames[i], peak[i] / 1048576.0,
                        reserved[i] / 1048576.0);
        }
        ImGui::Text("%-10s %6.2f / %6.2f MiB", "peak/set", peakTotal / 1048576.0,
                    reservedTotal / 1048576.0);
    }

    if (ImGui::CollapsingHeader("textures"))
    {
        uint64_t src = 0, uploaded = 0, count = 0;
        aurora_gfx_texture_stats(&src, &uploaded, &count);
        ImGui::Text("%llu textures", (unsigned long long)count);
        ImGui::Text("%.2f MB on disc -> %.2f MB uploaded", src / 1048576.0, uploaded / 1048576.0);
        if (src > 0)
            ImGui::Text("%.2fx expansion", (double)uploaded / (double)src);
    }

    if (ImGui::Button("print benchmark summary to stderr"))
        PortBenchReport();
}

void draw_match_tab()
{
    const PortDebugMatch& m = s_match;

    ImGui::Text("scene   %s", s_scene);
    ImGui::Text("stadium %s", s_stadium);

    if (!m.valid)
    {
        ImGui::TextDisabled("no match running");
        return;
    }

    ImGui::Text("clock   %5.1f / %.0f s   %s%s", (double)m.clock, (double)m.duration,
                name_of(kGameStates, m.gameState), m.inSuddenDeath ? "   SUDDEN DEATH" : "");
    ImGui::Text("mode    %s%s", name_of(kGameModes, m.gameMode), m.demoMode ? "  (demo)" : "");

    ImGui::SeparatorText("score");
    for (int t = 0; t < 2; t++)
    {
        ImGui::PushID(t);
        int score = m.teams[t].score;
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputInt(t == 0 ? "home" : "away", &score))
            cmd(PDBG_SET_SCORE, t, score < 0 ? 0 : score);
        ImGui::PopID();
    }

    ImGui::SeparatorText("clock");
    if (ImGui::SmallButton("+30 s"))
        cmd(PDBG_ADD_CLOCK, 0, 0, 0, 30.0f);
    ImGui::SameLine();
    if (ImGui::SmallButton("-30 s"))
        cmd(PDBG_ADD_CLOCK, 0, 0, 0, -30.0f);
    ImGui::SameLine();
    if (ImGui::SmallButton("10 s left"))
        cmd(PDBG_ADD_CLOCK, 0, 0, 0, 10.0f - (m.duration - m.clock));
    ImGui::SameLine();
    if (ImGui::SmallButton("end match"))
        cmd(PDBG_END_MATCH);
    help("Runs the clock out. A tied score goes to sudden death, which is the game's own path.");
    if (ImGui::SmallButton("sudden death now"))
        cmd(PDBG_SUDDEN_DEATH);
    ImGui::SameLine();
    if (ImGui::SmallButton("kickoff: home"))
        cmd(PDBG_KICKOFF, 0);
    ImGui::SameLine();
    if (ImGui::SmallButton("kickoff: away"))
        cmd(PDBG_KICKOFF, 1);

    ImGui::SeparatorText("simulation");
    {
        // The game's own slow-motion: it drives this itself, Presentation resets it to 1 on a
        // possession change, and goals slow it down, so the slider shows what is in force rather
        // than owning it.
        float* scale = PortSimTimeScalePtr();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::SliderFloat("time scale", scale, 0.0f, 2.0f, "%.2fx");
        ImGui::SameLine();
        if (ImGui::SmallButton("freeze"))
            *scale = 0.0f;
        ImGui::SameLine();
        if (ImGui::SmallButton("1/4"))
            *scale = 0.25f;
        ImGui::SameLine();
        if (ImGui::SmallButton("1x"))
            *scale = 1.0f;
        ImGui::TextDisabled("F3 freezes, F4 quarter speed.");
        if (s_simRateKnown)
        {
            const double x = s_simStepsPerSec / 50.0;
            if (x > 1.1)
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                   "simulation %.1f steps/s = %.2fx real time%s",
                                   s_simStepsPerSec, x,
                                   g_bRunSimAndRenderInLockStep ? "  (lockstep is on: Render tab)" : "");
            else
                ImGui::Text("simulation %.1f steps/s = %.2fx real time", s_simStepsPerSec, x);
            help("Fixed steps per wall second, counted from the game's own match update. "
                 "50 is real time in play at any frame rate; lower is normal, because the "
                 "fixed step does not run during the intro, goal cutscenes or the pause "
                 "menu. Higher is the time scale above or the lockstep switch on the "
                 "Render tab.");
        }
    }

    ImGui::SeparatorText("ball");
    const double speed =
        std::sqrt((double)m.ballVel[0] * m.ballVel[0] + (double)m.ballVel[1] * m.ballVel[1] +
                  (double)m.ballVel[2] * m.ballVel[2]);
    ImGui::Text("pos   %6.2f %6.2f %6.2f   speed %6.2f", (double)m.ballPos[0],
                (double)m.ballPos[1], (double)m.ballPos[2], speed);
    if (m.ballOwner < 0)
        ImGui::TextDisabled("loose");
    else
        ImGui::Text("held by #%d %s", m.ballOwner, m.characters[m.ballOwner].name);

    // These two are why this tab exists: they are the state behind the keeper standing next to a
    // ball it will not pick up.
    ImGui::Text("pass target %s      pickup lock %.2fs", m.ballHasPassTarget ? "SET" : "none",
                (double)m.ballNoPickup);
    if (ImGui::SmallButton("warp to centre"))
        cmd(PDBG_WARP_BALL, 0, 0, 0, 0.0f, 0.0f, 2.0f);
    ImGui::SameLine();
    if (ImGui::SmallButton("into home net"))
        cmd(PDBG_WARP_BALL, 0, 0, 0, -1.0f, 0.0f, 1.0f, 1.0f);
    ImGui::SameLine();
    if (ImGui::SmallButton("into away net"))
        cmd(PDBG_WARP_BALL, 0, 0, 0, 1.0f, 0.0f, 1.0f, 1.0f);
    help("The net warps put the ball just inside the goal line at a walking pace, so the "
         "game's own goal detection, scorer credit and celebration run. f[3] marks the "
         "request as net-relative; the drain resolves it against the net's real position.");

    ImGui::SeparatorText("presentation");
    ImGui::Text("%s   %.1fs%s", m.presentation, (double)m.presentationTime,
                m.nisActive ? "   NIS playing" : "");
    if (ImGui::SmallButton("skip"))
        cmd(PDBG_SKIP_PRESENTATION);
    help("Sets the same flag the pad's skip button does, so the script's own bypass "
         "windows decide when it takes. Not every phase is skippable.");
    ImGui::SameLine();
    ImGui::TextDisabled(m.skipAllowed ? "(skippable now)" : "(not skippable now)");

    ImGui::SeparatorText("bowser");
    ImGui::Text("%s   timer %.1fs", m.bowserAlive ? "on the pitch" : "away",
                (double)m.bowserTimer);
    if (ImGui::SmallButton("attack now"))
        cmd(PDBG_BOWSER_ATTACK);
    ImGui::SameLine();
    if (ImGui::SmallButton("send away"))
        cmd(PDBG_BOWSER_HIDE);

    ImGui::SeparatorText("effects");
    ImGui::Text("%d emission controllers live", m.effects);
    ImGui::SameLine();
    if (ImGui::SmallButton("kill all"))
        cmd(PDBG_KILL_EFFECTS, 0);
}

void draw_players_tab()
{
    const PortDebugMatch& m = s_match;
    if (!m.valid)
    {
        ImGui::TextDisabled("no match running");
        return;
    }

    static int s_selected = 0;

    if (ImGui::BeginTable("chars", 8,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 240.0f)))
    {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("who");
        ImGui::TableSetupColumn("role");
        ImGui::TableSetupColumn("action");
        ImGui::TableSetupColumn("wants");
        ImGui::TableSetupColumn("item");
        ImGui::TableSetupColumn("pos");
        ImGui::TableSetupColumn("flags");
        ImGui::TableHeadersRow();
        for (int i = 0; i < m.characterCount; i++)
        {
            const PortDebugCharacter& c = m.characters[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char label[16];
            std::snprintf(label, sizeof label, "%d##row", i);
            if (ImGui::Selectable(label, s_selected == i,
                                  ImGuiSelectableFlags_SpanAllColumns))
                s_selected = i;
            ImGui::TableNextColumn();
            ImGui::Text("%s%s %s", c.team == 0 ? "H " : "A ", c.name,
                        c.isGoalie ? "GK" : c.isCaptain ? "C" : "");
            ImGui::TableNextColumn();
            ImGui::Text("%s", c.isGoalie ? "goalie" : name_of(kRoles, c.role));
            ImGui::TableNextColumn();
            ImGui::Text("%s", c.isGoalie ? name_of(kGoalieStates, c.state)
                                         : name_of(kFielderStates, c.state));
            ImGui::TableNextColumn();
            if (c.isGoalie)
                ImGui::Text("urgency %s", name_of(kUrgency, c.urgency));
            else
                ImGui::Text("%s", name_of(kDesires, c.desire));
            ImGui::TableNextColumn();
            if (!c.isGoalie && c.powerup >= 0)
                ImGui::Text("%s x%d", name_of(kPowerups, c.powerup), c.powerupCount);
            ImGui::TableNextColumn();
            ImGui::Text("%6.1f %6.1f", (double)c.pos[0], (double)c.pos[1]);
            ImGui::TableNextColumn();
            ImGui::Text("%s%s%s%s%s%s", c.hasBall ? "BALL " : "", c.isHuman ? "human " : "",
                        c.frozenSeconds > 0.0f ? "frozen " : "", c.invincible ? "star " : "",
                        c.fallen ? "down " : "", c.card >= 0 ? name_of(kCards, c.card) : "");
        }
        ImGui::EndTable();
    }

    if (s_selected < 0 || s_selected >= m.characterCount)
        s_selected = 0;
    const PortDebugCharacter& c = m.characters[s_selected];

    ImGui::SeparatorText("selected");
    ImGui::Text("#%d %s   anim %s   speed %.2f", s_selected, c.name, c.anim, (double)c.speed);
    if (c.isGoalie)
        ImGui::Text("energy %.2f", (double)c.energy);
    else
        ImGui::Text("shot meter %s %.2f", name_of(kShotMeter, c.shotMeterState),
                    (double)c.shotMeterValue);

    if (ImGui::SmallButton("give ball"))
        cmd(PDBG_GIVE_BALL, s_selected);
    help("Releases the current owner and calls PickupBall, exactly as the game does at kickoff. "
         "The pickup lock on the ball and on the player can still refuse it.");
    if (!c.isGoalie)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("freeze 3 s"))
            cmd(PDBG_FREEZE_PLAYER, s_selected, 0, 0, 3.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("unfreeze"))
            cmd(PDBG_FREEZE_PLAYER, s_selected, 0, 0, 0.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("throw held item"))
            cmd(PDBG_THROW_POWERUP, s_selected, c.powerup, c.powerupCount);
    }

    ImGui::SeparatorText("teams");
    for (int t = 0; t < 2; t++)
    {
        const PortDebugTeam& tm = m.teams[t];
        ImGui::Text("%s  %s / %s   %s, %s   AI %s", t == 0 ? "home" : "away",
                    name_of(kTeams, tm.teamId), name_of(kSidekicks, tm.sidekickId),
                    name_of(kSituations, tm.situation), name_of(kStyles, tm.style),
                    name_of(kDifficulties, tm.difficulty));
    }
}

void draw_items_tab()
{
    const PortDebugMatch& m = s_match;
    if (!m.valid)
    {
        ImGui::TextDisabled("no match running");
        return;
    }

    static int s_type = 0;
    static int s_count = 1;
    ImGui::SetNextItemWidth(160.0f);
    ImGui::Combo("item", &s_type, kPowerups, (int)(sizeof kPowerups / sizeof kPowerups[0]));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputInt("count", &s_count);
    if (s_count < 1)
        s_count = 1;
    if (s_count > 3)
        s_count = 3;

    ImGui::Text("%d item objects live on the pitch", m.powerupObjects);
    ImGui::Text("%s", m.isPure ? "pure game: the game will not award items" : "");

    for (int t = 0; t < 2; t++)
    {
        const PortDebugTeam& tm = m.teams[t];
        ImGui::PushID(t);
        ImGui::SeparatorText(t == 0 ? "home" : "away");
        ImGui::ProgressBar(tm.powerupMeter, ImVec2(160.0f, 0.0f), "meter");
        for (int s = 0; s < 2; s++)
        {
            if (tm.powerup[s] >= 0)
                ImGui::Text("slot %d: %s x%d", s, name_of(kPowerups, tm.powerup[s]),
                            tm.powerupCount[s]);
            else
                ImGui::TextDisabled("slot %d: empty", s);
        }
        if (ImGui::SmallButton("give selected"))
            cmd(PDBG_GIVE_POWERUP, t, s_type, s_count);
        ImGui::SameLine();
        if (ImGui::SmallButton("random award"))
            cmd(PDBG_AWARD_POWERUP, t);
        help("The game's own award, the one the meter triggers at full. Refuses when items "
             "are off or the game is pure.");
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::SmallButton("destroy items on pitch"))
        cmd(PDBG_CLEAR_POWERUPS, 0);
    ImGui::SameLine();
    if (ImGui::SmallButton("clear everything"))
        cmd(PDBG_CLEAR_POWERUPS, 1);
    help("Also empties both teams' inventories.");
}

void draw_ai_tab()
{
    const PortDebugMatch& m = s_match;

    ImGui::SeparatorText("difficulty");
    ImGui::TextDisabled("The AI's chance tables, reloaded from the difficulty files.");
    ImGui::TextDisabled("A human side is normally `human`; the game re-derives it on");
    ImGui::TextDisabled("the next choose-sides screen.");
    static int s_diff[2] = { 2, 2 };
    if (m.valid)
    {
        static bool s_seeded = false;
        if (!s_seeded)
        {
            s_seeded = true;
            for (int t = 0; t < 2; t++)
                s_diff[t] = (m.teams[t].difficulty >= 0 && m.teams[t].difficulty < 7)
                                ? m.teams[t].difficulty : 2;
        }
    }
    ImGui::SetNextItemWidth(140.0f);
    ImGui::Combo("home", &s_diff[0], kDifficulties, 7);
    ImGui::SetNextItemWidth(140.0f);
    ImGui::Combo("away", &s_diff[1], kDifficulties, 7);
    ImGui::BeginDisabled(!m.valid);
    if (ImGui::SmallButton("apply"))
        cmd(PDBG_SET_DIFFICULTY, s_diff[0], s_diff[1]);
    ImGui::EndDisabled();
    if (m.valid)
        ImGui::Text("in force: %s / %s", name_of(kDifficulties, m.teams[0].difficulty),
                    name_of(kDifficulties, m.teams[1].difficulty));

    ImGui::SeparatorText("who plays which side");
    ImGui::TextDisabled("Pad -> side. The teams rebind their controllers every update,");
    ImGui::TextDisabled("so this takes effect on the next frame.");
    for (int pad = 0; pad < 4; pad++)
    {
        ImGui::PushID(pad);
        int side = m.valid ? m.padSide[pad] + 1 : 0;   // 0 none, 1 home, 2 away
        const char* const kSides[] = { "none (AI)", "home", "away" };
        ImGui::SetNextItemWidth(120.0f);
        char label[16];
        std::snprintf(label, sizeof label, "pad %d", pad);
        if (ImGui::Combo(label, &side, kSides, 3))
            cmd(PDBG_SET_SIDE, pad, side - 1);
        ImGui::PopID();
    }

    ImGui::SeparatorText("switches");
    ImGui::Checkbox("pad monkey (random input on every pad)", &g_bEnableGamecubePadMonkey);
    bool noPickups = sbNoBallPickups != 0;
    if (ImGui::Checkbox("nobody can pick up the ball", &noPickups))
        sbNoBallPickups = noPickups ? 1 : 0;
    ImGui::Checkbox("disable collision detection", &sbDisableCollisionDetection);
    help("Skips the physics collision pass for everything. The retail 'noclip'.");

    if (m.valid)
    {
        ImGui::SeparatorText("teams");
        for (int t = 0; t < 2; t++)
            ImGui::Text("%s: %s, %s", t == 0 ? "home" : "away",
                        name_of(kSituations, m.teams[t].situation),
                        name_of(kStyles, m.teams[t].style));
    }
}

void draw_cheats_tab()
{
    const PortDebugMatch& m = s_match;

    ImGui::SeparatorText("match settings");
    ImGui::TextDisabled("The options screen's values for the running match. Read where");
    ImGui::TextDisabled("they are used (an item throw, a shot, Bowser's timer), so a");
    ImGui::TextDisabled("change takes effect at the next one.");
    ImGui::BeginDisabled(!m.valid);
    check_cmd("items", m.optPowerUps, PDBG_SET_OPTION, PDBG_OPT_POWERUPS);
    check_cmd("super strikes", m.optShoot2Score, PDBG_SET_OPTION, PDBG_OPT_SHOOT2SCORE);
    check_cmd("bowser attacks", m.optBowser, PDBG_SET_OPTION, PDBG_OPT_BOWSER);
    check_cmd("rumble", m.optRumble, PDBG_SET_OPTION, PDBG_OPT_RUMBLE);
    check_cmd("pure game (no items at all)", m.isPure, PDBG_SET_OPTION, PDBG_OPT_PURE);
    {
        int skill = m.skillLevel;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::Combo("skill level", &skill, kSkills, 5))
            cmd(PDBG_SET_OPTION, PDBG_OPT_SKILL, skill);
        help("The user-facing setting. Apply a difficulty on the AI tab for it to reach the "
             "running match.");
    }

    ImGui::SeparatorText("cheats");
    ImGui::TextDisabled("The retail cheat options, as the save file holds them. The game");
    ImGui::TextDisabled("honours them in friendly and tournament modes only.");
    check_cmd("infinite items", m.cheatInfinite, PDBG_SET_OPTION, PDBG_OPT_INFINITE);
    check_cmd("glass-jaw goalies", m.cheatStunned, PDBG_SET_OPTION, PDBG_OPT_STUNNED);
    check_cmd("tilting field", m.cheatTilt, PDBG_SET_OPTION, PDBG_OPT_TILT);
    check_cmd("every super strike perfect", m.cheatPerfect, PDBG_SET_OPTION, PDBG_OPT_PERFECT);
    {
        int custom = m.cheatCustom;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::Combo("custom items", &custom, kCustomPowerups, 6))
            cmd(PDBG_SET_OPTION, PDBG_OPT_CUSTOM, custom);
    }
    {
        bool all = (m.trophies[0] == 0xFF && m.trophies[1] == 0xFF);
        if (ImGui::Checkbox("all trophies earned (unlocks the cheats menu)", &all))
            cmd(PDBG_SET_OPTION, PDBG_OPT_TROPHIES, all ? 1 : 0);
    }
    ImGui::EndDisabled();
    if (!m.valid)
        ImGui::TextDisabled("(settings need a running match to show and set)");

    ImGui::SeparatorText("unlocks");
    {
        bool unlock = PortGetUnlockAll() != 0;
        if (ImGui::Checkbox("every stadium, team and cup available", &unlock))
            PortSetUnlockAll(unlock ? 1 : 0);
        help("Answers the game's own `givealltrophies` config test, which every unlock "
             "check consults as it runs. STRIKERS_UNLOCK_ALL sets it at boot.");
    }

    ImGui::SeparatorText("next match");
    ImGui::TextDisabled("Read when a match is set up, so these apply to the next one.");
    {
        int forced = PortDebugForcedStadium() + 1;
        const char* names[8];
        names[0] = "(the game's own pick)";
        for (int i = 0; i < 7; i++)
            names[i + 1] = kStadiums[i];
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo("stadium", &forced, names, 8))
            PortDebugSetForcedStadium(forced - 1);
        help("Overrides PickStadium: attract mode, cup rounds and tournament rounds all "
             "go through it. The front end's own stadium screen does not.");
    }
    {
        static int s_team[2] = { 3, 2 };
        static int s_side[2] = { 0, 1 };
        const char* keys[4] = { "team1", "team2", "sidekick1", "sidekick2" };
        for (int t = 0; t < 2; t++)
        {
            ImGui::PushID(t);
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::Combo(keys[t], &s_team[t], kTeams, 9))
            {
                char kv[64];
                std::snprintf(kv, sizeof kv, "%s=%s", keys[t], kTeams[s_team[t]]);
                cmd(PDBG_SET_CONFIG_STRING, 0, 0, 0, 0, 0, 0, 0, kv);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::Combo(keys[t + 2], &s_side[t], kSidekicks, 4))
            {
                char kv[64];
                std::snprintf(kv, sizeof kv, "%s=%s", keys[t + 2], kSidekicks[s_side[t]]);
                cmd(PDBG_SET_CONFIG_STRING, 0, 0, 0, 0, 0, 0, 0, kv);
            }
            ImGui::PopID();
        }
        help("The retail `team1`/`team2`/`sidekick1`/`sidekick2` config keys, which the "
             "game reads for any match it sets up itself. STRIKERS_TEAM1 and friends set "
             "the same keys at boot.");
    }
}

void draw_camera_tab()
{
    const PortDebugMatch& m = s_match;

    ImGui::SeparatorText("current");
    if (m.valid)
    {
        ImGui::Text("%s   fov %.1f", name_of(kCameraTypes, m.cameraType), (double)m.camFov);
        ImGui::Text("eye    %7.2f %7.2f %7.2f", (double)m.camPos[0], (double)m.camPos[1],
                    (double)m.camPos[2]);
        ImGui::Text("target %7.2f %7.2f %7.2f", (double)m.camTarget[0], (double)m.camTarget[1],
                    (double)m.camTarget[2]);

        ImGui::Text("game camera at the bottom of the stack: %s",
                    name_of(kCameraTypes, m.cameraTypeWanted));
        ImGui::TextDisabled("Not switchable from here: the retail camera switch corrupts");
        ImGui::TextDisabled("memory on this port.");
    }
    else
    {
        ImGui::TextDisabled("no match running");
    }

    ImGui::SeparatorText("framing");
    ImGui::Text("display aspect  %.4f     logical frame %u x 448", (double)PortTargetAspect(),
                PortLogicalFrameWidth());

    // The console drew its 3D through a 1.25 view volume on a 4/3 screen, 6.7% wider than geometry.
    float stretch = PortGetFrustumStretch();
    if (ImGui::SliderFloat("console stretch", &stretch, 0.0f, 1.0f, "%.2f"))
        PortSetFrustumStretch(stretch);
    ImGui::SameLine();
    ImGui::TextDisabled("(1 = as it shipped)");

    float blend = PortGetCameraBlendOverride();
    bool derived = blend < 0.0f;
    if (ImGui::Checkbox("camera blend from aspect", &derived))
        PortSetCameraBlendOverride(derived ? -1.0f : PortCameraAspectBlend());
    if (!derived)
    {
        blend = PortCameraAspectBlend();
        if (ImGui::SliderFloat("blend", &blend, 0.0f, 3.0f, "%.2f"))
            PortSetCameraBlendOverride(blend);
        ImGui::TextDisabled("0 = the 4:3 camera table, 1 = the widescreen one");
    }

    static bool bars = PortDrawCinematicBars() != 0;
    if (ImGui::Checkbox("cinematic letterbox bars", &bars))
        PortSetDrawCinematicBars(bars);
}

void draw_render_tab()
{
    ImGui::TextDisabled("The retail tweak menu's switches. Each one is a global the");
    ImGui::TextDisabled("shipped code still reads; the menu that set them was stripped.");

    ImGui::SeparatorText("what is drawn");
    ImGui::Checkbox("world", &g_bRenderWorld);
    ImGui::SameLine();
    check_u8("HUD", &g_hudVisible);
    ImGui::SameLine();
    ImGui::Checkbox("static models", &g_bEnableDrawableModel);
    ImGui::SameLine();
    ImGui::Checkbox("skinned models", &g_bEnableDrawableSkinModel);
    // Class statics the platform cannot name: World::sbStadiumRenderingDisabled and its neighbours.
    static bool s_stadiumOff, s_skyboxOff, s_charShadowsOff;
    if (ImGui::Checkbox("hide stadium", &s_stadiumOff))
        cmd(PDBG_SET_CLASS_FLAG, PDBG_CF_STADIUM_OFF, s_stadiumOff ? 1 : 0);
    help("Everything but the ball, the hammer and the skybox.");
    ImGui::SameLine();
    if (ImGui::Checkbox("hide skybox", &s_skyboxOff))
        cmd(PDBG_SET_CLASS_FLAG, PDBG_CF_SKYBOX_OFF, s_skyboxOff ? 1 : 0);

    ImGui::SeparatorText("lighting and texture stages");
    ImGui::Checkbox("lighting", &g_bAllowLighting);
    ImGui::SameLine();
    ImGui::Checkbox("specular", &g_bAllowSpecular);
    ImGui::SameLine();
    check_u8("white diffuse", &g_bWhiteDiffuse);
    help("Replaces every diffuse texture with white: the lighting alone.");
    check_u8("detail", &g_TexDetail);
    ImGui::SameLine();
    check_u8("shadow", &g_TexShadow);
    ImGui::SameLine();
    check_u8("self-illum", &g_TexSelfIllum);
    ImGui::SameLine();
    check_u8("gloss", &g_TexGloss);
    check_u8("texel density view", &g_bTexelDensity);
    ImGui::SameLine();
    ImGui::Checkbox("ball glow", &g_bBallGlow);

    ImGui::SeparatorText("shadows");
    check_u8("blob shadows", &g_bShadowBlobs);
    ImGui::SameLine();
    ImGui::Checkbox("planar", &g_bDrawPlanarShadows);
    ImGui::SameLine();
    ImGui::Checkbox("volumes", &g_bShadowVolumes);
    ImGui::SameLine();
    if (ImGui::Checkbox("no character shadows", &s_charShadowsOff))
        cmd(PDBG_SET_CLASS_FLAG, PDBG_CF_CHAR_SHADOWS_OFF, s_charShadowsOff ? 1 : 0);
    check_u8("shadow preview", &g_bPreview);
    help("Draws the shadow map's own model.");
    ImGui::SameLine();
    check_u8("shadow bounds", &g_bShadowBounds);
    help("Shadow quad outlines in blue, the light in yellow.");
    ImGui::SameLine();
    ImGui::Checkbox("co-planar per object", &g_bCoPlanarPerObject);

    ImGui::SeparatorText("culling");
    check_u8("clip to frustum", &g_bClipToFrustum);
    ImGui::SameLine();
    check_u8("freeze frustum", &g_bFreezeFrustum);
    help("Stops the culling planes following the camera, so moving the camera shows "
         "what the culler thinks is visible.");
    ImGui::SameLine();
    check_u8("culling stats", &g_bDrawCullingInfo);

    ImGui::SeparatorText("debug drawing");
    check_u8("bounding boxes", &g_bDrawBoundingBoxes);
    ImGui::SameLine();
    check_u8("bounding spheres", &g_bDrawBoundingSphere);
    ImGui::SameLine();
    check_u8("net UV checker", &sbUseCheckerTexture);
    {
        const char* const kBones[] = { "off", "endpoint bounds", "all points" };
        ImGui::SetNextItemWidth(140.0f);
        if (g_nShowBones > 2)
            g_nShowBones = 2;
        ImGui::Combo("character bones", &g_nShowBones, kBones, 3);
    }
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderInt("world grid spacing", &g_nGridDisplaySpacing, 0, 20);
    ImGui::SameLine();
    ImGui::TextDisabled("(0 = off)");

    ImGui::SeparatorText("on-screen text (the game's own)");
    ImGui::Checkbox("frame stats", &g_bFrameStatsOnScreen);
    ImGui::SameLine();
    ImGui::Checkbox("frame smiler", &g_bFrameSmiler);
    ImGui::SameLine();
    check_u8("heap banner", &g_bMemoryOnScreen);
    ImGui::SameLine();
    ImGui::Checkbox("safe frame", &g_bDrawSafeFrame);
    {
        bool lock = g_bRunSimAndRenderInLockStep;
        if (ImGui::Checkbox("sim and render in lockstep", &lock))
            set_lockstep(lock);
        double limitHz = 0.0;
        PortFrameLimitInfo(&limitHz, nullptr, nullptr, nullptr);
        ImGui::SameLine();
        if (lock)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "one 20 ms step per frame: NOT real time (%.1fx at %.0f fps)",
                               limitHz > 0.0 ? limitHz / 50.0 : 0.0, limitHz);
        help("The retail tweak menu's switch. The simulation normally runs 50 fixed steps a "
             "second whatever the frame rate; with this on it runs one step per rendered "
             "frame, so the game plays at fps/50 times real time: 2.4x on a 120 Hz "
             "display, and uncapped it is a blur. If the game is running fast, this is "
             "the first thing to check; the Match tab shows the measured rate.");
    }
}

void draw_audio_tab()
{
    unsigned long buffers = 0, underruns = 0;
    int everNonSilent = 0;
    PortAudioStats(&buffers, &underruns, &everNonSilent);

    ImGui::SeparatorText("transport");
    if (!PortAudioDeviceOpen())
    {
        ImGui::TextDisabled("no audio device: STRIKERS_AUDIO is off, or the device failed to open");
        ImGui::TextDisabled("(audio cannot be switched on after boot; MusyX is not initialised)");
    }
    else
    {
        ImGui::Text("ticks %lu   underruns %lu   %s", buffers, underruns,
                    everNonSilent ? "has produced sound" : "silent so far");
        unsigned long calls = 0, over2 = 0;
        double mean = 0.0, worst = 0.0;
        PortAudioUpdateCost(&calls, &mean, &worst, &over2);
        ImGui::Text("update cost: mean %.3f ms  max %.2f ms  %lu frames over 2 ms", mean, worst,
                    over2);
    }

    ImGui::SeparatorText("mixer");
    PortAudioMixInfo mix;
    if (PortAudioMixStats(&mix))
    {
        // The six numbers that between them name every separate cause of silence.
        ImGui::Text("studios %d   voices %d   with sample %d", mix.studios, mix.voices,
                    mix.withSample);
        ImGui::Text("peak envelope 0x%04x   peak pan 0x%04x   bus peak %d / 32767", mix.peakEnv,
                    mix.peakVol, mix.busPeak);
        ImGui::Text("mix rate %u Hz", mix.mixFrq);
        float level = mix.busPeak / 32767.0f;
        ImGui::ProgressBar(level, ImVec2(200.0f, 0.0f), "bus");

        // The master stage is the port's own, so it is worth being able to see it working. rawPeak
        // over 32767 with a limitGain below 1 is the limiter doing its job; rawPeak under it and a
        // gain of 1 is the limiter out of the way.
        ImGui::Text("studios summed %d / 32767   limiter gain %.3f   master %.2f", mix.rawPeak,
                    (double)mix.limitGain, (double)mix.masterGain);
        help("The limiter and the master level have no console equivalent: the game shipped "
             "with the DSP compressor off and the final mix simply clamped. STRIKERS_AUDIO_VOLUME "
             "sets the master.");
    }
    else
    {
        ImGui::TextDisabled("the mixer has not run");
    }

    ImGui::SeparatorText("dump");
    static char s_dumpPath[256] = "/tmp/strikers-audio.wav";
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputText("##dumppath", s_dumpPath, sizeof s_dumpPath);
    ImGui::SameLine();
    if (mix.dumping)
    {
        if (ImGui::SmallButton("stop"))
            PortAudioDumpStop();
        ImGui::SameLine();
        ImGui::Text("%lu frames", mix.dumpFrames);
    }
    else if (ImGui::SmallButton("start"))
    {
        PortAudioDumpStart(s_dumpPath);
    }
    help("Exactly what goes to the device, as a WAV. 'Is there sound' and 'is it the right "
         "sound' are different questions; this answers the second.");

    ImGui::SeparatorText("volume groups");
    ImGui::TextDisabled("The game's master volumes. Set, not read back.");
    static float s_vol[3] = { 1.0f, 1.0f, 1.0f };
    const char* const kGroups[] = { "music", "sfx", "voice" };
    for (int g = 0; g < 3; g++)
    {
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat(kGroups[g], &s_vol[g], 0.0f, 1.0f, "%.2f"))
            cmd(PDBG_SET_VOLUME, g + 1, 0, 0, s_vol[g]);   // VG_Music = 1
    }
    if (ImGui::SmallButton("silence everything"))
        cmd(PDBG_SILENCE);

    ImGui::SeparatorText("play a world effect by name");
    static char s_sfx[64] = "Whistle";
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##sfx", s_sfx, sizeof s_sfx);
    ImGui::SameLine();
    if (ImGui::SmallButton("play"))
        cmd(PDBG_PLAY_SFX, 0, 0, 0, 0, 0, 0, 0, s_sfx);
    help("Audio::PlayWorldSFXbyStr: the eWorldSFX enumerator without its prefix. "
         "Only during a match, once the stadium's sound bank is loaded.");
}


// ---------------------------------------------------------------------------
// The look of the menu: colours, fonts and a few drawn widgets.  Everything
// here only draws; none of it touches the game or the online session.
// ---------------------------------------------------------------------------
namespace ui
{
const ImVec4 kLime(0.56f, 0.93f, 0.22f, 1.0f);
const ImVec4 kOrange(1.0f, 0.58f, 0.16f, 1.0f);
const ImVec4 kYellow(1.0f, 0.84f, 0.25f, 1.0f);
const ImVec4 kRed(1.0f, 0.40f, 0.34f, 1.0f);
const ImVec4 kBlue(0.40f, 0.72f, 1.0f, 1.0f);
const ImVec4 kText(0.93f, 0.95f, 0.98f, 1.0f);
const ImVec4 kDim(0.60f, 0.65f, 0.74f, 1.0f);
const ImVec4 kCard(0.10f, 0.125f, 0.19f, 1.0f);
}   // namespace ui

// Pushes a font for one scope; with no font loaded it is a no-op.
struct FontScope
{
    bool pushed;
    explicit FontScope(ImFont* f) : pushed(f != nullptr)
    {
        if (pushed)
            ImGui::PushFont(f);
    }
    ~FontScope()
    {
        if (pushed)
            ImGui::PopFont();
    }
};

ImVec4 with_alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// A section heading: bold, in the accent colour, with a thin rule under it.
void heading(const char* text)
{
    ImGui::Spacing();
    {
        FontScope f(g_fontBold);
        ImGui::TextColored(ui::kLime, "%s", text);
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y - 2.0f), ImVec2(p.x + w, p.y - 0.5f),
                                              ImGui::GetColorU32(with_alpha(ui::kLime, 0.25f)));
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.25f));
}

// Large text.  With no big font loaded, the window font scale stands in.
void big_text(ImFont* font, float fallbackScale, const ImVec4& col, const char* text, bool center)
{
    const bool scaled = font == nullptr;
    if (scaled)
        ImGui::SetWindowFontScale(fallbackScale);
    {
        FontScope f(font);
        if (center)
        {
            const float tw = ImGui::CalcTextSize(text).x;
            const float ww = ImGui::GetWindowWidth();
            if (ww > tw)
                ImGui::SetCursorPosX((ww - tw) * 0.5f);
        }
        ImGui::TextColored(col, "%s", text);
    }
    if (scaled)
        ImGui::SetWindowFontScale(1.0f);
}

void centered_text(const ImVec4& col, const char* text)
{
    const float tw = ImGui::CalcTextSize(text).x;
    const float ww = ImGui::GetWindowWidth();
    if (ww > tw)
        ImGui::SetCursorPosX((ww - tw) * 0.5f);
    ImGui::TextColored(col, "%s", text);
}

// Buttons in three moods: go (green), stop (red) and quiet (the default).
bool colored_button(const char* label, const ImVec2& size, const ImVec4& base)
{
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(base.x * 1.18f + 0.04f, base.y * 1.18f + 0.04f, base.z * 1.18f + 0.04f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(base.x * 0.85f, base.y * 0.85f, base.z * 0.85f, 1.0f));
    bool pressed;
    {
        FontScope f(g_fontBold);
        pressed = ImGui::Button(label, size);
    }
    ImGui::PopStyleColor(3);
    return pressed;
}
bool go_button(const char* label, const ImVec2& size)
{
    return colored_button(label, size, ImVec4(0.22f, 0.56f, 0.15f, 1.0f));
}
bool stop_button(const char* label, const ImVec2& size)
{
    return colored_button(label, size, ImVec4(0.62f, 0.19f, 0.17f, 1.0f));
}

// A rounded panel that grows to fit what is drawn in it.
bool begin_card(const char* id)
{
    const float fs = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::kCard);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 1.0f, fs * 0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, fs * 0.7f);
    const bool open = ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                                        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    return open;
}
void end_card()
{
    ImGui::EndChild();
    ImGui::Spacing();
}

// A turning arc: something is happening and nothing needs doing.
void spinner(const ImVec4& col)
{
    const float fs = ImGui::GetFontSize();
    const float r = fs * 0.55f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(r * 2.0f + 2.0f, fs));
    const ImVec2 c(p.x + r + 1.0f, p.y + fs * 0.5f);
    const float t = (float)ImGui::GetTime() * 6.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PathClear();
    dl->PathArcTo(c, r, t, t + 4.4f, 24);
    dl->PathStroke(ImGui::GetColorU32(col), 0, fs * 0.16f);
}

// A rounded label with a coloured dot: the online status.
float pill_width(const char* text)
{
    return ImGui::CalcTextSize(text).x + ImGui::GetFontSize() * 2.2f;
}
void pill(const char* text, const ImVec4& dot)
{
    const float fs = ImGui::GetFontSize();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ts.x + fs * 2.2f, fs * 1.8f);
    const ImVec2 q(p.x + size.x, p.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.45f)), size.y * 0.5f);
    dl->AddRect(p, q, ImGui::GetColorU32(with_alpha(dot, 0.6f)), size.y * 0.5f, 0, 1.5f);
    dl->AddCircleFilled(ImVec2(p.x + fs * 0.85f, p.y + size.y * 0.5f), fs * 0.27f, ImGui::GetColorU32(dot));
    dl->AddText(ImVec2(p.x + fs * 1.45f, p.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(ui::kText), text);
    ImGui::Dummy(size);
}

// One line for "where am I online", shared by the menu header and the corner badge.
struct NetStatus
{
    char text[96];
    ImVec4 color;
    bool show;   // worth a corner badge while playing
};
NetStatus net_status()
{
    NetStatus s;
    s.show = true;
    NetplayManager& m = NetplayManager::Instance();
    NetplayLobby& l = NetplayLobby::Instance();
    if (m.IsActive())
    {
        const unsigned ping = m.GetPingMs();
        s.color = ping <= 80 ? ui::kLime : (ping <= 150 ? ui::kYellow : ui::kOrange);
        std::snprintf(s.text, sizeof s.text, "Online \xC2\xB7 Player %d \xC2\xB7 %u ms", m.GetLocalSlot() + 1, ping);
    }
    else if (l.GetState() == LOBBY_STATE_HOSTING)
    {
        s.color = ui::kYellow;
        std::snprintf(s.text, sizeof s.text, "Room %s \xC2\xB7 waiting for your friend", l.GetRoomCode());
    }
    else if (l.GetState() == LOBBY_STATE_JOINING || m.GetState() == NETPLAY_STATE_CONNECTING)
    {
        s.color = ui::kBlue;
        std::snprintf(s.text, sizeof s.text, "Connecting...");
    }
    else
    {
        s.color = ui::kDim;
        std::snprintf(s.text, sizeof s.text, "Offline");
        s.show = false;
    }
    return s;
}

// The menu's left column: a large row with a title and a line under it.
bool nav_item(const char* label, const char* sub, bool selected)
{
    const float fs = ImGui::GetFontSize();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, fs * 2.9f);
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##nav", size);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImVec2 after = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected || hovered)
        dl->AddRectFilled(a, b,
                          ImGui::GetColorU32(selected ? with_alpha(ui::kLime, 0.15f) : ImVec4(1.0f, 1.0f, 1.0f, 0.05f)),
                          fs * 0.5f);
    if (selected)
        dl->AddRectFilled(a, ImVec2(a.x + fs * 0.22f, b.y), ImGui::GetColorU32(ui::kLime), fs * 0.11f);
    ImGui::SetCursorScreenPos(ImVec2(a.x + fs * 0.9f, a.y + fs * 0.32f));
    {
        FontScope f(g_fontBold);
        ImGui::TextColored(selected ? ui::kLime : ui::kText, "%s", label);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x + fs * 0.9f, ImGui::GetCursorScreenPos().y - fs * 0.2f));
    ImGui::TextColored(ui::kDim, "%s", sub);
    ImGui::SetCursorScreenPos(after);
    ImGui::Dummy(ImVec2(0.0f, fs * 0.2f));
    return pressed;
}

// A banner over the game (countdown, waiting, connection trouble): a dark
// rounded card with an accent border, centred at a fraction of the screen
// height.  Always pair with ImGui::End().
bool banner_begin(const char* id, float yFrac, const ImVec4& accent)
{
    const ImGuiIO& io = ImGui::GetIO();
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * yFrac), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.035f, 0.045f, 0.08f, 0.88f));
    ImGui::PushStyleColor(ImGuiCol_Border, with_alpha(accent, 0.75f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, fs * 0.9f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 1.8f, fs * 0.9f));
    const bool open = ImGui::Begin(id, nullptr,
                                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    return open;
}


// ---------------------------------------------------------------------------
// Player preferences kept on this PC (strikers-prefs.ini beside the game):
// which controller player 1 uses, and the sound levels.  The game's own save
// holds its volumes too, but only when the game itself saves, so these are
// put back once the main menu is reached.
// ---------------------------------------------------------------------------
struct Prefs
{
    bool  loaded = false;
    char  device[128] = "";   // "" automatic, "keyboard" keyboard only, else a controller's name
    int   deviceNth = 0;      // which of several controllers with that name
    int   keyboard = 1;       // the keyboard works as well as the controller
    int   music = -1, sfx = -1, voice = -1;   // 0..10, -1 = leave the game's own
    float master = -1.0f;     // 0..2, -1 = leave strikers.ini's
    bool  volumesApplied = false;
    float uiScale = 1.0f;     // the menu's text size (Display page)
};
Prefs s_prefs;

bool keyboard_pref_active() { return s_prefs.keyboard != 0 || std::strcmp(s_prefs.device, "keyboard") == 0; }

bool online_can_start(const char** where);   // below

const char* prefs_path()
{
    static char s_path[1024] = "";
    if (s_path[0] == '\0')
    {
        const char* base = SDL_GetBasePath();
        std::snprintf(s_path, sizeof s_path, "%sstrikers-prefs.ini", base != nullptr ? base : "");
    }
    return s_path;
}

void prefs_save()
{
    FILE* f = std::fopen(prefs_path(), "w");
    if (f == nullptr)
        return;
    std::fprintf(f, "# Kept by the game's F1 menu (Controls and Sound tabs).\n");
    std::fprintf(f, "device=%s\n", s_prefs.device);
    std::fprintf(f, "device_nth=%d\n", s_prefs.deviceNth);
    std::fprintf(f, "keyboard=%d\n", s_prefs.keyboard);
    std::fprintf(f, "music=%d\nsfx=%d\nvoice=%d\n", s_prefs.music, s_prefs.sfx, s_prefs.voice);
    std::fprintf(f, "master=%.2f\n", (double)s_prefs.master);
    std::fprintf(f, "ui_scale=%.2f\n", (double)s_prefs.uiScale);
    std::fclose(f);
}

void prefs_load()
{
    s_prefs.loaded = true;
    FILE* f = std::fopen(prefs_path(), "r");
    if (f == nullptr)
        return;
    char line[256];
    while (std::fgets(line, sizeof line, f) != nullptr)
    {
        char* nl = std::strpbrk(line, "\r\n");
        if (nl != nullptr)
            *nl = '\0';
        char* eq = std::strchr(line, '=');
        if (line[0] == '#' || eq == nullptr)
            continue;
        *eq = '\0';
        const char* k = line;
        const char* v = eq + 1;
        if (std::strcmp(k, "device") == 0)          copy_into(s_prefs.device, sizeof s_prefs.device, v);
        else if (std::strcmp(k, "device_nth") == 0) s_prefs.deviceNth = std::atoi(v);
        else if (std::strcmp(k, "keyboard") == 0)   s_prefs.keyboard = std::atoi(v) != 0;
        else if (std::strcmp(k, "music") == 0)      s_prefs.music = std::atoi(v);
        else if (std::strcmp(k, "sfx") == 0)        s_prefs.sfx = std::atoi(v);
        else if (std::strcmp(k, "voice") == 0)      s_prefs.voice = std::atoi(v);
        else if (std::strcmp(k, "master") == 0)     s_prefs.master = (float)std::atof(v);
        else if (std::strcmp(k, "ui_scale") == 0)
        {
            const float sc = (float)std::atof(v);
            s_prefs.uiScale = sc >= 0.7f && sc <= 1.6f ? sc : 1.0f;
        }
    }
    std::fclose(f);
}

// The controller index for a name (the nth with that name), or -1.
int controller_index_for(const char* name, int nth)
{
    const u32 n = PADCount();
    int seen = 0;
    for (u32 i = 0; i < n; i++)
    {
        const char* ci = PADGetNameForControllerIndex(i);
        if (ci != nullptr && std::strcmp(ci, name) == 0)
        {
            if (seen == nth)
                return (int)i;
            seen++;
        }
    }
    return -1;
}

// Which of the controllers with its name index `idx` is (0 for the first).
int controller_nth(u32 idx)
{
    const char* name = PADGetNameForControllerIndex(idx);
    int nth = 0;
    for (u32 i = 0; i < idx; i++)
    {
        const char* ci = PADGetNameForControllerIndex(i);
        if (name != nullptr && ci != nullptr && std::strcmp(ci, name) == 0)
            nth++;
    }
    return nth;
}

// Put player 1 on the chosen device.  Cheap, run a few times a second, so a
// controller plugged in later is picked up too.
void apply_device_pref()
{
    if (s_prefs.device[0] == '\0')
        return;   // automatic: the game's own choice
    const s32 cur = PADGetIndexForPort(0);
    if (std::strcmp(s_prefs.device, "keyboard") == 0)
    {
        if (cur >= 0)
            PADClearPort(0);
        return;
    }
    const int want = controller_index_for(s_prefs.device, s_prefs.deviceNth);
    if (want >= 0 && cur != want)
        PADSetPortForIndex((u32)want, 0);
}

void prefs_tick()
{
    if (!s_prefs.loaded)
    {
        prefs_load();
        if (s_prefs.master >= 0.0f)
            PortAudioSetMasterGain(s_prefs.master);
        if (!s_menuOpen)
            PADSetKeyboardActive(0, keyboard_pref_active() ? TRUE : FALSE);
    }
    static unsigned s_tick = 0;
    if ((s_tick++ % 20) == 0)
        apply_device_pref();
    // The game's volumes, once its save has been read (it is by the main menu).
    if (!s_prefs.volumesApplied && online_can_start(nullptr))
    {
        s_prefs.volumesApplied = true;
        if (s_prefs.music >= 0 && s_prefs.sfx >= 0 && s_prefs.voice >= 0)
            PortSetGameVolumes(s_prefs.music, s_prefs.sfx, s_prefs.voice);
    }
}

void draw_device_picker()
{
    heading("PLAYER 1 PLAYS WITH");
    const u32 n = PADCount();
    const s32 cur = PADGetIndexForPort(0);

    // Entries: automatic, keyboard only, then every controller.
    const int kFixed = 2;
    int selected = 0;
    if (std::strcmp(s_prefs.device, "keyboard") == 0)
        selected = 1;
    else if (s_prefs.device[0] != '\0')
    {
        const int idx = controller_index_for(s_prefs.device, s_prefs.deviceNth);
        selected = idx >= 0 ? kFixed + idx : 0;
    }

    char preview[160];
    if (selected == 0)
    {
        const char* curName = cur >= 0 ? PADGetNameForControllerIndex((u32)cur) : nullptr;
        std::snprintf(preview, sizeof preview, "Automatic (%s)", curName ? curName : "keyboard");
    }
    else if (selected == 1)
        std::snprintf(preview, sizeof preview, "Keyboard only");
    else
    {
        const char* nm = PADGetNameForControllerIndex((u32)(selected - kFixed));
        std::snprintf(preview, sizeof preview, "%s", nm ? nm : "Controller");
    }

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
    if (ImGui::BeginCombo("##device", preview))
    {
        if (ImGui::Selectable("Automatic (the first controller found)", selected == 0))
        {
            s_prefs.device[0] = '\0';
            s_prefs.deviceNth = 0;
            if (PADGetIndexForPort(0) < 0 && n > 0)
                PADSetPortForIndex(0, 0);
            prefs_save();
        }
        if (ImGui::Selectable("Keyboard only", selected == 1))
        {
            copy_into(s_prefs.device, sizeof s_prefs.device, "keyboard");
            s_prefs.deviceNth = 0;
            s_prefs.keyboard = 1;
            if (!s_menuOpen)
                PADSetKeyboardActive(0, TRUE);
            apply_device_pref();
            prefs_save();
        }
        for (u32 i = 0; i < n; i++)
        {
            const char* nm = PADGetNameForControllerIndex(i);
            char label[180];
            std::snprintf(label, sizeof label, "%s##pad%u", nm ? nm : "Controller", i);
            if (ImGui::Selectable(label, selected == kFixed + (int)i))
            {
                copy_into(s_prefs.device, sizeof s_prefs.device, nm ? nm : "");
                s_prefs.deviceNth = controller_nth(i);
                apply_device_pref();
                prefs_save();
            }
        }
        ImGui::EndCombo();
    }

    const bool kbOnly = selected == 1;
    ImGui::BeginDisabled(kbOnly);
    bool kb = kbOnly || s_prefs.keyboard != 0;
    if (ImGui::Checkbox("The keyboard works too", &kb))
    {
        s_prefs.keyboard = kb ? 1 : 0;
        if (!s_menuOpen)
            PADSetKeyboardActive(0, kb ? TRUE : FALSE);
        prefs_save();
    }
    ImGui::EndDisabled();
    if (n == 0)
        ImGui::TextDisabled("No controller connected. Plug one in and it appears in the list.");
    else
        ImGui::TextDisabled("%u controller%s connected. Your choice is remembered on this PC.",
                            n, n == 1 ? "" : "s");
    ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// Sound: the game's own three volumes (as in its Options > Audio screen) and
// the port's overall level.
// ---------------------------------------------------------------------------
void draw_sound_tab()
{
    if (!PortAudioDeviceOpen())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "Sound is switched off.");
        ImGui::TextDisabled("Turn it on in Mario Strikers Settings (Audio), or remove");
        ImGui::TextDisabled("\"audio = 0\" from strikers.ini, then restart the game.");
        return;
    }

    heading("VOLUME");
    {
        float master = PortAudioGetMasterGain() * 100.0f;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 15.0f);
        if (ImGui::SliderFloat("Overall", &master, 0.0f, 200.0f, "%.0f%%"))
        {
            s_prefs.master = master / 100.0f;
            PortAudioSetMasterGain(s_prefs.master);
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            prefs_save();
        ImGui::SameLine();
        if (ImGui::SmallButton(master > 0.0f ? "Mute" : "Unmute"))
        {
            s_prefs.master = master > 0.0f ? 0.0f : 1.0f;
            PortAudioSetMasterGain(s_prefs.master);
            prefs_save();
        }
    }

    int vol[3] = { 10, 10, 10 };
    if (!PortGetGameVolumes(&vol[0], &vol[1], &vol[2]))
    {
        ImGui::TextDisabled("(the game's own volumes appear once it has started)");
        return;
    }
    const char* const kNames[3] = { "Music", "Effects", "Commentary" };
    bool changed = false, done = false;
    for (int g = 0; g < 3; g++)
    {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 15.0f);
        if (ImGui::SliderInt(kNames[g], &vol[g], 0, 10))
            changed = true;
        if (ImGui::IsItemDeactivatedAfterEdit())
            done = true;
    }
    if (changed)
        PortSetGameVolumes(vol[0], vol[1], vol[2]);
    if (done)
    {
        s_prefs.music = vol[0];
        s_prefs.sfx = vol[1];
        s_prefs.voice = vol[2];
        s_prefs.volumesApplied = true;
        prefs_save();
    }
    if (ImGui::Button("Defaults"))
    {
        PortSetGameVolumes(10, 10, 10);
        PortAudioSetMasterGain(1.0f);
        s_prefs.music = s_prefs.sfx = s_prefs.voice = 10;
        s_prefs.master = 1.0f;
        prefs_save();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("The same three levels as the game's Options > Audio screen.");
    ImGui::TextDisabled("Remembered on this PC. Online, each player sets their own.");
}


// ---------------------------------------------------------------------------
// Controls: change which button on your controller does what.  Uses Aurora's
// controller mapping (saved in its settings folder, loaded at start), so it
// works in every mode, online included.
// ---------------------------------------------------------------------------
struct ControlsCapture
{
    int  slot = -1;          // index into kBindable, -1 = not capturing
    bool waitingRelease = true;
    u64  sinceUs = 0;
    char message[128] = "";
};
ControlsCapture s_ctl;

struct Bindable
{
    PADButton button;
    const char* name;
    const char* what;
};
const Bindable kBindable[] = {
    { PAD_BUTTON_A, "A", "pass" },
    { PAD_BUTTON_B, "B", "shoot (hold to charge)" },
    { PAD_BUTTON_X, "X", "" },
    { PAD_BUTTON_Y, "Y", "" },
    { PAD_TRIGGER_Z, "Z", "use item" },
    { PAD_TRIGGER_L, "L", "" },
    { PAD_TRIGGER_R, "R", "" },
    { PAD_BUTTON_START, "START", "pause / skip" },
    { PAD_BUTTON_UP, "D-pad up", "" },
    { PAD_BUTTON_DOWN, "D-pad down", "" },
    { PAD_BUTTON_LEFT, "D-pad left", "" },
    { PAD_BUTTON_RIGHT, "D-pad right", "" },
};
const int kBindableCount = (int)(sizeof kBindable / sizeof kBindable[0]);

const char* native_name(u32 native)
{
    if (native == PAD_NATIVE_BUTTON_INVALID)
        return "(analog / not set)";
    const char* n = PADGetNativeButtonName(native);
    return (n != nullptr && *n) ? n : "?";
}

void controls_end_capture(const char* msg)
{
    s_ctl.slot = -1;
    PADBlockInput(false);
    std::snprintf(s_ctl.message, sizeof s_ctl.message, "%s", msg);
}

void draw_controls_tab()
{
    const ImVec4 kGreen(0.3f, 1.0f, 0.3f, 1.0f), kYellow(1.0f, 0.85f, 0.25f, 1.0f);
    draw_device_picker();
    heading("BUTTONS");
    const char* padName = PADGetName(0);
    u32 count = 0;
    PADButtonMapping* maps = PADGetButtonMappings(0, &count);
    if (maps == nullptr || count == 0)
    {
        ImGui::TextColored(kYellow, "No controller on player 1. Choose one above, or plug one in.");
        ImGui::TextDisabled("(The keyboard keys are listed in the Input tab.)");
        if (s_ctl.slot >= 0)
            controls_end_capture("");
        return;
    }
    ImGui::Text("Controller: %s", padName ? padName : "?");
    ImGui::TextDisabled("Click Change, then press the button you want on your controller.");
    ImGui::TextDisabled("Changes are saved right away and kept for next time.");
    ImGui::Spacing();

    // Waiting for a button?
    if (s_ctl.slot >= 0)
    {
        const s32 native = PADGetNativeButtonPressed(0);
        const u64 now = NetplayGetTimeUs();
        if (s_ctl.waitingRelease)
        {
            if (native < 0)
                s_ctl.waitingRelease = false;
        }
        else if (native >= 0)
        {
            const Bindable& b = kBindable[s_ctl.slot];
            u32 oldNative = PAD_NATIVE_BUTTON_INVALID;
            for (u32 i = 0; i < count; i++)
                if (maps[i].padButton == b.button)
                    oldNative = maps[i].nativeButton;
            // Whatever used that button before takes this one's old button (a swap).
            for (u32 i = 0; i < count; i++)
            {
                if (maps[i].padButton != b.button && maps[i].nativeButton == (u32)native)
                {
                    PADButtonMapping other = maps[i];
                    other.nativeButton = oldNative;
                    PADSetButtonMapping(0, other);
                }
            }
            PADButtonMapping m;
            m.nativeButton = (u32)native;
            m.padButton = b.button;
            PADSetButtonMapping(0, m);
            PADSerializeMappings();
            char msg[128];
            std::snprintf(msg, sizeof msg, "%s is now on your %s button.", b.name, native_name((u32)native));
            controls_end_capture(msg);
            maps = PADGetButtonMappings(0, &count);
        }
        if (s_ctl.slot >= 0 && now - s_ctl.sinceUs > 8000000ULL)
            controls_end_capture("Nothing was pressed - no change.");
    }

    if (ImGui::BeginTable("##binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("GameCube button");
        ImGui::TableSetupColumn("your controller");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (int k = 0; k < kBindableCount; k++)
        {
            const Bindable& b = kBindable[k];
            u32 native = PAD_NATIVE_BUTTON_INVALID;
            for (u32 i = 0; i < count; i++)
                if (maps[i].padButton == b.button)
                    native = maps[i].nativeButton;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (b.what[0])
                ImGui::Text("%s  (%s)", b.name, b.what);
            else
                ImGui::Text("%s", b.name);
            ImGui::TableNextColumn();
            if (s_ctl.slot == k)
                ImGui::TextColored(kYellow, "press a button...");
            else
                ImGui::Text("%s", native_name(native));
            ImGui::TableNextColumn();
            ImGui::PushID(k);
            if (s_ctl.slot == k)
            {
                if (ImGui::SmallButton("Cancel"))
                    controls_end_capture("");
            }
            else
            {
                ImGui::BeginDisabled(s_ctl.slot >= 0);
                if (ImGui::SmallButton("Change"))
                {
                    s_ctl.slot = k;
                    s_ctl.waitingRelease = true;
                    s_ctl.sinceUs = NetplayGetTimeUs();
                    s_ctl.message[0] = '\0';
                    PADBlockInput(true);   // the game ignores the controller meanwhile
                }
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (s_ctl.message[0])
        ImGui::TextColored(kGreen, "%s", s_ctl.message);
    ImGui::Spacing();
    ImGui::BeginDisabled(s_ctl.slot >= 0);
    if (ImGui::Button("Reset to default"))
    {
        PADRestoreDefaultMapping(0);
        PADSerializeMappings();
        std::snprintf(s_ctl.message, sizeof s_ctl.message, "Back to the default buttons.");
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Sticks and analog triggers keep their usual places. Online, only");
    ImGui::TextDisabled("your own controller changes - your friend sets theirs.");
}

void draw_input_tab()
{
    ImGui::SeparatorText("pads");
    const unsigned int count = PADCount();
    ImGui::Text("%u controller%s connected", count, count == 1 ? "" : "s");
    for (unsigned int i = 0; i < count && i < 4; i++)
    {
        const char* name = PADGetName(i);
        ImGui::Text("  port %u: %s", i, name != nullptr ? name : "?");
    }

    ImGui::SeparatorText("pad 0, as the game read it this frame");
    {
        const PadSnapshot& p = s_pad[0];
        if (p.err != PAD_ERR_NONE)
        {
            ImGui::TextDisabled("err %d (no controller: no gamepad, and the keyboard is %s)",
                                p.err, s_menuOpen ? "detached while the menu is open" : "unbound");
        }
        struct
        {
            unsigned int bit;
            const char* name;
        } const kBits[] = {
            { PAD_BUTTON_A, "A" },     { PAD_BUTTON_B, "B" },     { PAD_BUTTON_X, "X" },
            { PAD_BUTTON_Y, "Y" },     { PAD_TRIGGER_Z, "Z" },    { PAD_TRIGGER_L, "L" },
            { PAD_TRIGGER_R, "R" },    { PAD_BUTTON_START, "START" },
            { PAD_BUTTON_UP, "up" },   { PAD_BUTTON_DOWN, "down" },
            { PAD_BUTTON_LEFT, "left" }, { PAD_BUTTON_RIGHT, "right" },
        };
        for (size_t i = 0; i < sizeof kBits / sizeof kBits[0]; i++)
        {
            const bool on = (p.buttons & kBits[i].bit) != 0;
            if (i != 0)
                ImGui::SameLine();
            if (on)
                ImGui::Text("[%s]", kBits[i].name);
            else
                ImGui::TextDisabled(" %s ", kBits[i].name);
        }
        ImGui::Text("stick %4d %4d   c-stick %4d %4d   L %3d  R %3d", p.stickX, p.stickY,
                    p.substickX, p.substickY, p.triggerL, p.triggerR);
    }

    ImGui::SeparatorText("press");
    ImGui::TextDisabled("Held for six frames on pad 0, merged with the real pad. The same");
    ImGui::TextDisabled("thing STRIKERS_AUTOPRESS does.");
    struct
    {
        unsigned int bit;
        const char* name;
    } const kPress[] = {
        { PAD_BUTTON_A, "A" },      { PAD_BUTTON_B, "B" },        { PAD_BUTTON_X, "X" },
        { PAD_BUTTON_Y, "Y" },      { PAD_TRIGGER_Z, "Z" },       { PAD_BUTTON_START, "Start" },
        { PAD_BUTTON_UP, "Up" },    { PAD_BUTTON_DOWN, "Down" },  { PAD_BUTTON_LEFT, "Left" },
        { PAD_BUTTON_RIGHT, "Right" },
    };
    for (size_t i = 0; i < sizeof kPress / sizeof kPress[0]; i++)
    {
        if (i != 0)
            ImGui::SameLine();
        if (ImGui::SmallButton(kPress[i].name))
            PortInputPress(kPress[i].bit, 0);
    }

    ImGui::SeparatorText("record");
    static char s_recPath[256] = "/tmp/strikers.pad";
    const char* recording = PortInputRecordPath();
    if (recording != nullptr)
    {
        ImGui::Text("recording to %s", recording);
        if (ImGui::SmallButton("stop recording"))
            PortInputRecordStop();
    }
    else
    {
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText("##recpath", s_recPath, sizeof s_recPath);
        ImGui::SameLine();
        if (ImGui::SmallButton("start recording"))
            PortInputRecordStart(s_recPath);
    }
    help("One line per change of pad state, frames counted from boot. STRIKERS_REPLAY_INPUT "
         "plays it back from boot; the replay below plays it from now.");

    ImGui::SeparatorText("replay");
    static char s_repPath[256] = "run.pad";
    unsigned long rcount = 0, rcursor = 0, rfirst = 0, rlast = 0;
    const char* rpath = nullptr;
    if (PortInputReplayStatus(&rcount, &rcursor, &rfirst, &rlast, &rpath))
    {
        ImGui::Text("%s: state %lu / %lu   frames %lu .. %lu   now %lu", rpath, rcursor + 1,
                    rcount, rfirst, rlast, PortInputFrame());
        if (ImGui::SmallButton("stop replay"))
            PortInputReplayStop();
    }
    else
    {
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText("##reppath", s_repPath, sizeof s_repPath);
        ImGui::SameLine();
        if (ImGui::SmallButton("start replay"))
            PortInputReplayStart(s_repPath);
    }

    ImGui::SeparatorText("keyboard");
    ImGui::TextDisabled("Detached from the pad while this menu is open.");
    static const struct
    {
        const char* key;
        const char* what;
    } kKeys[] = {
        { "X / Z / C / V", "A / B / X / Y" },
        { "Enter", "Start" },
        { "Space", "Z" },
        { "Q / E", "L / R" },
        { "W A S D", "control stick" },
        { "I J K L", "C stick" },
        { "arrows", "D-pad" },
        { "F1", "this menu" },
        { "F2", "the compact overlay" },
        { "F3 / F4", "freeze / quarter speed" },
        { "F11", "fullscreen" },
        { "F12 or P", "screenshot, to STRIKERS_SHOT_DIR" },
    };
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
    {
        for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", kKeys[i].key);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", kKeys[i].what);
        }
        ImGui::EndTable();
    }
}

void draw_system_tab()
{
    ImGui::SeparatorText("display");
    {
        bool fs = is_fullscreen();
        if (ImGui::Checkbox("fullscreen (F11)", &fs))
            set_fullscreen(fs);

        // No getter for either of these in Aurora. The vsync state lives at file scope
        // (vsync_state) because PDBG_SET_VSYNC changes it too; the slider owns its own from the
        // environment's answer onward.
        bool vsyncOn = vsync_state();
        if (ImGui::Checkbox("vsync", &vsyncOn))
            set_vsync(vsyncOn);
        help("Re-selects the present mode. The frame limiter's margin follows: with vsync "
             "on it sits 5% above the refresh rate so the two pacers never fight.");

        float s_scale = PortRenderScale() > 0.0f ? PortRenderScale() : 1.0f;
        const AuroraWindowSize fb = aurora_window_size();
        char scaleFmt[48];
        snprintf(scaleFmt, sizeof scaleFmt, "%%.2fx (%ux%u)", fb.fb_width, fb.fb_height);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("internal resolution", &s_scale, 0.25f, 4.0f, scaleFmt))
            PortSetRenderScale(s_scale);

        double limitHz = 0.0, displayHz = 0.0;
        int vsync = 0, overridden = 0;
        PortFrameLimitInfo(&limitHz, &displayHz, &vsync, &overridden);
        ImGui::Text("limiter %.2f Hz%s   display %.0f Hz%s", limitHz,
                    limitHz == 0.0 ? " (uncapped)" : "", displayHz, vsync ? " vsync" : "");
        if (ImGui::SmallButton("59.94"))
            PortSetFrameLimit(59.94);
        ImGui::SameLine();
        if (ImGui::SmallButton("60"))
            PortSetFrameLimit(60.0);
        ImGui::SameLine();
        if (ImGui::SmallButton("120"))
            PortSetFrameLimit(120.0);
        ImGui::SameLine();
        if (ImGui::SmallButton("uncapped"))
            PortSetFrameLimit(0.0);
        ImGui::SameLine();
        ImGui::BeginDisabled(!overridden);
        if (ImGui::SmallButton("follow display"))
            PortSetFrameLimit(-1.0);
        ImGui::EndDisabled();
        help("The frame limiter's period. 59.94 is console pacing. The simulation is "
             "not tied to it; STRIKERS_FIXED_DT is.");
    }

    ImGui::SeparatorText("capture");
    static char s_shotPath[256] = "/tmp/strikers-menu-shot.ppm";
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputText("##shot", s_shotPath, sizeof s_shotPath);
    ImGui::SameLine();
    if (ImGui::SmallButton("capture frame"))
        aurora_capture_frame(s_shotPath);
    help("Binary PPM of the game's render target, without this menu: Aurora captures "
         "before ImGui is composited. F12 writes numbered shots to STRIKERS_SHOT_DIR.");

    ImGui::SeparatorText("texture packs");
    {
        int count = 0;
        const char* folder = nullptr;
        for (int i = 0; (folder = PortTexturesFolder(i, &count)) != nullptr; i++)
            ImGui::Text("%5d  %s", count, folder);
        if (PortTexturesFolder(0, nullptr) == nullptr)
            ImGui::TextDisabled("no textures folder beside the game or in the user folder");
        if (ImGui::SmallButton("reload textures"))
            PortTexturesReload();
        help("Looks for the folders again and rescans them, so added or edited files show "
             "without a restart. Later folders in the list win.");
        bool dump = PortTextureDumpEnabled() != 0;
        if (ImGui::Checkbox("dump textures as they load", &dump))
            PortTextureDumpEnable(dump ? 1 : 0);
        if (dump)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%u written", PortTextureDumpWritten());
            ImGui::TextDisabled("%s", PortTextureDumpDir());
        }
        help("PNGs named for a texture pack, one folder per disc file. Only textures loaded "
             "after this is ticked; STRIKERS_TEXTURE_DUMP=1 dumps from the start.");
    }

    ImGui::SeparatorText("memory");
    {
        size_t used = 0, total = 0;
        port_region_stats(&used, &total);
        ImGui::Text("game region %.1f / %.1f MiB handed out", used / 1048576.0, total / 1048576.0);
        help("The port's stand-in for the console's main RAM: a bump allocator the game's "
             "own allocators carve up.");

        PortGfxArena arena;
        if (PortGfxArenaStats(&arena))
        {
            ImGui::Text("graphics arena %u / %u KB   marker level %d", arena.used >> 10,
                        arena.size >> 10, arena.markerLevel);
            if (ImGui::BeginTable("arena", 1 + arena.typeCount,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("level");
                for (int t = 0; t < arena.typeCount; t++)
                    ImGui::TableSetupColumn(arena.typeNames[t]);
                ImGui::TableHeadersRow();
                for (int level = 0; level <= arena.markerLevel && level < 8; level++)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", level);
                    for (int t = 0; t < arena.typeCount; t++)
                    {
                        ImGui::TableNextColumn();
                        if (arena.bytes[level][t] != 0)
                            ImGui::Text("%u KB", arena.bytes[level][t] >> 10);
                    }
                }
                ImGui::EndTable();
            }
            help("The game's resource memory, by marker level and type: a level whose bytes "
                 "do not return to zero when it is released is a leak, by definition. "
                 "The same table prints when the arena is exhausted.");
        }
    }

    ImGui::SeparatorText("logging");
    ImGui::TextDisabled("Switches the tree reads on every use, so they take effect at once.");
    for (size_t i = 0; i < sizeof kLiveEnv / sizeof kLiveEnv[0]; i++)
    {
        bool on = std::getenv(kLiveEnv[i].var) != nullptr;
        if (ImGui::Checkbox(kLiveEnv[i].var, &on))
        {
            set_env(kLiveEnv[i].var, on ? "1" : nullptr);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", kLiveEnv[i].what);
    }

    ImGui::SeparatorText("session");
    {
        const char* cfg = PortConfigPath();
        ImGui::Text("strikers.ini: %s", cfg != nullptr ? cfg : "(none found)");
        ImGui::Text("backend %d   frame %lu", (int)aurora_get_backend(), PortInputFrame());
        static bool s_pauseOnFocusLost = false;
        if (ImGui::Checkbox("pause when the window loses focus", &s_pauseOnFocusLost))
            aurora_set_pause_on_focus_lost(s_pauseOnFocusLost);
        if (ImGui::SmallButton("quit"))
            s_quit = true;
    }
}

void draw_session_tab()
{
    const PortDebugSession& s = s_session;
    const PortDebugMatch& m = s_match;

    ImGui::Text("task state 0x%x  %s", s.taskState, task_state_name(s.taskState));
    ImGui::Text("scene   %s", s_scene);

    ImGui::SeparatorText("front-end stack");
    if (s.feDepth == 0)
        ImGui::TextDisabled("empty");
    for (int i = 0; i < s.feDepth && i < 8; i++)
        ImGui::Text("  %d  %s", s.feStack[i], scene_name(s.feStack[i]));
    ImGui::SeparatorText("overlay stack");
    if (s.overlayDepth == 0)
        ImGui::TextDisabled("empty");
    for (int i = 0; i < s.overlayDepth && i < 8; i++)
        ImGui::Text("  %d  %s", s.overlayStack[i], scene_name(s.overlayStack[i]));

    ImGui::SeparatorText("in a match");
    ImGui::BeginDisabled(!m.valid);
    if (ImGui::SmallButton(s.inPauseMenu ? "leave pause menu" : "open pause menu"))
        cmd(PDBG_PAUSE_MENU, s.inPauseMenu ? 0 : 1);
    help("FrontEnd::EnterMenuState, which is what Start does.");
    ImGui::SameLine();
    if (ImGui::SmallButton("return to front end"))
        cmd(PDBG_RETURN_TO_FE);
    help("The pause menu's forfeit path.");
    ImGui::EndDisabled();

    ImGui::SeparatorText("start a match from here");
    ImGui::TextDisabled("The `skipfe` boot path, from the front end: sets the line-up on");
    ImGui::TextDisabled("GameInfoManager and asks the task manager for the match state.");
    static int s_team[2] = { 3, 2 };
    static int s_side[2] = { 0, 1 };
    static int s_stad = 1;
    static int s_human = 1;   // 0 none, 1 home, 2 away
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("home", &s_team[0], kTeams, 9);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::Combo("##sk1", &s_side[0], kSidekicks, 4);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("away", &s_team[1], kTeams, 9);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::Combo("##sk2", &s_side[1], kSidekicks, 4);
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo("stadium", &s_stad, kStadiums, 7);
    {
        const char* const kSides[] = { "AI vs AI", "pad 0 home", "pad 0 away" };
        ImGui::SetNextItemWidth(150.0f);
        ImGui::Combo("control", &s_human, kSides, 3);
    }
    ImGui::BeginDisabled(m.valid || s.taskState != 4);
    if (ImGui::Button("start friendly"))
        cmd(PDBG_START_MATCH, s_team[0], s_team[1], s_stad, (float)s_side[0],
            (float)s_side[1], (float)(s_human - 1));
    ImGui::EndDisabled();
    if (s.taskState != 4 && !m.valid)
        ImGui::TextDisabled("(only from the front end)");
}

// ---------------------------------------------------------------------------
// Online VS screen: shown as soon as both players are in the room. Each player
// picks a captain and sidekick, the host picks the stadium, both press READY,
// and the host starts the same match on both PCs.
// ---------------------------------------------------------------------------

const char* const kCaptainNames[]  = { "Daisy", "Donkey Kong", "Luigi", "Mario",
                                       "Peach", "Waluigi", "Wario", "Yoshi" };
const char* const kSidekickNames[] = { "Toad", "Koopa", "Hammer Bro", "Birdo" };
const char* const kStadiumNames[]  = { "Mario Stadium", "Peach & Toad Stadium", "DK & Daisy Stadium",
                                       "Wario Stadium", "Yoshi Stadium", "Super Stadium",
                                       "Forbidden Dome" };
const int kCaptainCount  = 8;   // kTeams without "mystery"
const int kSidekickCount = 4;
const int kStadiumCount  = 7;

const char* vs_name(const char* const* names, int count, int i)
{
    return (i >= 0 && i < count) ? names[i] : "?";
}

extern "C" int PortMatchExists(void);   // Game.cpp

// What the game says when it cannot start a match (PortDebugStartMatchResult codes).
const char* start_refusal_text(int code)
{
    switch (code)
    {
    case -1: return "it was not on the main menu";
    case -2: return "a match was still running";
    case -3: return "its menus were busy";
    case -5: return "the start command never ran";
    case -6: return "the match took too long to load";
    default: return "of an internal error";
    }
}

// Can this game start a match right now?  Mirrors the PDBG_START_MATCH checks
// in Game.cpp: front-end task state, no match, and the title or main menu on
// top of the scene stack.
bool online_can_start(const char** where)
{
    const PortDebugSession& s = s_session;
    int top = -1;
    if (s.feDepth > 0 && s.feDepth <= 8)
        top = s.feStack[s.feDepth - 1];
    if (where)
        *where = top >= 0 ? scene_name(top) : "loading";
    return s.taskState == 4 && !PortMatchExists() && (top == 2 || top == 3);
}

// Start bookkeeping, shared by host and guest.
struct OnlineStart
{
    u8   lastMatchId = 0;
    bool loading = false;
    u64  sinceUs = 0;
    char failText[160] = "";
    u64  failUntilUs = 0;
    bool returnToFE = false;   // opponent aborted while we may already be in the match
    u64  returnSinceUs = 0;
    u8   returnMatchId = 0;    // the match that abort was about
};
OnlineStart s_onlineStart;

extern "C" int PortDebugStartMatchResult(int* tag);

void online_fail(const char* text)
{
    std::snprintf(s_onlineStart.failText, sizeof s_onlineStart.failText, "%s", text);
    s_onlineStart.failUntilUs = NetplayGetTimeUs() + 10000000ULL;
    s_onlineStart.loading = false;
}

// Called every frame: follows a match start on this PC and backs out cleanly
// if either game could not start it.
void online_start_tick()
{
    NetplayManager& mgr = NetplayManager::Instance();
    OnlineStart& st = s_onlineStart;
    const u64 now = NetplayGetTimeUs();

    if (!mgr.IsActive())
    {
        st.loading = false;
        st.returnToFE = false;
        st.lastMatchId = mgr.GetMatchId();
        return;
    }

    if (mgr.GetMatchId() != st.lastMatchId)
    {
        // A new match was just started (by our host button or the host's packet). Match 0 is
        // a session that has just connected, with no match yet: nothing to load.
        st.lastMatchId = mgr.GetMatchId();
        if (st.lastMatchId == 0)
        {
            st.loading = false;
            return;
        }
        st.loading = true;
        st.sinceUs = now;
        st.failText[0] = '\0';
    }

    int remoteReason = 0;
    if (mgr.TakeRemoteAbort(&remoteReason))
    {
        char msg[160];
        std::snprintf(msg, sizeof msg, "Your friend's game couldn't start the match because %s.",
                      start_refusal_text(remoteReason));
        online_fail(msg);
        st.returnToFE = true;
        st.returnSinceUs = now;
        st.returnMatchId = mgr.GetMatchId();
    }

    if (st.returnToFE)
    {
        if (mgr.GetMatchId() != st.returnMatchId)
        {
            st.returnToFE = false;   // a newer match started since: that one is fine
        }
        else if (PortMatchExists())
        {
            cmd(PDBG_RETURN_TO_FE);
            st.returnToFE = false;
        }
        else if (now - st.returnSinceUs > 30000000ULL)
        {
            st.returnToFE = false;
        }
    }

    if (!st.loading)
        return;

    if (PortMatchExists())
    {
        st.loading = false;   // we are playing
        return;
    }

    int tag = -1;
    const int result = PortDebugStartMatchResult(&tag);
    const bool commandRan = (tag == (int)mgr.GetMatchId());

    int failCode = 0;
    if (commandRan && result < 0)
        failCode = result;
    else if (!commandRan && now - st.sinceUs > 3000000ULL)
        failCode = -5;
    else if (now - st.sinceUs > 45000000ULL)
        failCode = -6;

    if (failCode != 0)
    {
        char msg[160];
        std::snprintf(msg, sizeof msg, "Your game couldn't start the match because %s.",
                      start_refusal_text(failCode));
        online_fail(msg);
        mgr.SendMatchAbort(failCode);
    }
}

bool online_match_loading()
{
    return s_onlineStart.loading;
}

// ---------------------------------------------------------------------------
// The end of an online match.  The game's own winner screen offers Rematch,
// which restarts the match on this PC only and leaves the other one behind.
// Online, both games show the result for a few seconds and go back to team
// select instead, where the same picks are still chosen: READY again plays
// the rematch, from a fresh start both games share.
// ---------------------------------------------------------------------------
extern "C" int PortMatchIsOver(int* scoreHome, int* scoreAway);   // Game.cpp

bool s_rematchHint = false;   // shown on team select after a match

void online_end_tick()
{
    static u64 s_overSinceUs = 0;
    static bool s_inputBlocked = false;
    static bool s_returned = false;
    static u64 s_unblockAtUs = 0;
    const u64 kShowUs = 7000000ULL;
    // After going back, the controller stays away from the game a little longer: players are
    // usually still pressing A from the result screen, and on the game's own side-select screen
    // that confirmed "no sides chosen" and started a match on one PC only.
    const u64 kQuietAfterReturnUs = 3000000ULL;

    NetplayManager& mgr = NetplayManager::Instance();
    int home = 0, away = 0;
    const bool over = mgr.IsActive() && PortMatchIsOver(&home, &away) && s_session.taskState == 1;
    if (!over)
    {
        if (s_inputBlocked && (s_unblockAtUs == 0 || NetplayGetTimeUs() >= s_unblockAtUs))
        {
            PADBlockInput(false);
            s_inputBlocked = false;
            s_unblockAtUs = 0;
        }
        s_overSinceUs = 0;
        s_returned = false;
        if (PortMatchExists() && s_session.taskState == 2)
            s_rematchHint = false;   // the next match is on
        return;
    }

    const u64 now = NetplayGetTimeUs();
    if (s_overSinceUs == 0)
    {
        s_overSinceUs = now;
        PADBlockInput(true);   // the winner screen's own menu stays out of it
        s_inputBlocked = true;
        std::fprintf(stderr, "[netplay] match over (%d - %d): back to team select shortly\n", home, away);
    }
    const u64 elapsed = now - s_overSinceUs;
    if (elapsed >= kShowUs && !s_returned)
    {
        s_returned = true;
        s_rematchHint = true;
        s_unblockAtUs = now + kQuietAfterReturnUs;   // input comes back once on team select
        cmd(PDBG_RETURN_TO_FE);
        return;
    }
    if (s_returned)
        return;

    if (banner_begin("##online_full_time", 0.30f, ui::kLime))
    {
        big_text(g_fontTitle, 1.8f, ui::kLime, "FULL TIME", true);
        char score[32];
        std::snprintf(score, sizeof score, "%d  -  %d", home, away);
        big_text(g_fontHuge, 3.0f, ui::kText, score, true);
        const int left = (int)((kShowUs - elapsed + 999999ULL) / 1000000ULL);
        char line[128];
        std::snprintf(line, sizeof line, "Back to team select in %d s. Press READY there for a rematch.", left);
        centered_text(ui::kDim, line);
    }
    ImGui::End();
}


// ---------------------------------------------------------------------------
// Automatic online test (TEST-ONLINE.bat).  STRIKERS_AUTOTEST=host or guest:
// the host opens a room on the local relay and writes the code to
// autotest-room.txt, the guest joins it, both press READY by themselves, fake
// players play (see NetplayManager::CaptureLocalInput), and after
// STRIKERS_AUTOTEST_MINUTES both games write autotest-report-pN.txt and quit.
// ---------------------------------------------------------------------------
struct AutoTest
{
    int  role = -2;           // -2 not read yet, -1 off, 0 host, 1 guest
    u64  startUs = 0;
    u64  limitUs = 0;
    bool roomRequested = false;
    u64  lastJoinTryUs = 0;
    int  matches = 0;
    u8   lastMatchId = 0;
    bool desyncNoted = false;
    char log[4096] = "";
    bool done = false;
    char server[128] = "127.0.0.1";
    int  port = 7200;
};
AutoTest s_auto;

int autotest_role()
{
    AutoTest& a = s_auto;
    if (a.role == -2)
    {
        const char* e = std::getenv("STRIKERS_AUTOTEST");
        a.role = (e == nullptr || *e == '\0') ? -1 : (std::strcmp(e, "guest") == 0 ? 1 : 0);
        a.startUs = NetplayGetTimeUs();
        const char* m = std::getenv("STRIKERS_AUTOTEST_MINUTES");
        const int minutes = (m && atoi(m) > 0) ? atoi(m) : 10;
        a.limitUs = static_cast<u64>(minutes) * 60ULL * 1000000ULL;
        const char* srv = std::getenv("STRIKERS_AUTOTEST_SERVER");
        if (srv && *srv)
        {
            const char* colon = std::strchr(srv, ':');
            size_t n = colon ? static_cast<size_t>(colon - srv) : std::strlen(srv);
            if (n >= sizeof(a.server))
                n = sizeof(a.server) - 1;
            std::memcpy(a.server, srv, n);
            a.server[n] = '\0';
            if (colon)
                a.port = atoi(colon + 1);
        }
        if (a.role >= 0)
            std::fprintf(stderr, "[autotest] running as %s for %d minutes (relay %s:%d)\n",
                         a.role == 0 ? "host" : "guest", minutes, a.server, a.port);
    }
    return a.role;
}

void autotest_note(const char* fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[autotest] %s\n", line);
    const double secs = (NetplayGetTimeUs() - s_auto.startUs) / 1000000.0;
    char stamped[300];
    std::snprintf(stamped, sizeof stamped, "%6.0f s  %s\n", secs, line);
    const size_t have = std::strlen(s_auto.log);
    if (have + std::strlen(stamped) < sizeof(s_auto.log))
        std::strcat(s_auto.log, stamped);
}

void autotest_finish(const char* why)
{
    AutoTest& a = s_auto;
    if (a.done)
        return;
    a.done = true;
    autotest_note("finished: %s", why);
    char path[64];
    std::snprintf(path, sizeof path, "autotest-report-p%d.txt", a.role + 1);
    FILE* f = std::fopen(path, "w");
    if (f)
    {
        std::fprintf(f, "Automatic online test, player %d (%s)\n", a.role + 1, a.role == 0 ? "host" : "guest");
        std::fprintf(f, "Build %s %s, netcode v%d\n", __DATE__, __TIME__, NETPLAY_PROTOCOL_VERSION);
        std::fprintf(f, "Matches started: %d\n\n%s", a.matches, a.log);
        std::fclose(f);
    }
    PortRequestQuit();
}

void autotest_tick()
{
    if (autotest_role() < 0)
        return;
    AutoTest& a = s_auto;
    if (a.done)
        return;
    NetplayLobby& lobby = NetplayLobby::Instance();
    NetplayManager& mgr = NetplayManager::Instance();
    const u64 now = NetplayGetTimeUs();

    if (now - a.startUs > a.limitUs)
    {
        autotest_finish(a.matches > 0 ? "time is up" : "time is up, no match ever started");
        return;
    }

    // Get into a room.
    if (!mgr.IsActive() && lobby.GetState() == LOBBY_STATE_IDLE && online_can_start(nullptr))
    {
        if (a.role == 0 && !a.roomRequested)
        {
            std::remove("autotest-room.txt");
            a.roomRequested = lobby.HostRoom(a.server, static_cast<u16>(a.port));
            autotest_note("opening a room on %s:%d", a.server, a.port);
        }
        else if (a.role == 1 && now - a.lastJoinTryUs > 2000000ULL)
        {
            a.lastJoinTryUs = now;
            FILE* f = std::fopen("autotest-room.txt", "r");
            if (f)
            {
                char code[16] = "";
                if (std::fgets(code, sizeof code, f))
                {
                    code[std::strcspn(code, "\r\n ")] = '\0';
                    if (code[0])
                    {
                        autotest_note("joining room %s", code);
                        lobby.JoinRoom(a.server, static_cast<u16>(a.port), code);
                    }
                }
                std::fclose(f);
            }
        }
    }
    if (a.role == 0 && lobby.GetState() == LOBBY_STATE_HOSTING && lobby.GetRoomCode()[0])
    {
        static bool s_written = false;
        if (!s_written)
        {
            FILE* f = std::fopen("autotest-room.txt", "w");
            if (f)
            {
                std::fprintf(f, "%s\n", lobby.GetRoomCode());
                std::fclose(f);
                s_written = true;
                autotest_note("room %s is open", lobby.GetRoomCode());
            }
        }
    }
    if (lobby.GetState() == LOBBY_STATE_ERROR)
    {
        autotest_note("lobby error: %s", lobby.GetErrorMessage());
        lobby.ClearError();
        a.roomRequested = false;
    }

    // Follow the matches.
    if (mgr.IsActive() && mgr.GetMatchId() != a.lastMatchId)
    {
        a.lastMatchId = mgr.GetMatchId();
        a.matches++;
        a.desyncNoted = false;
        autotest_note("match %u starting", a.lastMatchId);
    }
    if (mgr.IsActive() && mgr.IsDesynced() && !a.desyncNoted)
    {
        a.desyncNoted = true;
        autotest_note("DESYNC in match %u at game frame %d", a.lastMatchId, mgr.GetDesyncFrame());
    }
    static int s_lastResyncs = 0;
    if (mgr.IsActive() && mgr.GetResyncCount() != s_lastResyncs)
    {
        s_lastResyncs = mgr.GetResyncCount();
        if (s_lastResyncs > 0)
            autotest_note("play resumed together (resync %d) at frame %u", s_lastResyncs, mgr.GetSimFrame());
    }
    static NetplaySessionState s_prev = NETPLAY_STATE_IDLE;
    if (mgr.GetState() != s_prev)
    {
        if (s_prev == NETPLAY_STATE_RUNNING && mgr.GetState() != NETPLAY_STATE_RUNNING)
            autotest_note("connection to the other game ended");
        s_prev = mgr.GetState();
    }
}

extern "C" int PortAutotestActive(void)
{
    return autotest_role() >= 0 ? 1 : 0;
}

// The match rules on the online VS screen.  The host's own game options are
// what both games play by (sent with the start), so the host edits those here
// and the guest sees them in the host's VS picks.
const int kRuleTimes[] = { 120, 180, 240, 300, 600, 900 };
const char* const kRuleTimeNames[] = { "2 minutes", "3 minutes", "4 minutes", "5 minutes",
                                       "10 minutes", "15 minutes" };
const char* const kRuleSkillNames[] = { "Rookie", "Professional", "Superstar", "Legend" };
const char* const kRuleCustomNames[] = { "Normal", "Explosive only", "Freezing only",
                                         "Shells only", "Giant only", "Boosts only" };

void rules_to_choice(const PortMatchRules& r, NetplayVsChoice* c)
{
    const int t = r.gameTimeSec / 10;
    c->ruleTimeSec10 = (u8)(t < 1 ? 1 : (t > 255 ? 255 : t));
    c->ruleSkill = (u8)r.skill;
    c->ruleFlags = (u8)((r.powerUps ? NETPLAY_RULE_ITEMS : 0) | (r.shoot2Score ? NETPLAY_RULE_S2S : 0) |
                        (r.bowser ? NETPLAY_RULE_BOWSER : 0) |
                        (r.infiniteItems ? NETPLAY_RULE_INFINITE : 0) |
                        (r.stunnedGoalies ? NETPLAY_RULE_STUNNED : 0));
    c->ruleCustom = (u8)r.customItems;
}

const char* yes_no(bool on) { return on ? "On" : "Off"; }

void draw_rules_readonly(const NetplayVsChoice& r)
{
    const int sec = r.ruleTimeSec10 * 10;
    const int skill = r.ruleSkill >= 1 && r.ruleSkill <= 4 ? r.ruleSkill : 2;
    const int custom = r.ruleCustom <= 5 ? r.ruleCustom : 0;
    ImGui::Text("Match length: %d:%02d     Teammates: %s", sec / 60, sec % 60, kRuleSkillNames[skill - 1]);
    ImGui::Text("Items: %s   Super Strikes: %s   Bowser: %s", yes_no(r.ruleFlags & NETPLAY_RULE_ITEMS),
                yes_no(r.ruleFlags & NETPLAY_RULE_S2S), yes_no(r.ruleFlags & NETPLAY_RULE_BOWSER));
    ImGui::Text("Infinite items: %s   Glass-jaw goalies: %s   Item set: %s",
                yes_no(r.ruleFlags & NETPLAY_RULE_INFINITE), yes_no(r.ruleFlags & NETPLAY_RULE_STUNNED),
                kRuleCustomNames[custom]);
}

// Host: edits its own game options.  Returns nothing; the VS picks carry them.
void draw_rules_editor(bool locked)
{
    PortMatchRules r;
    if (!PortGetMatchRules(&r))
    {
        ImGui::TextDisabled("(the rules appear once the game has started)");
        return;
    }
    PortMatchRules before = r;
    ImGui::BeginDisabled(locked);

    int t = 0;
    for (int i = 0; i < 6; i++)
        if (kRuleTimes[i] == r.gameTimeSec)
            t = i;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.5f);
    if (ImGui::Combo("Match length", &t, kRuleTimeNames, 6))
        r.gameTimeSec = kRuleTimes[t];
    ImGui::SameLine(0.0f, 24.0f);
    int sk = (r.skill >= 1 && r.skill <= 4 ? r.skill : 2) - 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.5f);
    if (ImGui::Combo("Teammates (CPU)", &sk, kRuleSkillNames, 4))
        r.skill = sk + 1;

    bool items = r.powerUps != 0, s2s = r.shoot2Score != 0, bowser = r.bowser != 0;
    if (ImGui::Checkbox("Items", &items)) r.powerUps = items;
    ImGui::SameLine(0.0f, 20.0f);
    if (ImGui::Checkbox("Super Strikes", &s2s)) r.shoot2Score = s2s;
    ImGui::SameLine(0.0f, 20.0f);
    if (ImGui::Checkbox("Bowser attacks", &bowser)) r.bowser = bowser;

    bool inf = r.infiniteItems != 0, stun = r.stunnedGoalies != 0;
    if (ImGui::Checkbox("Infinite items", &inf)) r.infiniteItems = inf;
    ImGui::SameLine(0.0f, 20.0f);
    if (ImGui::Checkbox("Glass-jaw goalies", &stun)) r.stunnedGoalies = stun;
    ImGui::SameLine(0.0f, 20.0f);
    int cu = r.customItems >= 0 && r.customItems <= 5 ? r.customItems : 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.5f);
    if (ImGui::Combo("Item set", &cu, kRuleCustomNames, 6))
        r.customItems = cu;

    ImGui::EndDisabled();
    if (std::memcmp(&r, &before, sizeof r) != 0)
        PortSetMatchRules(&r);
}

void draw_online_vs()
{
    NetplayManager& mgr = NetplayManager::Instance();
    const bool isHost = mgr.GetLocalSlot() == 0;
    const ImVec4 kGreen = ui::kLime, kYellow = ui::kYellow, kGrey = ui::kDim, kRed = ui::kRed;
    const float fs = ImGui::GetFontSize();

    NetplayVsChoice c = mgr.GetVsChoice();
    if (c.captain >= kCaptainCount)
    {
        // Sensible defaults: host is Mario + Toad, guest is Luigi + Koopa.
        c.captain  = isHost ? 3 : 2;
        c.sidekick = isHost ? 0 : 1;
        c.stadium  = 0;
        c.ready    = 0;
    }
    const char* where = "";
    c.atFrontEnd = online_can_start(&where) ? 1 : 0;

    NetplayVsChoice r;
    std::memset(&r, 0, sizeof r);
    const bool haveRemote = mgr.GetRemoteVsChoice(&r);
    const u8 stadium = isHost ? c.stadium : (haveRemote ? r.stadium : 0);

    if (online_match_loading())
    {
        ImGui::TextColored(kGreen, "MATCH STARTING...");
        ImGui::Text("%s + %s  vs  %s + %s",
                    vs_name(kCaptainNames, kCaptainCount, isHost ? c.captain : r.captain),
                    vs_name(kSidekickNames, kSidekickCount, isHost ? c.sidekick : r.sidekick),
                    vs_name(kCaptainNames, kCaptainCount, isHost ? r.captain : c.captain),
                    vs_name(kSidekickNames, kSidekickCount, isHost ? r.sidekick : c.sidekick));
        ImGui::TextColored(kGrey, "%s", vs_name(kStadiumNames, kStadiumCount, stadium));
        mgr.SetVsChoice(c);
        return;
    }

    if (s_onlineStart.failText[0] != '\0' && NetplayGetTimeUs() < s_onlineStart.failUntilUs)
    {
        ImGui::TextColored(kRed, "%s", s_onlineStart.failText);
        ImGui::TextColored(kRed, "Both go to the main menu and press READY again.");
    }
    ImGui::TextColored(kGrey, "You are Player %d (%s)", mgr.GetLocalSlot() + 1,
                       isHost ? "host, home team" : "away team");
    ImGui::Spacing();

    if (ImGui::BeginTable("##vs", 2, ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("you", ImGuiTableColumnFlags_WidthFixed, fs * 23.0f);
        ImGui::TableSetupColumn("them", ImGuiTableColumnFlags_WidthFixed, fs * 13.0f);
        ImGui::TableNextRow();

        // --- Your picks ---
        ImGui::TableNextColumn();
        heading("YOUR CAPTAIN");
        ImGui::BeginDisabled(c.ready != 0);
        for (int i = 0; i < kCaptainCount; i++)
        {
            if (i % 4 != 0)
                ImGui::SameLine();
            const bool taken = haveRemote && r.captain == i;
            char label[48];
            std::snprintf(label, sizeof label, "%s%s##cap%d", kCaptainNames[i], taken ? " *" : "", i);
            if (ImGui::Selectable(label, c.captain == i, 0, ImVec2(fs * 5.4f, fs * 1.7f)))
                c.captain = static_cast<u8>(i);
        }
        heading("YOUR SIDEKICKS");
        for (int i = 0; i < kSidekickCount; i++)
        {
            if (i != 0)
                ImGui::SameLine();
            char label[32];
            std::snprintf(label, sizeof label, "%s##sk%d", kSidekickNames[i], i);
            if (ImGui::Selectable(label, c.sidekick == i, 0, ImVec2(fs * 5.4f, fs * 1.5f)))
                c.sidekick = static_cast<u8>(i);
        }
        heading(isHost ? "STADIUM (you choose)" : "STADIUM (host chooses)");
        if (isHost)
        {
            int st = c.stadium;
            ImGui::SetNextItemWidth(fs * 14.0f);
            if (ImGui::Combo("##stadium", &st, kStadiumNames, kStadiumCount))
                c.stadium = static_cast<u8>(st);
        }
        else
        {
            ImGui::Text("%s", haveRemote ? vs_name(kStadiumNames, kStadiumCount, r.stadium) : "...");
        }
        ImGui::EndDisabled();

        // --- Opponent ---
        ImGui::TableNextColumn();
        heading("OPPONENT");
        if (!haveRemote)
        {
            ImGui::TextColored(kGrey, "waiting for their picks...");
        }
        else
        {
            ImGui::Text("Captain:  %s", vs_name(kCaptainNames, kCaptainCount, r.captain));
            ImGui::Text("Sidekicks: %s", vs_name(kSidekickNames, kSidekickCount, r.sidekick));
            ImGui::Spacing();
            if (r.ready)
                ImGui::TextColored(kGreen, "READY!");
            else
                ImGui::TextColored(kYellow, "choosing...");
            if (!r.atFrontEnd)
                ImGui::TextColored(kYellow, "(not on the main menu)");
        }
        ImGui::EndTable();
    }

    // --- Match rules: the host's ---
    heading(isHost ? "MATCH RULES (you choose)" : "MATCH RULES (host chooses)");
    if (isHost)
    {
        draw_rules_editor(c.ready != 0);
        PortMatchRules rules;
        if (PortGetMatchRules(&rules))
            rules_to_choice(rules, &c);
    }
    else if (haveRemote && r.ruleTimeSec10 != 0)
    {
        draw_rules_readonly(r);
    }
    else
    {
        ImGui::TextColored(kGrey, "waiting for the host's rules...");
    }

    if (s_rematchHint)
        ImGui::TextColored(ui::kLime, "Rematch? Your teams are still picked: press READY!");

    ImGui::Spacing();
    const bool sameCaptain = haveRemote && r.captain == c.captain;
    const bool canReady = c.atFrontEnd && !sameCaptain;
    if (c.ready && !canReady)
        c.ready = 0;   // e.g. the opponent just took our captain

    ImGui::BeginDisabled(!c.ready && !canReady);
    if (c.ready ? stop_button("CANCEL READY", ImVec2(fs * 12.0f, fs * 2.4f))
                : go_button("READY!", ImVec2(fs * 12.0f, fs * 2.4f)))
        c.ready = c.ready ? 0 : 1;
    ImGui::EndDisabled();
    if (autotest_role() >= 0 && canReady)
        c.ready = 1;   // automatic test: ready as soon as possible
    ImGui::SameLine();
    if (ImGui::Button("Leave room", ImVec2(fs * 7.0f, fs * 2.4f)))
    {
        NetplayLobby::Instance().Leave();
        return;
    }

    // What is everyone waiting for?
    static u64 s_bothReadySinceUs = 0;
    const bool bothReady = c.ready && haveRemote && r.ready && c.atFrontEnd && r.atFrontEnd && !sameCaptain;
    if (!c.atFrontEnd)
        ImGui::TextColored(kRed, "Your game is on: %s. Go to the main menu (Grudge Match, Cup Battles...)",
                           where);
    else if (sameCaptain)
        ImGui::TextColored(kRed, "You both picked %s. One of you choose another captain.",
                           vs_name(kCaptainNames, kCaptainCount, c.captain));
    else if (haveRemote && !r.atFrontEnd)
        ImGui::TextColored(kYellow, "Waiting for your friend to reach the main menu...");
    else if (bothReady)
        ImGui::TextColored(kGreen, isHost ? "Both ready! Starting..." : "Both ready! Waiting for the host to start...");
    else if (c.ready)
        ImGui::TextColored(kYellow, "Waiting for your friend to press READY...");
    else
        ImGui::TextColored(kGrey, "Pick your team, then press READY.");

    // The host starts the match one second after both are ready (time to cancel).
    if (isHost && bothReady)
    {
        const u64 now = NetplayGetTimeUs();
        if (s_bothReadySinceUs == 0)
            s_bothReadySinceUs = now;
        else if (now - s_bothReadySinceUs > 1000000ULL)
        {
            s_bothReadySinceUs = 0;
            mgr.SendStartMatch(c.captain, r.captain, c.stadium, c.sidekick, r.sidekick);
            cmd(PDBG_START_MATCH, c.captain, r.captain, c.stadium, (float)c.sidekick,
                (float)r.sidekick, 99.0f, (float)mgr.GetMatchId());
            c = mgr.GetVsChoice();   // SendStartMatch cleared "ready"
        }
    }
    else
    {
        s_bothReadySinceUs = 0;
    }

    mgr.SetVsChoice(c);
}

void draw_netplay_tab()
{
    static char s_serverHost[128] = "";
    static int  s_serverPort = 7200;
    static char s_roomCodeInput[16] = "";
    static bool s_initialized = false;

    if (!s_initialized)
    {
        s_initialized = true;
        const char* envServer = std::getenv("STRIKERS_SERVER");
        if (!envServer) envServer = std::getenv("STRIKERS_RELAY");
        if (!envServer) envServer = std::getenv("STRIKERS_NETPLAY_SERVER");

        if (envServer && *envServer)
        {
            const char* colon = strchr(envServer, ':');
            if (colon)
            {
                size_t hostLen = colon - envServer;
                if (hostLen < sizeof(s_serverHost))
                {
                    memcpy(s_serverHost, envServer, hostLen);
                    s_serverHost[hostLen] = '\0';
                }
                s_serverPort = atoi(colon + 1);
            }
            else
            {
                strncpy(s_serverHost, envServer, sizeof(s_serverHost) - 1);
            }
        }
        else
        {
            strcpy(s_serverHost, "127.0.0.1");
        }
    }

    NetplayLobby& lobby = NetplayLobby::Instance();
    NetplayManager& mgr = NetplayManager::Instance();
    NetplayLobbyState lState = lobby.GetState();
    NetplaySessionState mState = mgr.GetState();
    const float fs = ImGui::GetFontSize();

    // ---- In a room with a friend ----
    if (mState == NETPLAY_STATE_RUNNING)
    {
        if (begin_card("##np_connected"))
        {
            const NetStatus st = net_status();
            pill(st.text, st.color);
            ImGui::SameLine(0.0f, fs);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ui::kDim, "Room %s  \xC2\xB7  you are %s", lobby.GetRoomCode()[0] ? lobby.GetRoomCode() : "direct",
                               mgr.GetLocalSlot() == 0 ? "the home team (host)" : "the away team");
            ImGui::TextColored(ui::kDim, "Input delay %d frames  \xC2\xB7  %s", mgr.GetInputDelay(),
                               mgr.GetMaxRollback() == 0 ? "lockstep (never guesses)" : "rollback (experimental)");
            if (mgr.IsDesynced())
            {
                ImGui::TextColored(ui::kOrange, "The two games went out of step at frame %d. They are put back in "
                                                "step automatically; if it keeps happening, send both logs.",
                                   mgr.GetDesyncFrame());
            }
        }
        end_card();

        if (PortMatchExists())
            ImGui::TextColored(ui::kDim, "A match is being played. Team selection comes back after it.");
        else
            draw_online_vs();

        ImGui::Spacing();
        if (stop_button("Leave the room", ImVec2(fs * 10.0f, fs * 2.0f)))
            lobby.Leave();

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Troubleshooting"))
        {
            ImGui::TextDisabled("Build: %s %s (netcode v%d). Both players need the same one.", __DATE__, __TIME__,
                                NETPLAY_PROTOCOL_VERSION);
            ImGui::TextDisabled("Rollback %d frames, frame advantage %+d.", mgr.GetRollbackFrames(),
                                mgr.GetFrameAdvantage());
            if (lobby.UsingLocalRelay())
                ImGui::TextDisabled("Relay: this PC (your friend reaches it through Playit).");
            if (ImGui::TreeNode("Ball animation test (breaks sync!)"))
            {
                ImGui::TextWrapped("Each box puts back the offline behaviour of one online change. Tick one "
                                   "at a time in ONE window during a match and watch the ball. The two games "
                                   "will desync while a box is ticked; that is expected.");
                const char* const kDiag[] = {
                    "Clocks advance per rendered frame",
                    "Stadium updates per rendered frame",
                    "Gameplay shares the random numbers with rendering",
                    "Flick history updates per rendered frame",
                    "Button hold times count real time",
                };
                for (int i = 0; i < 5; i++)
                {
                    bool on = PortNetplayDiag(i) != 0;
                    if (ImGui::Checkbox(kDiag[i], &on))
                    {
                        PortNetplaySetDiag(i, on ? 1 : 0);
                        std::fprintf(stderr, "[netplay] ball test: '%s' %s\n", kDiag[i], on ? "ON" : "off");
                    }
                }
                ImGui::TreePop();
            }
        }
        return;
    }

    // ---- Room open, waiting ----
    if (lState == LOBBY_STATE_HOSTING)
    {
        if (begin_card("##np_hosting"))
        {
            heading("YOUR ROOM IS OPEN");
            spinner(ui::kYellow);
            ImGui::SameLine();
            ImGui::TextColored(ui::kYellow, "Waiting for your friend to join...");
            if (lobby.GetNotice()[0] != '\0')
                ImGui::TextColored(ui::kOrange, "%s", lobby.GetNotice());

            ImGui::Spacing();
            ImGui::TextColored(ui::kDim, "ROOM CODE");
            char spaced[32];
            const char* code = lobby.GetRoomCode();
            size_t n = 0;
            for (size_t i = 0; code[i] != '\0' && n + 2 < sizeof spaced; i++)
            {
                if (i > 0)
                    spaced[n++] = ' ';
                spaced[n++] = code[i];
            }
            spaced[n] = '\0';
            big_text(g_fontHuge, 3.6f, ui::kLime, spaced, false);
            ImGui::SameLine(0.0f, fs * 1.5f);
            if (ImGui::Button("Copy code"))
                ImGui::SetClipboardText(code);

            ImGui::Spacing();
            const bool isLocal = (strcmp(s_serverHost, "127.0.0.1") == 0 || strcmp(s_serverHost, "localhost") == 0);
            ImGui::TextColored(ui::kText, "Tell your friend:");
            ImGui::BulletText("Open the game, press F1, go to Play Online.");
            if (strstr(s_serverHost, ".ply.gg") || !isLocal)
                ImGui::BulletText("Server: %s, port %d (it is in their strikers.ini already).", s_serverHost,
                                  s_serverPort);
            else
            {
                ImGui::BulletText("Same Wi-Fi: server %s, port %d.", NetplayGetLocalIP(), s_serverPort);
                ImGui::BulletText("Over the internet: give them your Playit address and port.");
            }
            ImGui::BulletText("Type the code %s and press Join.", code);
        }
        end_card();
        if (stop_button("Close the room", ImVec2(fs * 10.0f, fs * 2.0f)))
            lobby.Leave();
        return;
    }

    // ---- On the way in ----
    if (lState == LOBBY_STATE_JOINING || mState == NETPLAY_STATE_CONNECTING)
    {
        if (begin_card("##np_joining"))
        {
            spinner(ui::kBlue);
            ImGui::SameLine();
            {
                FontScope f(g_fontBold);
                if (lState == LOBBY_STATE_JOINING)
                    ImGui::TextColored(ui::kBlue, "Joining room %s...", lobby.GetRoomCode());
                else
                    ImGui::TextColored(ui::kBlue, "Your friend is here! Connecting the two games...");
            }
            ImGui::TextColored(ui::kDim, "Server %s:%d. This takes a few seconds (8 at most).", s_serverHost,
                               s_serverPort);
            if (mgr.GetPeerVersionMismatch() != 0)
            {
                ImGui::TextColored(ui::kRed, "Your friend's game is a different version (netcode %u, yours %d).",
                                   mgr.GetPeerVersionMismatch(), NETPLAY_PROTOCOL_VERSION);
                ImGui::TextColored(ui::kRed, "You both need the same release: whoever is older, update (F1 > Updates).");
            }
        }
        end_card();
        if (ImGui::Button("Cancel", ImVec2(fs * 7.0f, fs * 2.0f)))
            lobby.Leave();
        return;
    }

    // ---- Something went wrong ----
    if (lState == LOBBY_STATE_ERROR)
    {
        if (begin_card("##np_error"))
        {
            {
                FontScope f(g_fontBold);
                ImGui::TextColored(ui::kRed, "Could not connect");
            }
            ImGui::TextWrapped("%s", lobby.GetErrorMessage()[0] ? lobby.GetErrorMessage() : "Connection failed.");
            if (ImGui::Button("OK"))
                lobby.ClearError();
        }
        end_card();
    }

    // ---- Not online: host or join ----
    if (ImGui::BeginTable("##np_choose", 2, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (begin_card("##np_host"))
        {
            heading("HOST A GAME");
            ImGui::TextWrapped("Open a room and share its code with your friend.");
            ImGui::Spacing();
            if (go_button("Create a room", ImVec2(-FLT_MIN, fs * 2.4f)))
                lobby.HostRoom(s_serverHost, static_cast<u16>(s_serverPort));
        }
        end_card();

        ImGui::TableNextColumn();
        if (begin_card("##np_join"))
        {
            heading("JOIN A GAME");
            ImGui::TextWrapped("Type the 4-digit code your friend gives you.");
            {
                FontScope f(g_fontHead);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint("##join_code", "0000", s_roomCodeInput, sizeof(s_roomCodeInput),
                                         ImGuiInputTextFlags_CharsDecimal);
            }
            ImGui::BeginDisabled(s_roomCodeInput[0] == '\0');
            if (go_button("Join", ImVec2(-FLT_MIN, fs * 2.4f)))
                lobby.JoinRoom(s_serverHost, static_cast<u16>(s_serverPort), s_roomCodeInput);
            ImGui::EndDisabled();
        }
        end_card();
        ImGui::EndTable();
    }

    const bool isLocalHost = (strcmp(s_serverHost, "127.0.0.1") == 0 || strcmp(s_serverHost, "localhost") == 0);
    ImGui::TextColored(ui::kDim, "Server: %s:%d%s", s_serverHost, s_serverPort,
                       isLocalHost ? "  (this PC only - set your Playit address to play over the internet)" : "");

    if (ImGui::CollapsingHeader("Server settings"))
    {
        ImGui::SetNextItemWidth(fs * 12.0f);
        ImGui::InputText("Server address", s_serverHost, sizeof(s_serverHost));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 5.0f);
        ImGui::InputInt("Port", &s_serverPort, 0, 0);
        if (isLocalHost)
        {
            if (s_serverPort != 7200)
            {
                ImGui::TextColored(ui::kOrange, "The relay on this PC listens on port 7200.");
                ImGui::SameLine();
                if (ImGui::SmallButton("Use 7200"))
                    s_serverPort = 7200;
            }
            ImGui::TextWrapped("127.0.0.1 is for testing on this PC. To play with a friend over the internet, "
                               "enter your Playit address and port. (When you host, the game still uses the "
                               "relay on this PC if one is running.)");
        }
        ImGui::TextDisabled("The address saved in strikers.ini ([netplay] server) is used at start.");
    }

    if (ImGui::CollapsingHeader("Direct connection (advanced)"))
    {
        static char s_directPeer[128] = "127.0.0.1:7100";
        static int  s_directSlot = 0;
        ImGui::SetNextItemWidth(fs * 12.0f);
        ImGui::InputText("Other PC (IP:port)", s_directPeer, sizeof(s_directPeer));
        const char* const kSlots[] = { "Player 1 (host)", "Player 2 (guest)" };
        ImGui::SetNextItemWidth(fs * 9.0f);
        ImGui::Combo("I am", &s_directSlot, kSlots, 2);
        if (ImGui::Button("Connect"))
        {
            const char* delayStr = std::getenv("STRIKERS_NETPLAY_DELAY");
            if (!delayStr || !*delayStr) delayStr = std::getenv("STRIKERS_DELAY");
            mgr.Connect(7100, s_directPeer, s_directSlot, (delayStr && *delayStr) ? atoi(delayStr) : 2);
        }
    }

    if (ImGui::CollapsingHeader("Sync test (developers)"))
    {
        ImGui::TextDisabled("Build: %s %s (netcode v%d)", __DATE__, __TIME__, NETPLAY_PROTOCOL_VERSION);
        ImGui::TextWrapped("Saves the game, plays a few frames, loads it back and plays them again: both "
                           "runs must match exactly.");
        static char s_testResult[256] = "";
        static bool s_testSuccess = false;
        static bool s_tested = false;
        if (ImGui::Button("5-frame test"))
        {
            s_tested = true;
            s_testSuccess = NetplayTestRollbackDeterminism(5, s_testResult, sizeof(s_testResult));
        }
        ImGui::SameLine();
        if (ImGui::Button("10-frame test"))
        {
            s_tested = true;
            s_testSuccess = NetplayTestRollbackDeterminism(10, s_testResult, sizeof(s_testResult));
        }
        if (s_tested)
            ImGui::TextColored(s_testSuccess ? ui::kLime : ui::kRed, "%s %s", s_testSuccess ? "PASS" : "FAIL",
                               s_testResult);
    }
}

// ---------------------------------------------------------------------------
// Display: the few screen settings worth changing while playing.
// ---------------------------------------------------------------------------
void draw_display_page()
{
    const float fs = ImGui::GetFontSize();
    heading("SCREEN");
    bool full = is_fullscreen();
    if (ImGui::Checkbox("Fullscreen  (F11)", &full))
        set_fullscreen(full);
    bool vs = vsync_state();
    if (ImGui::Checkbox("V-Sync  (steadier picture, slightly more input delay)", &vs))
        set_vsync(vs);

    heading("MENU");
    {
        const float kScales[] = { 0.9f, 1.0f, 1.15f, 1.3f };
        const char* const kScaleNames[] = { "Small", "Normal", "Large", "Extra large" };
        int cur = 1;
        for (int i = 0; i < 4; i++)
            if (std::fabs(s_prefs.uiScale - kScales[i]) < 0.01f)
                cur = i;
        ImGui::SetNextItemWidth(fs * 10.0f);
        if (ImGui::Combo("Text size", &cur, kScaleNames, 4))
        {
            s_prefs.uiScale = kScales[cur];
            prefs_save();
        }
    }
    bool perf = s_enabled != 0;
    if (ImGui::Checkbox("Show frame rate in the corner  (F2)", &perf))
        s_enabled = perf ? 1 : 0;

    heading("MORE");
    ImGui::TextColored(ui::kDim, "Resolution, graphics quality, the game files and the keyboard keys are in");
    ImGui::TextColored(ui::kDim, "Mario Strikers Settings, next to the game.");
}

// ---------------------------------------------------------------------------
// Advanced: the developer tools, as they always were, behind a warning.
// ---------------------------------------------------------------------------
void draw_advanced_page(const PortBenchLive& live)
{
    const bool online = NetplayManager::Instance().IsActive();
    static bool s_allowOnline = false;
    if (online)
    {
        ImGui::TextColored(ui::kOrange, "You are online. Most of these tools change the match on this PC only,");
        ImGui::TextColored(ui::kOrange, "which puts the two games out of step.");
        ImGui::Checkbox("Let me use them anyway", &s_allowOnline);
    }
    else
    {
        ImGui::TextColored(ui::kDim, "Developer tools: match state, AI, camera, rendering, audio and input details.");
    }

    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyScroll))
    {
        struct Tab
        {
            const char* name;
            void (*draw)();
        };
        const bool locked = online && !s_allowOnline;
        if (ImGui::BeginTabItem("Frame"))
        {
            ImGui::BeginChild("body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            draw_frame_tab(live);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        const Tab tabs[] = {
            { "Match", draw_match_tab },   { "Players", draw_players_tab },
            { "Items", draw_items_tab },   { "AI", draw_ai_tab },
            { "Cheats", draw_cheats_tab }, { "Camera", draw_camera_tab },
            { "Render", draw_render_tab }, { "Audio", draw_audio_tab },
            { "Input", draw_input_tab },   { "Session", draw_session_tab },
            { "System", draw_system_tab },
        };
        // STRIKERS_OVERLAY_TAB=<name> selects a tab on the first frame, so a headless run can
        // exercise a tab's draw code without a hand on the mouse.
        static const char* s_wantTab = std::getenv("STRIKERS_OVERLAY_TAB");
        for (size_t i = 0; i < sizeof tabs / sizeof tabs[0]; i++)
        {
            ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
            if (s_wantTab != nullptr && std::strcmp(s_wantTab, tabs[i].name) == 0)
            {
                flags |= ImGuiTabItemFlags_SetSelected;
                s_wantTab = nullptr;
            }
            if (ImGui::BeginTabItem(tabs[i].name, nullptr, flags))
            {
                ImGui::BeginChild("body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
                ImGui::BeginDisabled(locked);
                tabs[i].draw();
                ImGui::EndDisabled();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
}

// ---------------------------------------------------------------------------
// Updates: this version, and the newest one on GitHub.
// ---------------------------------------------------------------------------
void draw_updates_page()
{
    const float fs = ImGui::GetFontSize();
    heading("THIS VERSION");
    {
        FontScope f(g_fontBold);
        ImGui::TextColored(ui::kText, "%s", PortUpdaterVersion());
    }
    ImGui::TextColored(ui::kDim, "Both players need the same version to play online.");

    heading("NEWEST VERSION");
    const PortUpdateState st = PortUpdaterPoll();
    switch (st)
    {
    case PORT_UPDATE_UNAVAILABLE:
        ImGui::TextColored(ui::kDim, "Updates work in the release downloaded from GitHub (it has UPDATE.bat).");
        ImGui::TextColored(ui::kDim, "This copy was built from source.");
        break;
    case PORT_UPDATE_IDLE:
        if (ImGui::Button("Check for updates"))
            PortUpdaterCheck();
        break;
    case PORT_UPDATE_CHECKING:
        spinner(ui::kBlue);
        ImGui::SameLine();
        ImGui::TextColored(ui::kBlue, "Asking GitHub...");
        break;
    case PORT_UPDATE_UP_TO_DATE:
        ImGui::TextColored(ui::kLime, "You have the newest version.");
        if (ImGui::Button("Check again"))
            PortUpdaterCheck();
        break;
    case PORT_UPDATE_ERROR:
        ImGui::TextColored(ui::kOrange, "Could not check: %s", PortUpdaterError());
        if (ImGui::Button("Try again"))
            PortUpdaterCheck();
        break;
    case PORT_UPDATE_AVAILABLE:
    {
        if (begin_card("##update_card"))
        {
            char title[96];
            std::snprintf(title, sizeof title, "%s IS OUT", PortUpdaterLatest());
            heading(title);
            const char* notes = PortUpdaterNotes();
            if (notes[0] != '\0')
            {
                ImGui::BeginChild("##update_notes", ImVec2(0.0f, fs * 9.0f), ImGuiChildFlags_None);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(notes);
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
            }
            const bool online = NetplayManager::Instance().IsActive() ||
                                NetplayLobby::Instance().GetState() != LOBBY_STATE_IDLE;
            static bool s_launchFailed = false;
            ImGui::BeginDisabled(online);
            if (go_button("Update now", ImVec2(fs * 10.0f, fs * 2.4f)))
            {
                if (PortUpdaterLaunch())
                    PortRequestQuit();
                else
                    s_launchFailed = true;
            }
            ImGui::EndDisabled();
            if (PortUpdaterPage()[0] != '\0')
            {
                ImGui::SameLine();
                if (ImGui::Button("See it on GitHub", ImVec2(0.0f, fs * 2.4f)))
                    SDL_OpenURL(PortUpdaterPage());
            }
            ImGui::SameLine();
            if (ImGui::Button("Later", ImVec2(fs * 6.0f, fs * 2.4f)))
                set_menu_open(false);
            if (online)
                ImGui::TextColored(ui::kOrange, "Leave the online room first.");
            else if (s_launchFailed)
                ImGui::TextColored(ui::kRed, "Could not start the updater. Close the game and run UPDATE.bat.");
            else
                ImGui::TextColored(ui::kDim, "The game closes, updates and starts again. Your settings and saves are kept.");
        }
        end_card();
        break;
    }
    }
}

// Asks GitHub once, a few seconds after the game starts, and says so if there is something new.
// Returns true once, the first moment a newer release is known and the menu may open for it
// (not during a match or an online session); the caller then opens the Updates page.
bool update_tick()
{
    static bool s_announced = false;
    static double s_start = -1.0;
    static bool s_asked = false;
    static double s_toastUntil = -1.0;
    const double now = ImGui::GetTime();
    if (s_start < 0.0)
        s_start = now;
    if (!s_asked && now - s_start > 4.0)
    {
        s_asked = true;
        PortUpdaterCheck();
    }
    const PortUpdateState st = PortUpdaterPoll();
    if (st == PORT_UPDATE_AVAILABLE && !s_announced && !s_menuOpen && !PortMatchExists() &&
        !NetplayManager::Instance().IsActive() && NetplayLobby::Instance().GetState() == LOBBY_STATE_IDLE)
    {
        s_announced = true;
        fprintf(stderr, "[update] %s is out (this is %s); showing the Updates page\n", PortUpdaterLatest(),
                PortUpdaterVersion());
        return true;
    }
    if (st == PORT_UPDATE_AVAILABLE && s_toastUntil < 0.0)
        s_toastUntil = now + 15.0;
    if (st == PORT_UPDATE_AVAILABLE && now < s_toastUntil && !s_menuOpen && !PortMatchExists())
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float fs = ImGui::GetFontSize();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - fs * 0.8f, io.DisplaySize.y - fs * 3.0f),
                                ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        if (ImGui::Begin("##update_toast", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
        {
            char text[128];
            std::snprintf(text, sizeof text, "Update %s is out: F1 > Updates", PortUpdaterLatest());
            pill(text, ui::kYellow);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    return false;
}

// ---------------------------------------------------------------------------
// The F1 menu: a centred panel with the pages down the left.
// ---------------------------------------------------------------------------
enum MenuPage
{
    PAGE_ONLINE,
    PAGE_CONTROLS,
    PAGE_SOUND,
    PAGE_DISPLAY,
    PAGE_UPDATES,
    PAGE_ADVANCED,
    PAGE_COUNT
};
struct PageInfo
{
    const char* name;
    const char* sub;
    const char* title;
    const char* blurb;
};
const PageInfo kPages[PAGE_COUNT] = {
    { "Play Online", "Host or join a friend", "Play Online", "Play a match with a friend over the internet." },
    { "Controls", "Controller and buttons", "Controls", "Choose what you play with and which button does what." },
    { "Sound", "Music, effects, voices", "Sound", "Volume levels, kept on this PC." },
    { "Display", "Screen and menu", "Display", "Fullscreen, V-Sync and the size of this menu." },
    { "Updates", "Get the newest version", "Updates", "Install the newest release from GitHub in one click." },
    { "Advanced", "Developer tools", "Advanced", "Everything else: the tools used to build and debug this port." },
};
int s_page = PAGE_ONLINE;

void draw_main_menu(const PortBenchLive& live)
{
    // STRIKERS_OVERLAY_TAB names a page as well as a developer tab.
    static bool s_tabRead = false;
    if (!s_tabRead)
    {
        s_tabRead = true;
        if (const char* t = std::getenv("STRIKERS_OVERLAY_TAB"))
        {
            if (std::strcmp(t, "Netplay") == 0) s_page = PAGE_ONLINE;
            else if (std::strcmp(t, "Controls") == 0) s_page = PAGE_CONTROLS;
            else if (std::strcmp(t, "Sound") == 0) s_page = PAGE_SOUND;
            else if (std::strcmp(t, "Display") == 0) s_page = PAGE_DISPLAY;
            else if (std::strcmp(t, "Updates") == 0) s_page = PAGE_UPDATES;
            else s_page = PAGE_ADVANCED;
        }
    }

    const ImGuiIO& io = ImGui::GetIO();
    const float fs = ImGui::GetFontSize();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(4, 6, 12, 150));

    float w = fs * 58.0f, h = fs * 38.0f;
    const float margin = fs * 1.5f;
    if (w > io.DisplaySize.x - margin * 2.0f) w = io.DisplaySize.x - margin * 2.0f;
    if (h > io.DisplaySize.y - margin * 2.0f) h = io.DisplaySize.y - margin * 2.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.067f, 0.11f, 0.97f));
    const bool open = ImGui::Begin("##strikers_menu", nullptr,
                                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (open)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float headerH = fs * 4.4f;

        // Header band with a lime rule under it.
        dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + headerH), ImGui::GetColorU32(ImVec4(0.075f, 0.095f, 0.15f, 1.0f)),
                          ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersTop);
        dl->AddRectFilledMultiColor(ImVec2(wp.x, wp.y + headerH - 3.0f), ImVec2(wp.x + w, wp.y + headerH),
                                    ImGui::GetColorU32(ui::kLime), ImGui::GetColorU32(with_alpha(ui::kLime, 0.0f)),
                                    ImGui::GetColorU32(with_alpha(ui::kLime, 0.0f)), ImGui::GetColorU32(ui::kLime));

        ImGui::SetCursorPos(ImVec2(fs * 1.5f, fs * 0.7f));
        ImGui::BeginGroup();
        big_text(g_fontTitle, 1.8f, ui::kLime, "SUPER MARIO STRIKERS", false);
        ImGui::TextColored(ui::kDim, "ONLINE EDITION  \xC2\xB7  %s", PortUpdaterVersion());
        ImGui::EndGroup();

        // Status and close, right-aligned in the header.
        const NetStatus st = net_status();
        const char* kClose = "Close  (F1)";
        const float closeW = ImGui::CalcTextSize(kClose).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float pillW = pill_width(st.text);
        ImGui::SetCursorPos(ImVec2(w - fs * 1.5f - closeW - fs - pillW, (headerH - fs * 1.8f) * 0.5f));
        pill(st.text, st.color);
        ImGui::SameLine(0.0f, fs);
        ImGui::SetCursorPosY((headerH - (fs + ImGui::GetStyle().FramePadding.y * 2.0f)) * 0.5f);
        if (ImGui::Button(kClose))
            set_menu_open(false);

        // Left column: the pages.
        ImGui::SetCursorPos(ImVec2(0.0f, headerH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.8f, fs * 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.065f, 0.08f, 0.13f, 1.0f));
        if (ImGui::BeginChild("##menu_nav", ImVec2(fs * 13.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            for (int i = 0; i < PAGE_COUNT; i++)
            {
                if (i == PAGE_ADVANCED)
                    ImGui::Dummy(ImVec2(0.0f, fs * 0.8f));
                const char* sub = kPages[i].sub;
                char newSub[64];
                if (i == PAGE_UPDATES && PortUpdaterPoll() == PORT_UPDATE_AVAILABLE)
                {
                    std::snprintf(newSub, sizeof newSub, "New: %s!", PortUpdaterLatest());
                    sub = newSub;
                }
                if (nav_item(kPages[i].name, sub, s_page == i))
                    s_page = i;
            }
            // Keys, at the bottom.
            const float keysH = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
            if (ImGui::GetContentRegionAvail().y > keysH)
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y - keysH);
            ImGui::TextColored(ui::kDim, "F1   this menu");
            ImGui::TextColored(ui::kDim, "F2   frame rate");
            ImGui::TextColored(ui::kDim, "F11  fullscreen");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        // Right: the page.
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 1.5f, fs * 1.1f));
        if (ImGui::BeginChild("##menu_page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            ImGui::PopStyleVar();
            big_text(g_fontHead, 1.4f, ui::kText, kPages[s_page].title, false);
            ImGui::TextColored(ui::kDim, "%s", kPages[s_page].blurb);
            ImGui::Spacing();
            switch (s_page)
            {
            case PAGE_ONLINE: draw_netplay_tab(); break;
            case PAGE_CONTROLS: draw_controls_tab(); break;
            case PAGE_SOUND: draw_sound_tab(); break;
            case PAGE_DISPLAY: draw_display_page(); break;
            case PAGE_UPDATES: draw_updates_page(); break;
            default: draw_advanced_page(live); break;
            }
        }
        else
        {
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

}   // namespace

namespace
{

// STRIKERS_DEBUG_CMD: queue commands from the environment, for a headless run.
void queue_scripted_commands(unsigned long frame)
{
    static const char* s_spec = std::getenv("STRIKERS_DEBUG_CMD");
    static bool s_fired[32];
    if (s_spec == nullptr)
        return;
    int index = 0;
    for (const char* p = s_spec; *p != '\0' && index < 32; index++)
    {
        const char* end = std::strchr(p, ';');
        const size_t len = end != nullptr ? (size_t)(end - p) : std::strlen(p);
        char item[160];
        if (len >= sizeof item)
            break;
        std::memcpy(item, p, len);
        item[len] = '\0';
        p += len + (end != nullptr ? 1 : 0);

        char* at = std::strchr(item, '@');
        if (at == nullptr || s_fired[index])
            continue;
        *at = '\0';
        const unsigned long when = std::strtoul(at + 1, nullptr, 10);
        if (frame != when)
            continue;
        s_fired[index] = true;

        // The syntax after the @frame is the control channel's cmd line, parsed there so the two
        // cannot drift apart.
        PortDebugCommand k;
        if (!PortDebugParseCommand(item, &k))
            continue;
        PortDebugPushCommand(&k);
        std::fprintf(stderr, "[overlay] scripted command %d at frame %lu\n", k.op, frame);
    }
}

}   // namespace

void PortOverlayDraw(void)
{
    // Before the read, not after: STRIKERS_OVERLAY=menu is what opens the menu and this is where
    // that is first seen.
    PortOverlayInit();
    queue_scripted_commands(PortInputFrame());
    sim_rate_tick();

    // Pump netplay packets and lobby matchmaking every rendered frame
    NetplayLobby::Instance().Update();
    NetplayManager::Instance().Update();

    // STRIKERS_CONTROL, polled here because this runs every frame before the STRIKERS_OVERLAY test
    // and inside Aurora's frame, which is where shot has to ask for its readback.
    PortControlPoll();

    // Player 1's controller and the sound levels this PC keeps (Controls and Sound tabs).
    prefs_tick();

    // Text size: the fonts were made for the window the game opened with; follow the window
    // when it is resized, times the size chosen on the Display page.
    {
        ImGuiIO& io = ImGui::GetIO();
        float target = io.DisplaySize.y > 0.0f ? io.DisplaySize.y / 56.0f : g_fontBasePx;
        target = target < 14.0f ? 14.0f : (target > 40.0f ? 40.0f : target);
        const float follow = g_fontBody != nullptr ? target / g_fontBasePx : 1.0f;
        io.FontGlobalScale = follow * s_prefs.uiScale;
    }

    // While online (or getting there) and the menu is closed: a small status badge, top right.
    {
        const NetStatus st = net_status();
        if (st.show && !s_menuOpen)
        {
            const ImGuiIO& io = ImGui::GetIO();
            const float fs = ImGui::GetFontSize();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - fs * 0.8f, fs * 0.8f), ImGuiCond_Always,
                                    ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            if (ImGui::Begin("##netplay_corner_badge", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
            {
                pill(st.text, st.color);
                if (NetplayLobby::Instance().GetState() == LOBBY_STATE_HOSTING &&
                    NetplayLobby::Instance().GetNotice()[0] != '\0')
                    ImGui::TextColored(ui::kOrange, "Your friend left. They can rejoin with code %s.",
                                       NetplayLobby::Instance().GetRoomCode());
            }
            ImGui::End();
            ImGui::PopStyleVar(2);
        }
    }

    autotest_tick();
    online_start_tick();
    online_end_tick();
    if (s_ctl.slot >= 0 && !s_menuOpen)
        controls_end_capture("");   // menu closed while waiting for a button

    // Online VS screen: pops up by itself once both players are in the room and
    // no match is running.  While it is up, the keyboard drives it, not the game.
    {
        static bool s_vsTookKeyboard = false;
        const bool showVs = NetplayManager::Instance().IsActive() && !PortMatchExists() && !s_menuOpen;
        const bool wantKeyboard = showVs && s_session.taskState == 4;
        if (wantKeyboard != s_vsTookKeyboard)
        {
            s_vsTookKeyboard = wantKeyboard;
            if (!s_menuOpen)
                PADSetKeyboardActive(0, (!wantKeyboard && keyboard_pref_active()) ? TRUE : FALSE);
        }
        if (showVs)
        {
            const ImGuiIO& io = ImGui::GetIO();
            const float fs = ImGui::GetFontSize();
            ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(4, 6, 12, 110));
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 1.5f, fs * 1.2f));
            if (ImGui::Begin("Online VS##netplay_vs", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings))
            {
                big_text(g_fontTitle, 1.8f, ui::kLime, "CHOOSE YOUR TEAM", false);
                const NetStatus st = net_status();
                ImGui::SameLine(0.0f, fs * 2.0f);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + fs * 0.35f);
                pill(st.text, st.color);
                ImGui::Spacing();
                draw_online_vs();
                ImGui::TextColored(ui::kDim, "F1 opens the menu (Controls, Sound...).");
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }
    }

    // The opponent went quiet: say so instead of looking frozen.
    {
        NetplayManager& np = NetplayManager::Instance();
        const float quiet = np.IsActive() ? np.GetSecondsSinceLastPacket() : 0.0f;
        if (quiet > 1.5f)
        {
            if (banner_begin("##netplay_quiet", 0.62f, ui::kOrange))
            {
                big_text(g_fontHead, 1.6f, ui::kOrange, "Connection lost... waiting for your opponent", true);
                const int left = 10 - (int)quiet;
                char line[96];
                std::snprintf(line, sizeof line, "Back to the menu in %d s if they don't return.", left > 0 ? left : 0);
                centered_text(ui::kText, line);
            }
            ImGui::End();
        }
    }

    // Online resync banner: after a goal, replay or pause both games wait for each other, sync,
    // and count down together.
    {
        NetplayManager& np = NetplayManager::Instance();
        const int phase = np.IsActive() ? np.GetResyncPhase() : (int)NetplayManager::RESYNC_NONE;
        static int s_prevPhase = NetplayManager::RESYNC_NONE;
        static u64 s_goUntilUs = 0;
        const u64 nowUs = NetplayGetTimeUs();
        if (s_prevPhase == NetplayManager::RESYNC_COUNTDOWN && phase == NetplayManager::RESYNC_NONE)
            s_goUntilUs = nowUs + 700000ULL;
        s_prevPhase = phase;

        const bool showGo = phase == NetplayManager::RESYNC_NONE && nowUs < s_goUntilUs;
        const bool held   = phase == NetplayManager::RESYNC_NONE && np.IsActive() && np.IsWaitingForOpponentBreak();
        if (phase != NetplayManager::RESYNC_NONE || showGo || held)
        {
            const ImVec4 accent = showGo || phase == NetplayManager::RESYNC_COUNTDOWN ? ui::kLime : ui::kYellow;
            if (banner_begin("##netplay_resync", 0.38f, accent))
            {
                if (held)
                {
                    big_text(g_fontHead, 2.0f, ui::kYellow, "Your opponent stopped the game", true);
                    centered_text(ui::kText, "Pause menu or replay. Press A or B to skip a replay.");
                }
                else if (showGo)
                {
                    big_text(g_fontHuge, 4.0f, ui::kLime, "GO!", true);
                }
                else if (phase == NetplayManager::RESYNC_COUNTDOWN)
                {
                    const float t = np.GetCountdownSeconds();
                    char num[8];
                    std::snprintf(num, sizeof num, "%d", (int)(t + 0.999f) > 0 ? (int)(t + 0.999f) : 1);
                    big_text(g_fontHuge, 4.0f, ui::kText, num, true);
                }
                else if (phase == NetplayManager::RESYNC_TRANSFER)
                {
                    big_text(g_fontHead, 2.0f, ui::kText, "Syncing...", true);
                }
                else if (!np.IsLocalReadyForResync())
                {
                    big_text(g_fontHead, 2.0f, ui::kLime, "Your opponent is ready!", true);
                    centered_text(ui::kText, "Press A or B to skip the replay (it skips for both of you).");
                }
                else
                {
                    big_text(g_fontHead, 2.0f, ui::kYellow, "Waiting for your opponent...", true);
                    centered_text(ui::kText, "Press A or B to skip the replay for both of you.");
                }
            }
            ImGui::End();
        }
    }

    // Is there a newer release? Asked once at start; when there is, the menu opens on Updates.
    if (update_tick())
    {
        s_page = PAGE_UPDATES;
        set_menu_open(true);
    }

    // A hint for the first seconds: where the menu is.
    {
        static double s_firstDraw = -1.0;
        const double now = ImGui::GetTime();
        if (s_firstDraw < 0.0)
            s_firstDraw = now;
        const double age = now - s_firstDraw;
        if (!s_menuOpen && age < 12.0 && !NetplayManager::Instance().IsActive())
        {
            const float alpha = age > 10.0 ? (float)(12.0 - age) * 0.5f : 1.0f;
            const ImGuiIO& io = ImGui::GetIO();
            const float fs = ImGui::GetFontSize();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - fs * 0.8f, io.DisplaySize.y - fs * 0.8f),
                                    ImGuiCond_Always, ImVec2(1.0f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            if (ImGui::Begin("##menu_hint", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
                pill("Press F1 for the menu: Play Online, Controls, Sound", ui::kLime);
            ImGui::End();
            ImGui::PopStyleVar(3);
        }
    }

    const bool menu = s_menuOpen;
    if (!menu && !s_enabled)
        return;

    PortBenchLive live;
    PortBenchGetLive(&live);

    // Say once, on stderr, that the overlay actually drew and how big ImGui thinks the display is.
    static bool s_said = false;
    if (!s_said)
    {
        s_said = true;
        const ImGuiIO& io = ImGui::GetIO();
        std::fprintf(stderr, "[overlay] drawing; imgui display %.0fx%.0f, context %s\n",
                     (double)io.DisplaySize.x, (double)io.DisplaySize.y,
                     ImGui::GetCurrentContext() != nullptr ? "live" : "NULL");
    }

    if (!menu)
    {
        // F2: the frame rate, small, top left.  Nothing to click.
        const float fs = ImGui::GetFontSize();
        ImGui::SetNextWindowPos(ImVec2(fs * 0.8f, fs * 0.8f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.72f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.8f, fs * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, fs * 0.6f);
        if (ImGui::Begin("##perf_hud", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
        {
            {
                FontScope f(g_fontBold);
                const ImVec4 col = live.fps >= 55.0f ? ui::kLime : (live.fps >= 40.0f ? ui::kYellow : ui::kOrange);
                ImGui::TextColored(col, "%.0f FPS", (double)live.fps);
            }
            ImGui::SameLine();
            ImGui::TextColored(ui::kDim, "%.1f ms  (busy %.1f, p95 %.1f)", (double)live.frameMs,
                               (double)live.busyMs, (double)live.busyP95Ms);
            ImGui::TextColored(ui::kDim, "%s  \xC2\xB7  %s", s_scene, s_stadium);
            if (live.matchActive)
                ImGui::TextColored(ui::kDim, "match %.0f s   %d - %d", (double)s_clock, s_scoreHome, s_scoreAway);
            if (NetplayManager::Instance().IsActive())
                ImGui::TextColored(ui::kDim, "online: delay %d, rollback %d, advantage %+d",
                                   NetplayManager::Instance().GetInputDelay(),
                                   NetplayManager::Instance().GetRollbackFrames(),
                                   NetplayManager::Instance().GetFrameAdvantage());
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    draw_main_menu(live);
}
