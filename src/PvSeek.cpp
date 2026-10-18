// PvSeek - jump to any time inside a PV in Hatsune Miku: Project DIVA Mega Mix+
// A DivaModLoader (DML) DLL mod.
//
// HOW IT WORKS (short version, details are in README.md)
//   The game plays a PV by running a script (DSC) command by command. Every command
//   that starts a motion, camera cut, stage change or 2D animation already knows how to
//   run "late": it works out the right start frame from (current time - command time).
//   The game also keeps an offset in its PV data that is added to the PV clock.
//
//   So a seek is done like this:
//     1. call the game's own pv_game::restart()  -> game resets the PV state to time 0
//     2. write the target time into the PV clock offset
//     3. on the next frame the game runs every script command up to the target time in
//        one go, using its own late-start logic, so motions/camera/2D land on the
//        correct frame
//     4. the music start is shifted by the same amount (hook on pv_game_music::play)
//
// STATUS: addresses below were checked against the installed Mega Mix+ executable.
//   The mode field is checked against the installed game's PV Viewer value (type 3).
//   Runtime behavior still needs an in-game check.
//
//   Checks that lower the risk (they cannot remove it):
//     - the hook target must look like a function start (16-byte aligned, after padding)
//     - the first time it runs, the object it receives must have the expected layout
//       (back pointer inside the PV data points to the pv_game); otherwise the mod disables itself
//     - pv_game::restart is compiled in and checked for a valid function start
//   Calling a wrong restart() address WILL crash the game, so confirm that one by hand.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>

#if __has_include(<detours/detours.h>)
#include <detours/detours.h>
#else
#include <detours.h>
#endif

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------------------
// Game constants (from ReDIVA)
// ---------------------------------------------------------------------------------------
namespace Game
{
    // Preferred image base is 0x140000000. These fixed addresses match the values
    // from the supplied pvseek.ini and are compiled into the DLL.
    constexpr uintptr_t kImageBase = 0x140000000;
    constexpr uintptr_t kModeVar   = 0x140DAE930; // current exe: pv_game_data.type (int32)
    constexpr uintptr_t kTickFn    = 0x14024B0E0; // fixed per-frame PV hook address
    constexpr uintptr_t kAnchorA   = 0x140104FB0; // fallback restart-search anchor
    constexpr uintptr_t kAnchorB   = 0x1400FCEB0; // fallback restart-search anchor
    constexpr uintptr_t kRestartFn = 0x1402436F0; // pv_game::restart
    constexpr uintptr_t kMusicPlayFn = 0x14023E2C0; // pv_game_music::play
    constexpr uintptr_t kMusicLoadFn = 0x14023E440; // pv_game_music::load

    // Offsets inside pv_game_pv_data (ReDIVA field names are the original offsets).
    constexpr size_t kPvEnabled  = 0x00000; // bool  field_0
    constexpr size_t kPvPlay     = 0x00008; // bool  play
    constexpr size_t kPvDsc      = 0x0000C; // uint32 dsc_buffer[45000]
    [[maybe_unused]] constexpr size_t kPvOrigin = 0x2BF30; // int64 field_2BF30 (time origin, -1 = not set yet; reset by restart)
    constexpr size_t kPvOffset   = 0x2BF38; // int64 field_2BF38 (added to the PV clock)
    constexpr size_t kPvTime     = 0x2BF40; // int64 curr_time (nanoseconds)
    constexpr size_t kPvPvGame   = 0x2BF78; // pv_game* back pointer
    constexpr size_t kPvMusic    = 0x2BF88; // pv_game_music*
    constexpr size_t kPvDscTime  = 0x2BFA8; // int64 dsc_time (time of the command being run)
    // ReDIVA pv_game_pv_data and rob_chara layout used to repair SET_MOTION after seek catch-up.
    constexpr uintptr_t kRobReqFrameChange = 0x1404E22F0; // verified current-exe rob_chara::req_frame_change
    constexpr size_t kPvPlaydata = 0x2BFD8;
    constexpr size_t kPvPlaydataStride = 0xA8; // sizeof(pv_play_data): vector + ptr + list + flags + motion_data
    constexpr size_t kPvPlaydataRobChara = 0x18;
    constexpr size_t kPvSetMotionActive = 0x2C560;
    constexpr size_t kPvSetMotionRecords = 0x2C568;
    constexpr size_t kPvSetMotionStride = 0x30; // sizeof(pv_play_data_set_motion)
    constexpr size_t kPvSetMotionFrame = 0x08;
    constexpr size_t kPvSetMotionTime = 0x20;
    constexpr size_t kRobIdMax = 6;


    constexpr size_t   kDscMaxWords = 45000;
    constexpr uint32_t kDscSignature = 0x15122517;   // FT script
    constexpr uint32_t kDscSignatureF2 = 0x14050921; // F2 script (older songs). TIME=1, PV_END=32 as well.
    constexpr int kF2Count = 111;
    constexpr int kF2Len[kF2Count] = { 0,1,4,2,2,2,12,4,2,6,2,1,6,2,2,1,3,2,3,5,5,4,4,5,2,0,2,4,2,2,1,21,0,3,2,5,1,1,7,1,1,2,1,2,1,2,3,3,1,2,2,3,6,6,1,1,2,3,1,2,2,4,4,1,2,1,2,1,1,3,3,3,2,1,9,3,2,4,2,3,2,22,1,2,1,6,1,9,2,3,3,11,3,2,3,3,4,1,1,3,3,4,1,112,1,1,7,2,2,2,5 };
    constexpr int      kFtCount = 107;
    constexpr uint32_t kFtTime = 1, kFtPvEnd = 32;

    // Number of arguments of every DSC command (FT format), from KKdLib/dsc.cpp.
    constexpr int kFtLen[kFtCount] = {
        0, 1, 4, 2, 2, 2, 7, 4, 2, 6, 2, 1, 6, 2, 1, 1,
        3, 2, 3, 5, 5, 4, 4, 5, 2, 0, 2, 4, 2, 2, 1, 21,
        0, 3, 2, 5, 1, 1, 7, 1, 1, 2, 1, 2, 1, 2, 3, 3,
        1, 2, 2, 3, 6, 6, 1, 1, 2, 3, 1, 2, 2, 4, 4, 1,
        2, 1, 2, 1, 1, 3, 3, 3, 2, 1, 9, 3, 2, 4, 2, 3,
        2, 24, 1, 2, 1, 3, 1, 3, 4, 1, 2, 6, 3, 2, 3, 3,
        4, 1, 1, 3, 3, 4, 1, 3, 3, 8, 2,
    };
}

// ---------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------
// The hooked function must have exactly this shape: only `this` (pv_game*) in, an int/bool out.
// A function with a float or extra arguments would lose them through this hook.
using TickFn      = bool(__fastcall*)(void* pvGame);
using RestartFn   = void(__fastcall*)(void* pvGame);
using MusicPlayFn = int32_t(__fastcall*)(void* music, int32_t type, void* path, bool playOnEnd,
                                         float start, float end, float fadeIn, float fadeOut, bool a9);

using MusicLoadFn = int32_t(__fastcall*)(void* music, int32_t type, void* path, bool waitLoad,
                                         float time, bool a6);

struct Settings
{
    uintptr_t tickAddr      = Game::kTickFn;  // fixed function we hook; gets pv_game* in rcx every frame
    uintptr_t modeAddr      = Game::kModeVar; // fixed int32 address holding the game mode
    uintptr_t anchorA       = Game::kAnchorA; // only used to auto-detect restart
    uintptr_t anchorB       = Game::kAnchorB;
    uintptr_t restartAddr   = Game::kRestartFn; // fixed pv_game::restart address
    uintptr_t musicPlayAddr = Game::kMusicPlayFn; // fixed pv_game_music::play address
    uintptr_t musicLoadAddr = Game::kMusicLoadFn; // fixed pv_game_music::load address
    int       viewerMode    = 3;   // observed in the running game's PV Viewer
    double    stepSmall     = 5.0; // seconds
    double    stepBig       = 30.0;
    bool      showWindow    = true;
    bool      fixPauseJump  = true;  // undo the game's clock-offset jump on pause/resume after a seek
    bool      forwardNoRestart = false;  // seeking forward only moves the PV clock (no restart()); music does not follow
    bool      backwardNoRestart = false; // same for seeking backward (the picture can get out of sync, see README)
};

static struct State
{
    HMODULE   self = nullptr;
    intptr_t  slide = 0;                       // real image base - 0x140000000
    Settings  cfg;
    TickFn      origTick = nullptr;
    RestartFn   restart = nullptr;
    MusicPlayFn origMusicPlay = nullptr;
    MusicLoadFn origMusicLoad = nullptr;
    bool      loadConfirmed = false;           // the fixed MusicLoad address was supplied and passed checks
    long long musicNs = 0;                     // script time of the MUSIC_PLAY command (from the script)
    bool      inRestart = false;               // true while we call restart()
    float     pendingStart = 0.0f;             // music start (seconds) to use for load() during restart
    void*     musicObj = nullptr;              // pv_game_music*, seen in the load() hook

    uint8_t* game = nullptr;                   // pv_game*
    uint8_t* pv   = nullptr;                   // pv_game_pv_data*
    bool     disabled = false;
    bool     replaying = false;                // true while the restarted PV catches up to the seek target
    bool     seeked = false;                   // a seek has changed the PV clock in this PV
    bool     haveOffset = false;
    long long lastOffset = 0;                  // PV clock offset as last observed or written by the mod
    long long replayTargetNs = 0;
    int      replayTicks = 0;
    int      replayCorrected = 0;
    uint32_t replayActiveMask = 0;
    uint32_t replayZeroMask = 0;
    int      dscTry = 0;




    std::atomic<long long> reqNs{ -1 };        // seek request from UI thread / hotkeys
    std::atomic<long long> curNs{ 0 };         // current PV time, for the UI
    std::atomic<long long> durNs{ -1 };        // PV length (-1 = unknown)
    std::atomic<bool>      active{ false };    // a PV is loaded
    std::atomic<bool>      toggleWindow{ false };
    std::atomic<bool>      windowWanted{ false };
    HWND hwnd = nullptr;
} g;

// ---------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------
static char g_dir[MAX_PATH];

static void InitPaths()
{
    GetModuleFileNameA(g.self, g_dir, MAX_PATH);
    char* slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0;
}

static void Log(const char* fmt, ...)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%spvseek.log", g_dir);
    FILE* f = fopen(path, "a");
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (f) { fprintf(f, "%s\n", line); fclose(f); }
    printf("[PvSeek] %s\n", line);
}

template <class T> static T Rd(const uint8_t* p, size_t off) { T v; memcpy(&v, p + off, sizeof(T)); return v; }
template <class T> static void Wr(uint8_t* p, size_t off, T v) { memcpy(p + off, &v, sizeof(T)); }

static uintptr_t Real(uintptr_t addr) { return addr + (uintptr_t)g.slide; }

static bool Readable(const void* p, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi;
    const uint8_t* cur = (const uint8_t*)p;
    const uint8_t* end = cur + size;
    while (cur < end)
    {
        if (!VirtualQuery(cur, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        cur = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

static void LoadSettings()
{
    char path[MAX_PATH], buf[64];
    snprintf(path, sizeof(path), "%spvseek.ini", g_dir);

    auto num = [&](const char* key, uintptr_t def) -> uintptr_t {
        GetPrivateProfileStringA("PvSeek", key, "", buf, sizeof(buf), path);
        return buf[0] ? (uintptr_t)strtoull(buf, nullptr, 0) : def;
    };
    auto dbl = [&](const char* key, double def) -> double {
        GetPrivateProfileStringA("PvSeek", key, "", buf, sizeof(buf), path);
        return buf[0] ? atof(buf) : def;
    };
    g.cfg.viewerMode    = (int)num("ViewerMode", 3);
    g.cfg.stepSmall     = dbl("StepSmallSeconds", 5.0);
    g.cfg.stepBig       = dbl("StepBigSeconds", 30.0);
    g.cfg.showWindow    = num("ShowWindow", 1) != 0;
    g.cfg.fixPauseJump  = num("FixPauseJump", 1) != 0;
    g.cfg.forwardNoRestart = num("ForwardWithoutRestart", 0) != 0;
    g.cfg.backwardNoRestart = num("BackwardWithoutRestart", 0) != 0;
}

// The mode address points to pv_game_data.type in this installed executable.
// The running game reported type 3 while the user was in PV Viewer. Require exact
// equality so Practice, normal play and all other/unreadable states are denied.
static bool IsPvViewerMode()
{
    const void* modeAddr = (const void*)Real(g.cfg.modeAddr);
    if (!Readable(modeAddr, sizeof(int32_t)))
    {
        static bool unreadableLogged = false;
        if (!unreadableLogged) { Log("PV Viewer check failed closed: ModeAddress 0x%llX is not readable", (unsigned long long)g.cfg.modeAddr); unreadableLogged = true; }
        return false;
    }

    int32_t mode;
    memcpy(&mode, modeAddr, sizeof(mode));
    static int32_t lastMode = 0;
    static bool loggedMode = false;
    if (!loggedMode || mode != lastMode)
    {
        Log("game mode value = %d (PV Viewer requires ViewerMode = %d)", mode, g.cfg.viewerMode);
        lastMode = mode;
        loggedMode = true;
    }
    return mode >= 0 && mode <= 32 && mode == g.cfg.viewerMode;
}

static void HideSeekUi()
{
    g.active = false;
    g.reqNs = -1;
    g.toggleWindow = false;
    if (g.hwnd) ShowWindow(g.hwnd, SW_HIDE);
}

static void ResetPvState()
{
    const bool replayPending = g.replaying;
    g.pv = nullptr;
    g.durNs = -1;
    g.dscTry = 0;
    g.musicNs = 0;

    // restart() can briefly mark the PV unloaded while it rebuilds its state.
    // Keep a pending seek alive so the first loaded catch-up frames can be repaired.
    if (!replayPending)
    {
        g.replaying = false;
        g.replayTargetNs = 0;
        g.replayTicks = 0;
        g.replayCorrected = 0;
        g.replayActiveMask = 0;
        g.replayZeroMask = 0;
        g.seeked = false;
        g.haveOffset = false;
        g.lastOffset = 0;
    }
}

// After a seek, the game's pause code can add a large jump to the PV clock offset.
// Restore the last known offset before the next frame so the PV and audio stay paused
// at the same position when playback resumes.
static void GuardOffset()
{
    using namespace Game;
    if (!g.pv || !g.cfg.fixPauseJump) return;

    const long long offset = Rd<int64_t>(g.pv, kPvOffset);
    if (g.haveOffset && g.seeked && offset - g.lastOffset > 1'000'000'000LL)
    {
        static int logs = 0;
        if (logs++ < 20)
            Log("pause fix: game moved the clock offset by +%.3f s (%.3f -> %.3f); restored it",
                (offset - g.lastOffset) * 1e-9, g.lastOffset * 1e-9, offset * 1e-9);
        Wr<int64_t>(g.pv, kPvOffset, g.lastOffset);
        return;
    }

    g.lastOffset = offset;
    g.haveOffset = true;
}

// ---------------------------------------------------------------------------------------
// Finding things inside the game
// ---------------------------------------------------------------------------------------
static bool GetTextSection(uint8_t** text, size_t* size)
{
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto first = IMAGE_FIRST_SECTION(nt);
    auto sec = first;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (memcmp(sec->Name, ".text", 5) == 0)
        {
            *text = base + sec->VirtualAddress;
            *size = sec->Misc.VirtualSize;
            return true;
        }

    // No section called ".text": this exe is packed/renamed. The game code is in an executable
    // section with some other name. Log them all and take the first sensible executable one.
    static bool logged = false;
    const IMAGE_SECTION_HEADER* best = nullptr;
    sec = first;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        bool exec = (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (!logged) Log("section %u: %.8s  flags 0x%08lX  size 0x%lX%s", i, (const char*)sec->Name,
                         (unsigned long)sec->Characteristics, (unsigned long)sec->Misc.VirtualSize, exec ? "  (code)" : "");
        if (exec && sec->Misc.VirtualSize <= 0x4000000 && !best) best = sec;
    }
    logged = true;
    if (!best) return false;
    *text = base + best->VirtualAddress;
    *size = best->Misc.VirtualSize;
    return true;
}

// The executable section that contains <addr> (the right place to search for things near it).
static bool GetCodeSectionContaining(uintptr_t addr, uint8_t** text, size_t* size)
{
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        uintptr_t lo = (uintptr_t)(base + sec->VirtualAddress), hi = lo + sec->Misc.VirtualSize;
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) && addr >= lo && addr < hi)
        {
            *text = (uint8_t*)lo;
            *size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return GetTextSection(text, size);
}

static bool CallsTarget(const uint8_t* p, uintptr_t target)
{
    if (*p != 0xE8) return false;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    return (uintptr_t)(p + 5 + rel) == target;
}

// MSVC aligns functions to 16 bytes and fills the gap with int3 (0xCC). A real function
// start is therefore aligned and preceded by 0xCC (or by the ret of the previous function).
static bool LooksLikeFunctionStart(const uint8_t* p)
{
    if (((uintptr_t)p & 0xF) != 0) return false;
    if (!Readable(p - 1, 17)) return false;
    return (p[-1] == 0xCC || p[-1] == 0xC3) && p[0] != 0xCC;
}

static void DumpBytes(const char* what, uintptr_t addr)
{
    const uint8_t* p = (const uint8_t*)addr;
    if (!Readable(p - 1, 17)) { Log("%s @ 0x%llX: not readable", what, (unsigned long long)addr); return; }
    Log("%s @ 0x%llX: [%02X] %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X  function-start=%s",
        what, (unsigned long long)addr, p[-1], p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],
        p[10], p[11], LooksLikeFunctionStart(p) ? "yes" : "NO");
}

// pv_game::restart is the one function that calls both anchor functions.
static uintptr_t FindRestart()
{
    uint8_t* text; size_t size;
    if (!GetTextSection(&text, &size)) return 0;

    const uintptr_t a = Real(g.cfg.anchorA), b = Real(g.cfg.anchorB);
    std::vector<uintptr_t> found;

    for (uint8_t* p = text; p + 5 <= text + size; ++p)
    {
        if (!CallsTarget(p, a)) continue;

        // walk back to the start of the function: 16-byte aligned, right after int3 padding / a ret
        uint8_t* s = p;
        uint8_t* limit = (p - text > 0x2000) ? p - 0x2000 : text + 1;
        while (s > limit && !(((uintptr_t)s & 0xF) == 0 && (s[-1] == 0xCC || s[-1] == 0xC3) && s[0] != 0xCC)) --s;
        if (s <= limit) continue;

        // the function ends at the next run of two int3 bytes (padding)
        uint8_t* e = p;
        uint8_t* elimit = (p + 0x2000 < text + size - 1) ? p + 0x2000 : text + size - 1;
        while (e < elimit && !(e[0] == 0xCC && e[1] == 0xCC)) ++e;

        bool hasB = false;
        for (uint8_t* q = s; q + 5 <= e; ++q)
            if (CallsTarget(q, b)) { hasB = true; break; }
        if (!hasB) continue;

        bool dup = false;
        for (uintptr_t f : found) dup |= (f == (uintptr_t)s);
        if (!dup) found.push_back((uintptr_t)s);
    }

    if (found.size() == 1) return found[0];
    Log("restart auto-detect: %zu candidates (need exactly 1)", found.size());
    for (uintptr_t c : found) Log("  candidate 0x%llX", (unsigned long long)c);
    return 0;
}

// pv_game_music::load is called by restart. It is the only function there that has
// "return -6" in it (mov eax, 0FFFFFFFAh). Look at every function that restart calls.
static uintptr_t FindMusicLoad(uintptr_t restart)
{
    uint8_t* text; size_t size;
    if (!restart) { Log("music load auto-detect: restart address unknown"); return 0; }
    if (!GetCodeSectionContaining(restart, &text, &size)) { Log("music load auto-detect: could not find the code section of the exe"); return 0; }
    if (restart < (uintptr_t)text || restart >= (uintptr_t)(text + size))
        Log("note: restart (0x%llX) is outside the code section 0x%llX..0x%llX",
            (unsigned long long)restart, (unsigned long long)(uintptr_t)text, (unsigned long long)(uintptr_t)(text + size));
    const uint8_t* s = (const uint8_t*)restart;
    const uint8_t* e = s;
    while (e < text + size - 1 && e < s + 0x2000 && !(e[0] == 0xCC && e[1] == 0xCC)) ++e;

    std::vector<uintptr_t> targets, found;
    for (const uint8_t* q = s; q + 5 <= e; ++q)
    {
        if (*q != 0xE8) continue;
        int32_t rel; memcpy(&rel, q + 1, 4);
        uintptr_t t = (uintptr_t)(q + 5 + rel);
        if (t < (uintptr_t)text || t >= (uintptr_t)(text + size)) continue;
        bool dup = false;
        for (uintptr_t x : targets) dup |= (x == t);
        if (!dup) targets.push_back(t);
    }
    Log("restart calls %zu different functions (exe addresses, in order of appearance):", targets.size());
    for (uintptr_t t0 : targets)
    {
        // Tiny "jmp target" stubs (5 bytes, E9) just forward the call: look at where they lead.
        uintptr_t t = t0;
        for (int hop = 0; hop < 3 && Readable((const void*)t, 5) && *(const uint8_t*)t == 0xE9; ++hop)
        {
            int32_t rel; memcpy(&rel, (const uint8_t*)t + 1, 4);
            t = t + 5 + rel;
        }
        const uint8_t* f = (const uint8_t*)t;
        if (!Readable(f, 0x2000) || !LooksLikeFunctionStart(f))
        {
            Log("  call -> 0x%llX  (not a function start, skipped)", (unsigned long long)(t - (uintptr_t)g.slide));
            continue;
        }
        const uint8_t* fe = f;
        while (fe < f + 0x2000 && !(fe[0] == 0xCC && fe[1] == 0xCC)) ++fe;
        bool hit = false;
        for (const uint8_t* q = f; q + 5 <= fe; ++q)
        {
            if (q[0] >= 0xB8 && q[0] <= 0xBF && q[1] == 0xFA && q[2] == 0xFF && q[3] == 0xFF && q[4] == 0xFF) hit = true;   // mov reg,-6
            if (q[0] == 0x83 && (q[1] & 0xF8) == 0xC8 && q[2] == 0xFA) hit = true;                                         // or reg,-6
        }
        Log("  call -> 0x%llX%s  size 0x%zX  has -6: %s", (unsigned long long)(t0 - (uintptr_t)g.slide),
            t != t0 ? " (jump stub)" : "", (size_t)(fe - f), hit ? "YES" : "no");
        if (hit) found.push_back(t);
    }
    if (found.size() == 1) return found[0];
    Log("music load auto-detect: %zu candidates (need exactly 1)", found.size());
    for (uintptr_t c : found) Log("  candidate 0x%llX", (unsigned long long)(c - (uintptr_t)g.slide));
    return 0;
}

// pv_game_pv_data is the first member of pv_game::data. Its back pointer (at +0x2BF78)
// points to the pv_game, so we look for that pointer to find where pv_data starts.
static uint8_t* LocatePvData(uint8_t* pvGame)
{
    using namespace Game;
    constexpr size_t kScan = 0x800; // pv_data is the first member of pv_game::data, so it sits near the start
    if (!Readable(pvGame, kScan + kPvMusic + 8)) return nullptr;

    for (size_t d = 0; d <= kScan; d += 8)
    {
        uint8_t* x = pvGame + d;
        if (Rd<uint8_t*>(x, kPvPvGame) != pvGame) continue;
        if (Rd<uint8_t>(x, kPvEnabled) > 1 || Rd<uint8_t>(x, kPvPlay) > 1) continue;
        if (!Rd<void*>(x, kPvMusic)) continue;
        int64_t t = Rd<int64_t>(x, kPvTime);
        if (t < 0 || t > 4'000'000'000'000LL) continue;
        Log("pv_game_pv_data found at pv_game + 0x%zX", d);
        return x;
    }
    return nullptr;
}

// Walk the script to find when PV_END runs (= length of the PV).
static void ParseDsc()
{
    using namespace Game;
    const uint32_t* d = (const uint32_t*)(g.pv + kPvDsc);
    const bool f2 = d[0] == kDscSignatureF2;
    const int* lens = f2 ? kF2Len : kFtLen;
    const uint32_t count = f2 ? (uint32_t)kF2Count : (uint32_t)kFtCount;
    size_t i = (d[0] == kDscSignature || f2) ? 1 : 0;

    int64_t timeNs = 0, endNs = -1;
    bool sawMusic = false;
    size_t stopAt = 0; uint32_t stopOp = 0;

    while (i < kDscMaxWords)
    {
        uint32_t f = d[i++];
        if (f == 0) break;                       // END
        if (f >= count) { endNs = -1; stopAt = i - 1; stopOp = f; break; }

        if (f == 25 && !sawMusic) { sawMusic = true; g.musicNs = timeNs; }   // MUSIC_PLAY
        if (f == kFtTime && i < kDscMaxWords) timeNs = 10000LL * (int32_t)d[i];
        if (f == kFtPvEnd) { endNs = timeNs; break; }
        i += (size_t)lens[f];
    }
    g.durNs = endNs;
    static int failLogs = 0;
    if (endNs >= 0 || failLogs < 2)
    {
        Log("PV length from script: %.3f s", endNs >= 0 ? endNs * 1e-9 : -1.0);
        if (endNs < 0)
        {
            ++failLogs;
            if (stopOp) Log("  walk stopped at word %zu: unknown command %u (0x%X)", stopAt, stopOp, stopOp);
            Log("  MUSIC_PLAY found at script time %.3f s", sawMusic ? g.musicNs * 1e-9 : -1.0);
            Log("  script buffer starts with: %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11]);
        }
    }
}

// ---------------------------------------------------------------------------------------
// The seek itself
// ---------------------------------------------------------------------------------------
// After restart() the music object remembers "loaded at <start>" (args.start). The script's
// MUSIC_PLAY asks for start 0, sees a difference and reloads the song from 0:00. We set the
// remembered start back to 0, so MUSIC_PLAY just un-pauses the song that is already at <start>.
static void FixMusicArgs(float start)
{
    uint8_t* m = (uint8_t*)g.musicObj;
    if (!m) { Log("music: load() was not called during restart - music will start at 0:00"); return; }
    constexpr size_t kLo = 0x98, kHi = 0x100;
    if (!Readable(m, kHi)) { Log("music: object not readable"); return; }
    uint32_t bits; memcpy(&bits, &start, 4);
    size_t hit = 0, count = 0;
    for (size_t o = kLo; o + 4 <= kHi; o += 4)
        if (Rd<uint32_t>(m, o) == bits) { hit = o; ++count; }
    if (count != 1) { Log("music: found %zu places holding the start time %.3f (need 1) - music will start at 0:00", count, start); return; }
    Wr<float>(m, hit, 0.0f);
    Log("music: loaded at %.3f s, args.start (music+0x%zX) reset to 0", start, hit);
}

static void DoSeek(long long reqNs)
{
    using namespace Game;

    if (!IsPvViewerMode()) { Log("seek ignored: the game is not in PV Viewer mode"); return; }
    if (!Rd<uint8_t>(g.pv, kPvEnabled) || !Rd<uint8_t>(g.pv, kPvPlay)) return;

    long long target = reqNs < 0 ? 0 : reqNs;
    long long dur = g.durNs;
    if (dur > 0 && target > dur - 200'000'000LL) target = dur > 200'000'000LL ? dur - 200'000'000LL : 0;

    // Clock only (no restart, so no white flash). The music does not follow in this mode.
    // Forward: moving the clock ahead makes the game run every script command in between in one
    // frame, each starting "late" at the right frame.
    // Backward: the script only runs forward, so commands that already ran are not run again.
    // Motions, camera and 2D can end up out of sync until the script reaches new commands.
    long long now = Rd<int64_t>(g.pv, kPvTime);
    bool forward = target > now;
    if (forward ? g.cfg.forwardNoRestart : g.cfg.backwardNoRestart)
    {
        long long off = Rd<int64_t>(g.pv, kPvOffset);
        Log("seek %s to %.3f s (from %.3f s), clock only, no restart", forward ? "forward" : "backward",
            target * 1e-9, now * 1e-9);
        const long long newOffset = off + (target - now);
        Wr<int64_t>(g.pv, kPvOffset, newOffset);
        g.lastOffset = newOffset; g.haveOffset = true; g.seeked = true;
        return;
    }

    if (!g.restart) { Log("seek ignored: the fixed pv_game::restart address is unavailable; check kRestartFn in src/PvSeek.cpp"); return; }

    Log("seek to %.3f s (from %.3f s)", target * 1e-9, g.curNs.load() * 1e-9);

    // Music: restart() loads the song paused. If we make that load start at the right place and
    // then make the later MUSIC_PLAY call accept the loaded song, the audio follows the seek.
    float musicStart = target > g.musicNs ? (float)((target - g.musicNs) * 1e-9) : 0.0f;
    bool musicOn = g.origMusicLoad != nullptr && musicStart > 0.0f;
    g.pendingStart = musicStart;
    g.musicObj = nullptr;
    g.inRestart = musicOn;
    g.restart(g.game);                           // game rebuilds the PV state at time 0
    g.inRestart = false;
    if (musicOn) FixMusicArgs(musicStart);
    Wr<int64_t>(g.pv, kPvOffset, target);        // next pv_data.ctrl runs the script up to this time
    g.lastOffset = target; g.haveOffset = true; g.seeked = true;
    g.replayTargetNs = target;
    g.replayTicks = 0;
    g.replayCorrected = 0;
    g.replayActiveMask = 0;
    g.replayZeroMask = 0;
    g.replaying = true;
}

static void OnPvFrameImpl(uint8_t* pvGame)
{
    using namespace Game;
    if (g.disabled) return;

    if (!pvGame[0])
    {
        HideSeekUi();
        ResetPvState();
        return;
    } // pv_game::loaded

    if (!IsPvViewerMode()) { HideSeekUi(); return; } // PV Viewer only

    if (pvGame != g.game)
    {
        g.game = pvGame;
        ResetPvState();
    }
    if (!g.pv)
    {
        g.pv = LocatePvData(pvGame);
        if (!g.pv) { Log("could not find pv_game_pv_data inside pv_game - mod disabled"); g.disabled = true; return; }
        ParseDsc();
    }


    if (g.durNs.load() < 0 && g.dscTry < 1200 && (++g.dscTry % 120) == 0) ParseDsc();
    long long t = Rd<int64_t>(g.pv, kPvTime);
    g.curNs = t;
    GuardOffset();

    // The seek UI is only active in the exact PV Viewer/watch mode.
    g.active = true;
    if (g.toggleWindow.exchange(false)) g.windowWanted = !g.windowWanted.load();
    if (g.hwnd) ShowWindow(g.hwnd, g.windowWanted.load() ? SW_SHOWNOACTIVATE : SW_HIDE);

    long long req = g.reqNs.exchange(-1);
    if (req >= 0) DoSeek(req);
}

#ifdef _MSC_VER
static void OnPvFrame(uint8_t* pvGame)
{
    __try { OnPvFrameImpl(pvGame); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g.disabled = true;
        Log("exception inside seek code (0x%08lX) - mod disabled", GetExceptionCode());
    }
}
#else
static void OnPvFrame(uint8_t* pvGame) { OnPvFrameImpl(pvGame); }
#endif

// ---------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------
static void RequestRelative(double seconds)
{
    long long t = g.curNs.load() + (long long)(seconds * 1e9);
    g.reqNs = t < 0 ? 0 : t;
}

// [ and ] seek back / forward, hold Shift for the big step, F8 shows or hides the window.
static void PollHotkeys()
{
    static bool prev[3] = {};
    const int keys[3] = { VK_OEM_4, VK_OEM_6, VK_F8 };
    if (!g.active.load())
    {
        // Keep edge detection in sync so holding a key while entering the viewer does not fire.
        for (int i = 0; i < 3; ++i) prev[i] = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        return;
    }
    // only react while the game (or our window) is the foreground window
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return;

    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    for (int i = 0; i < 3; ++i)
    {
        bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down && !prev[i])
        {
            double step = shift ? g.cfg.stepBig : g.cfg.stepSmall;
            if (i == 0) RequestRelative(-step);
            if (i == 1) RequestRelative(step);
            if (i == 2) g.toggleWindow = true;
        }
        prev[i] = down;
    }
}

static void FixReplayedSetMotionFrames()
{
    using namespace Game;
    if (!g.pv || !g.game ||
        !Readable(g.pv, kPvSetMotionRecords + kRobIdMax * kPvSetMotionStride))
        return;

    const int64_t targetTime = g.replayTargetNs;
    if (targetTime <= 0 || targetTime > 4'000'000'000'000LL)
        return;

    const uintptr_t requestAddress = Real(kRobReqFrameChange);
    const uint8_t* requestCode = (const uint8_t*)requestAddress;
    // This leaf routine is intentionally unaligned in the current executable; validate its body,
    // not a generic function prologue/alignment heuristic.
    static const uint8_t expectedRequestCode[] = {
        0x80, 0x89, 0xF8, 0x06, 0x00, 0x00, 0x80,
        0xF3, 0x0F, 0x11, 0x89, 0xE0, 0x06, 0x00, 0x00, 0xC3
    };
    if (!Readable(requestCode, sizeof(expectedRequestCode)) ||
        memcmp(requestCode, expectedRequestCode, sizeof(expectedRequestCode)) != 0)
    {
        static bool logged = false;
        if (!logged)
        {
            Log("motion catch-up repair disabled: req_frame_change signature mismatch at 0x%llX",
                (unsigned long long)requestAddress);
            logged = true;
        }
        return;
    }

    using ReqFrameChangeFn = void(__fastcall*)(void* robChar, float frame);
    ReqFrameChangeFn requestFrame = (ReqFrameChangeFn)requestAddress;
    int corrected = 0;

    for (size_t robId = 0; robId < kRobIdMax; ++robId)
    {
        if (Rd<uint8_t>(g.pv, kPvSetMotionActive + robId) != 1)
            continue;

        g.replayActiveMask |= (1u << robId);
        const size_t record = kPvSetMotionRecords + robId * kPvSetMotionStride;
        const float originalFrame = Rd<float>(g.pv, record + kPvSetMotionFrame);
        if (!(originalFrame > -0.0001f && originalFrame < 0.0001f))
            continue;
        g.replayZeroMask |= (1u << robId);

        const int64_t motionStart = Rd<int64_t>(g.pv, record + kPvSetMotionTime);
        if (motionStart < 0 || motionStart >= targetTime)
            continue;

        const int64_t elapsed = targetTime - motionStart;
        if (elapsed <= 0 || elapsed > 3'600'000'000'000LL)
            continue;

        // ReDIVA stores frame_speed after applying anim_frame_speed. Match its late-start path.
        float frameSpeed = Rd<float>(g.pv, record);
        if (!(frameSpeed >= 0.0f && frameSpeed <= 100.0f))
            frameSpeed = 1.0f;
        const float correctedFrame = (float)((double)elapsed * 60.0 / 1'000'000'000.0 * frameSpeed);
        if (!(correctedFrame > 0.0f && correctedFrame < 216'000.0f))
            continue;

        const size_t robCharOffset = kPvPlaydata + robId * kPvPlaydataStride + kPvPlaydataRobChara;
        void* robChar = Rd<void*>(g.pv, robCharOffset);
        if (!robChar || !Readable(robChar, 1) ||
            Rd<int8_t>((const uint8_t*)robChar, 0) != (int8_t)robId)
            continue;

        requestFrame(robChar, correctedFrame);
        Wr<float>(g.pv, record + kPvSetMotionFrame, correctedFrame);
        ++corrected;
    }

    if (corrected)
    {
        g.replayCorrected += corrected;
        Log("seek catch-up corrected %d zero-frame SET_MOTION command(s) at target %.3f s",
            corrected, (double)targetTime * 1e-9);
    }
}
static bool __fastcall HookTick(void* pvGame)
{
    bool r = g.origTick(pvGame);
    if (!g.disabled && g.replaying)
    {
        ++g.replayTicks;
        if (g.pv && g.game == (uint8_t*)pvGame)
        {
            FixReplayedSetMotionFrames();
            const int64_t currentTime = Rd<int64_t>(g.pv, Game::kPvTime);
            if (g.replayTargetNs <= 0 || currentTime >= g.replayTargetNs - 50'000'000LL)
            {
                Log("seek catch-up finished: target=%.3f s current=%.3f s frames=%d active=0x%02X zero=0x%02X corrected=%d",
                    (double)g.replayTargetNs * 1e-9, (double)currentTime * 1e-9,
                    g.replayTicks, g.replayActiveMask, g.replayZeroMask, g.replayCorrected);
                g.replaying = false;
            }
        }
        if (g.replaying && g.replayTicks >= 120)
        {
            Log("seek catch-up timed out: target=%.3f s frames=%d active=0x%02X zero=0x%02X corrected=%d",
                (double)g.replayTargetNs * 1e-9, g.replayTicks,
                g.replayActiveMask, g.replayZeroMask, g.replayCorrected);
            g.replaying = false;
        }
    }
    if (!g.disabled)
    {
        PollHotkeys();
        OnPvFrame((uint8_t*)pvGame);
    }
    return r;
}

// pv_game_music::load is called by restart(). While we seek, load the song at the target time.
static int32_t __fastcall HookMusicLoad(void* music, int32_t type, void* path, bool waitLoad,
                                        float time, bool a6)
{
    // Only touch the call if it really looks like load(): our music object, type 4, a start time
    // that is a small normal number. Anything else (a wrongly guessed function) passes unchanged.
    bool sameMusic = g.pv && music == Rd<void*>(g.pv, Game::kPvMusic);
    if (g.inRestart && type == 4 && !sameMusic)
    {
        static bool once = false;
        if (!once) { once = true; Log("music: load() got music object %p but pv_data says %p%s", music,
                                      g.pv ? Rd<void*>(g.pv, Game::kPvMusic) : nullptr, g.loadConfirmed ? " (trusting the fixed MusicLoad address)" : ""); }
    }
    if (g.inRestart && type == 4 && g.pv && (sameMusic || g.loadConfirmed) && time >= 0.0f && time < 100000.0f)
    {
        Log("music: load() start %.3f s -> %.3f s", time, g.pendingStart);
        g.musicObj = music;
        time = g.pendingStart;
        waitLoad = true;
    }
    return g.origMusicLoad(music, type, path, waitLoad, time, a6);
}

// pv_game_music::play is what the script's MUSIC_PLAY command calls. While we replay the
// script after a seek, start the audio at (target time - time of the MUSIC_PLAY command).
static int32_t __fastcall HookMusicPlay(void* music, int32_t type, void* path, bool playOnEnd,
                                        float start, float end, float fadeIn, float fadeOut, bool a9)
{
    if (g.replaying && IsPvViewerMode() && g.pv && !g.origMusicLoad)   // with MusicLoad the song is already placed
    {
        long long cur = Rd<int64_t>(g.pv, Game::kPvTime);
        long long at  = Rd<int64_t>(g.pv, Game::kPvDscTime);
        if (cur > at)
        {
            float shift = (float)((cur - at) * 1e-9);
            Log("music: start %.3f s -> %.3f s", start, start + shift);
            start += shift;
        }
    }
    return g.origMusicPlay(music, type, path, playOnEnd, start, end, fadeIn, fadeOut, a9);
}

// ---------------------------------------------------------------------------------------
// Small control window (exact time entry + slider)
// ---------------------------------------------------------------------------------------
enum { ID_EDIT = 101, ID_GO, ID_M30, ID_M5, ID_P5, ID_P30, ID_SLIDER, ID_LABEL, TIMER_UI = 1 };

static bool ParseTime(const char* s, double* out)
{
    // accepts  83.5   1:23.5   1:23.456   0:01:23.5
    double parts[3] = { 0, 0, 0 };
    int n = 0;
    const char* p = s;
    while (*p == ' ') ++p;
    if (!*p) return false;
    while (*p && n < 3)
    {
        char* e;
        double v = strtod(p, &e);
        if (e == p || v < 0) return false;
        parts[n++] = v;
        p = e;
        while (*p == ' ') ++p;
        if (*p == ':') { ++p; if (!*p) return false; } // a colon must be followed by more digits
        else break;
    }
    if (*p) return false;
    double sec = 0;
    for (int i = 0; i < n; ++i) sec = sec * 60.0 + parts[i];
    *out = sec;
    return true;
}

static void FormatTime(long long ns, char* out, size_t cap)
{
    if (ns < 0) { snprintf(out, cap, "--:--.---"); return; }
    long long ms = ns / 1'000'000LL;
    snprintf(out, cap, "%02lld:%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    static HWND label, edit, slider;
    switch (m)
    {
    case WM_CREATE:
    {
        HINSTANCE hi = g.self;
        label = CreateWindowExW(0, L"STATIC", L"--:--.---", WS_CHILD | WS_VISIBLE, 10, 8, 320, 20, h, (HMENU)ID_LABEL, hi, nullptr);
        slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_NOTICKS, 5, 30, 330, 28, h, (HMENU)ID_SLIDER, hi, nullptr);
        edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0:00.000", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 64, 120, 24, h, (HMENU)ID_EDIT, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Go", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 136, 63, 50, 26, h, (HMENU)ID_GO, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"-30s", WS_CHILD | WS_VISIBLE, 10, 98, 50, 26, h, (HMENU)ID_M30, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"-5s", WS_CHILD | WS_VISIBLE, 64, 98, 50, 26, h, (HMENU)ID_M5, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"+5s", WS_CHILD | WS_VISIBLE, 118, 98, 50, 26, h, (HMENU)ID_P5, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"+30s", WS_CHILD | WS_VISIBLE, 172, 98, 50, 26, h, (HMENU)ID_P30, hi, nullptr);
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
        SetTimer(h, TIMER_UI, 100, nullptr);
        return 0;
    }
    case WM_TIMER:
    {
        char a[32], b[32], text[96];
        FormatTime(g.curNs, a, sizeof(a));
        FormatTime(g.durNs, b, sizeof(b));
        snprintf(text, sizeof(text), g.active ? "%s / %s" : "%s / %s   (no PV running)", a, b);
        SetWindowTextA(label, text);
        long long dur = g.durNs;
        if (dur > 0 && GetCapture() != slider && GetFocus() != slider)
            SendMessageW(slider, TBM_SETPOS, TRUE, (LPARAM)(g.curNs.load() * 1000 / dur));
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w))
        {
        case ID_GO:
        {
            char buf[64];
            GetWindowTextA(edit, buf, sizeof(buf));
            double sec;
            if (ParseTime(buf, &sec)) g.reqNs = (long long)(sec * 1e9);
            else Log("could not read time \"%s\"", buf);
            return 0;
        }
        case ID_M30: RequestRelative(-30); return 0;
        case ID_M5:  RequestRelative(-5);  return 0;
        case ID_P5:  RequestRelative(5);   return 0;
        case ID_P30: RequestRelative(30);  return 0;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)l == slider && (LOWORD(w) == TB_ENDTRACK) && g.durNs > 0)
        {
            long long pos = SendMessageW(slider, TBM_GETPOS, 0, 0);
            g.reqNs = g.durNs.load() * pos / 1000;
        }
        return 0;
    case WM_CLOSE:
        g.windowWanted = false;
        ShowWindow(h, SW_HIDE);                  // F8 brings it back in PV Viewer
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI WindowThread(LPVOID)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g.self;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"PvSeekWindow";
    RegisterClassExW(&wc);

    g.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"PV Seek",
        WS_CAPTION | WS_SYSMENU, 40, 40, 358, 170, nullptr, nullptr, g.self, nullptr);
    ShowWindow(g.hwnd, SW_HIDE);                 // shown by the PV frame hook only in PV Viewer

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ---------------------------------------------------------------------------------------
// DML entry points
// ---------------------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) g.self = inst; // nothing else here, as DML recommends
    return TRUE;
}

extern "C" __declspec(dllexport) void Init()
{
    InitPaths();
    Log("---- PvSeek starting ----");
    LoadSettings();
    g.windowWanted = g.cfg.showWindow;

    g.slide = (intptr_t)GetModuleHandleW(nullptr) - (intptr_t)Game::kImageBase;
    if (g.slide != 0) Log("image base moved, slide = 0x%llX", (unsigned long long)g.slide);

    // Diagnostics: these lines are what you need if an address turns out to be wrong.
    uint8_t* tick = (uint8_t*)Real(g.cfg.tickAddr);
    DumpBytes("TickAddress", (uintptr_t)tick);
    DumpBytes("AnchorA", Real(g.cfg.anchorA));
    DumpBytes("AnchorB", Real(g.cfg.anchorB));

    if (!LooksLikeFunctionStart(tick))
    {
        Log("The fixed tick address does not look like the start of a function, so this exe most likely does not");
        Log("match the compiled-in addresses. NOT hooking anything. Check the constants in src/PvSeek.cpp.");
        return;
    }

    // Compiled-in addresses are exe addresses (as Ghidra/IDA show them); convert to real ones.
    // FindRestart() already returns a real address.
    uintptr_t restartReal = g.cfg.restartAddr ? Real(g.cfg.restartAddr) : FindRestart();
    if (restartReal && !LooksLikeFunctionStart((const uint8_t*)restartReal))
    {
        Log("restart address 0x%llX does not look like a function start - ignoring it.", (unsigned long long)restartReal);
        restartReal = 0;
    }
    g.cfg.restartAddr = restartReal; // from here on this is the real address
    uintptr_t musicReal = g.cfg.musicPlayAddr ? Real(g.cfg.musicPlayAddr) : 0;
    if (musicReal && !LooksLikeFunctionStart((const uint8_t*)musicReal))
    {
        Log("pv_game_music::play address 0x%llX does not look like a function start - ignoring it.", (unsigned long long)musicReal);
        musicReal = 0;
    }
    g.cfg.musicPlayAddr = musicReal;
    uintptr_t loadReal = g.cfg.musicLoadAddr ? Real(g.cfg.musicLoadAddr) : FindMusicLoad(restartReal);
    if (loadReal && !LooksLikeFunctionStart((const uint8_t*)loadReal))
    {
        Log("pv_game_music::load address 0x%llX does not look like a function start - ignoring it.", (unsigned long long)loadReal);
        loadReal = 0;
    }
    if (loadReal) Log("pv_game_music::load = 0x%llX", (unsigned long long)loadReal);
    else Log("pv_game_music::load unavailable: check kMusicLoadFn in src/PvSeek.cpp (audio will NOT follow the seek)");
    if (g.cfg.restartAddr)
    {
        g.restart = (RestartFn)g.cfg.restartAddr;
        Log("pv_game::restart = 0x%llX", (unsigned long long)g.cfg.restartAddr);
    }
    else Log("pv_game::restart unavailable: check kRestartFn in src/PvSeek.cpp");

    g.origTick = (TickFn)tick;
    if (loadReal) { g.origMusicLoad = (MusicLoadFn)loadReal; g.loadConfirmed = g.cfg.musicLoadAddr != 0; }
    if (g.cfg.musicPlayAddr) g.origMusicPlay = (MusicPlayFn)g.cfg.musicPlayAddr;
    else Log("pv_game_music::play unavailable: check kMusicPlayFn in src/PvSeek.cpp (not needed when MusicLoad works)");

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g.origTick, (PVOID)HookTick);
    if (g.origMusicPlay) DetourAttach(&(PVOID&)g.origMusicPlay, (PVOID)HookMusicPlay);
    if (g.origMusicLoad) DetourAttach(&(PVOID&)g.origMusicLoad, (PVOID)HookMusicLoad);
    LONG err = DetourTransactionCommit();
    Log("hooks installed: %s (detours code %ld)", err == NO_ERROR ? "yes" : "NO", err);

    CreateThread(nullptr, 0, WindowThread, nullptr, 0, nullptr);
}
