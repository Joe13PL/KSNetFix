// KSNetFix - KnightShift multiplayer synchronisation fix.
//
// Loaded as a dinput8.dll proxy placed next to KnightShift.ex1/.ex2. On attach it
// patches the running game image (both engine builds are supported) and then
// forwards DirectInput8Create to the real system dinput8.dll.
//
// Everything here is wire-compatible with unpatched peers and keeps the engine's
// deterministic contract: every machine still runs exactly turnLen simulation
// ticks between two turn markers - we only change *when* (in wall-clock time)
// those ticks happen, and we keep rendering from touching simulation RNG state.
//
// See docs/NETCODE.md for the reverse-engineered protocol this relies on.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "patch.h"
#include "steampeer.h"
#include "earthnet.h"
#ifdef KSNETFIX_SERVER
#include "server.h" // dedicated server add-on (separate repository)
#endif

#pragma comment(lib, "winmm.lib")

#define KSNETFIX_VERSION "2.5"

// ---------------------------------------------------------------------------
// Per-build addresses (all verified by signature before use)
// ---------------------------------------------------------------------------
struct GameAddrs {
    const char* name;
    // code
    uint32_t timerFn;        // rdtsc; ret                     - engine clock
    uint32_t calibFn;        // TSC calibration (sets tscPerMs)
    uint32_t pumpCallSite;   // call NetPump inside the simulation loop
    uint32_t pumpFn;         // NetPump (receive / exec turns / flush)
    uint32_t ackSite;        // mov esi,[maxTurnsAhead]; shr esi,1; inc esi
    uint32_t traceFn;        // compiled-out debug printf (just 'ret')
    uint32_t gateSite;       // loop gate: mov edx,[turnLen]; mov eax,[turnBase]; mov ecx,[realTick]
    uint32_t tickStartSite;  // first call of every simulation tick (semaphore held)
    uint32_t tickStartFn;
    uint32_t relSemSite;     // call [ReleaseSemaphore] ending a batch of ticks
    uint32_t relSemIat;
    uint32_t packetFn;       // CInNet::OnPacket (thiscall, 5 args)
    uint32_t mutexJeSite;    // je <exit> after CreateMutex("Earth 2150") == ERROR_ALREADY_EXISTS
    uint32_t keyConvSite;    // call KeyToChar(dik, down, prev) in the DirectInput keyboard event loop
    uint32_t keyConvFn;      // stock: synthesises WM_KEYDOWN and fishes WM_CHAR out of the queue
    uint32_t keyVkFn;        // DIK -> VK (engine table + MapVirtualKeyA)
    uint32_t diKeyState;     // BYTE[256] DirectInput keyboard state (DIK indexed)
    uint32_t idSendSite;     // mov eax,[installId.hi] while building message 0x60010 (join request)
    uint32_t installIdHi;    // high dword of the 64-bit identity derived from the serial key
    // data
    uint32_t tscPerMs;       // clock units per millisecond
    uint32_t gameMode;       // 2 = network game
    uint32_t realTick;       // 30 Hz wall-clock tick counter (always advances)
    uint32_t tickPeriod;     // u64, clock units per tick
    uint32_t nextTickTime;   // u64, scheduled time of the next tick
    uint32_t netFlags;       // CNetwork state bits
    uint32_t turnLen;        // sim ticks per turn (5 @ 30 Hz)
    uint32_t turnBase;       // realTick when the current turn started executing
    uint32_t turnsExecuted;  // turn markers executed locally
    uint32_t turnsGenerated; // host: turn markers generated
    uint32_t maxTurnsAhead;  // host: max turns generated beyond slowest peer (3)
    uint32_t maxTurnSpread;  // host: max executed-turn spread between peers (4)
    uint32_t paused;
    uint32_t syncHash;       // dword[3] of every game packet header (always 0xFFFFFFFF in stock game)
    uint32_t serverIn, clientIn, serverOut, clientOut; // CInNet / COutNet objects
    uint32_t dpPeer;         // IDirectPlay8Peer*
    uint32_t hostDpnid;      // DPNID of the session host (0xF6226C is the local player)
    uint32_t playerTable;    // 8 x 24 bytes: +0 DPNID, +4 flags (0x80 = connected)
    uint32_t rngBase;        // 8 LCG streams (x = x*0x343FD + 0x269EC3)
};

static const GameAddrs kEx1 = {
    "KnightShift.ex1 (D3D8 classic)",
    0x00796E70, 0x00796E80, 0x00406400, 0x007EEA10, 0x007F9B04, 0x0041D080, 0x00406438,
    0x004063F3, 0x00796910, 0x00406566, 0x008DB278, 0x007FA690, 0x004079E3,
    0x007D9A69, 0x007D9FD0, 0x007D9F00, 0x00F61C48, 0x0084978B, 0x00937914,
    0x00F5C9B8, 0x009132C8, 0x009372B0, 0x00937290, 0x00937298, 0x00F6238C,
    0x00F62530, 0x00F62534, 0x00F62538, 0x00F62518, 0x00F62510, 0x00F6251C, 0x00F62500, 0x00F624FC,
    0x00F62428, 0x00F6242C, 0x00F62430, 0x00F62434, 0x00F62198, 0x00F62270, 0x00F62290,
    0x00928670,
};

static const GameAddrs kEx2 = {
    "KnightShift.ex2 (D3D8 shaders)",
    0x00799B30, 0x00799B40, 0x004063E0, 0x007F1630, 0x007FC784, 0x0052E7F0, 0x00406418,
    0x004063D3, 0x007995D0, 0x00406546, 0x008E228C, 0x007FD310, 0x004079C3,
    0x007DC629, 0x007DCB90, 0x007DCAC0, 0x0152D188, 0x0084C51B, 0x00943B54,
    0x01527EFC, 0x0091C408, 0x009434F0, 0x009434D0, 0x009434D8, 0x0152D8CC,
    0x0152DA70, 0x0152DA74, 0x0152DA78, 0x0152DA58, 0x0152DA50, 0x0152DA5C, 0x0152DA40, 0x0152DA3C,
    0x0152D968, 0x0152D96C, 0x0152D970, 0x0152D974, 0x0152D6D8, 0x0152D7B0, 0x0152D7D0,
    0x00931DF8,
};

static const GameAddrs* A = nullptr;

static const uint32_t kRngOffsets[8] = {0x00, 0x04, 0x08, 0x0C, 0x10, 0x18, 0x1C, 0x20};

// CNetwork::m_dwFlags bits we care about
enum : uint32_t {
    NF_HOST        = 0x00000004,
    NF_GAME_ACTIVE = 0x40000000,
};

// CInNet layout (same in both builds)
enum : uint32_t {
    IN_FLAGS   = 0x10,   // 1 = executes turns (client role), 2 = relays (server role), 0x20 = archive
    IN_RING    = 0x68,   // CNetMessage* [0x401]
    IN_READ    = 0x106C,
    IN_WRITE   = 0x1070,
    IN_RINGLEN = 0x401,
    MSG_SIZE   = 0x08,
    MSG_DATA   = 0x0C,
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct Config {
    int enabled = 1;
    int preciseClock = 1;
    int timerResolution = 1;
    int catchUp = 1;
    int catchUpTarget = 0;          // turn markers allowed to wait in the queue (0 = execute on arrival, like the host)
    int catchUpOnHost = 0;
    double catchUpGain = 0.5;       // extra speed per excess waiting turn (0.5 -> 1.5x at one waiting turn)
    double catchUpMaxSpeed = 3.0;   // wall-clock speed cap while catching up
    int catchUpSmooth = 1;          // pace evenly spaced ticks by the average queue depth instead of extra ticks
    double smoothTarget = 0.25;     // average turn markers waiting in the queue (0.25 = ~1 tick of slack per turn)
    double smoothMinSpeed = 0.95;
    double smoothMaxSpeed = 1.25;
    int desyncStreamMask = 0x3F;    // gameplay RNG streams; 6/7 (0x68C/0x690) drive visual effects and differ per camera
    int ackEveryTurns = 1;          // original engine: (maxTurnsAhead/2)+1 = 2
    int maxTurnsAhead = 5;          // host, original 3
    int maxTurnSpread = 6;          // host, original 4
    int rngIsolation = 1;
    int rngMonitor = 1;
    int desyncCheck = 1;
    int allowMultipleInstances = 0;
    int keyboardFix = 1;
    int log = 1;
    int logInterval = 10;           // seconds
    int netTrace = 0;               // revive the engine's own net debug trace
    SteamSettings steam;
    SteamNetSettings steamNet;
    bool server = false;            // [Server] Enabled (dedicated server builds only)
};
static Config cfg;

static char g_dir[MAX_PATH];

static void LoadConfig() {
    char ini[MAX_PATH];
    _snprintf(ini, sizeof(ini), "%sksnetfix.ini", g_dir);
    auto I = [&](const char* k, int def) {
        char buf[64];
        GetPrivateProfileStringA("KSNetFix", k, "", buf, sizeof(buf), ini);
        if (!buf[0]) return def;
        char* end = nullptr;
        long v = strtol(buf, &end, 0); // decimal or 0x hex
        return end == buf ? def : (int)v;
    };
    auto D = [&](const char* k, double def) {
        char buf[64], d[64];
        _snprintf(d, sizeof(d), "%g", def);
        GetPrivateProfileStringA("KSNetFix", k, d, buf, sizeof(buf), ini);
        return atof(buf);
    };
    cfg.enabled                = I("Enabled", cfg.enabled);
    cfg.preciseClock           = I("PreciseClock", cfg.preciseClock);
    cfg.timerResolution        = I("TimerResolution", cfg.timerResolution);
    cfg.catchUp                = I("CatchUp", cfg.catchUp);
    cfg.catchUpTarget          = I("CatchUpTarget", cfg.catchUpTarget);
    cfg.catchUpOnHost          = I("CatchUpOnHost", cfg.catchUpOnHost);
    cfg.catchUpGain            = D("CatchUpGain", cfg.catchUpGain);
    cfg.catchUpMaxSpeed        = D("CatchUpMaxSpeed", cfg.catchUpMaxSpeed);
    cfg.catchUpSmooth          = I("CatchUpSmooth", cfg.catchUpSmooth);
    cfg.smoothTarget           = D("SmoothTarget", cfg.smoothTarget);
    cfg.smoothMinSpeed         = D("SmoothMinSpeed", cfg.smoothMinSpeed);
    cfg.smoothMaxSpeed         = D("SmoothMaxSpeed", cfg.smoothMaxSpeed);
    cfg.desyncStreamMask       = I("DesyncStreamMask", cfg.desyncStreamMask);
    cfg.ackEveryTurns          = I("AckEveryTurns", cfg.ackEveryTurns);
    cfg.maxTurnsAhead          = I("MaxTurnsAhead", cfg.maxTurnsAhead);
    cfg.maxTurnSpread          = I("MaxTurnSpread", cfg.maxTurnSpread);
    cfg.rngIsolation           = I("RngIsolation", cfg.rngIsolation);
    cfg.rngMonitor             = I("RngMonitor", cfg.rngMonitor);
    cfg.desyncCheck            = I("DesyncCheck", cfg.desyncCheck);
    cfg.allowMultipleInstances = I("AllowMultipleInstances", cfg.allowMultipleInstances);
    cfg.keyboardFix            = I("KeyboardFix", cfg.keyboardFix);
    cfg.log                    = I("Log", cfg.log);
    cfg.logInterval            = I("LogInterval", cfg.logInterval);
    cfg.netTrace               = I("NetTrace", cfg.netTrace);
    cfg.steam.enabled          = GetPrivateProfileIntA("Steam", "Enabled", 1, ini) != 0;
    cfg.steam.appId            = GetPrivateProfileIntA("Steam", "AppId", 254060, ini);
    char lt[32];
    GetPrivateProfileStringA("Steam", "LobbyType", "public", lt, sizeof(lt), ini);
    cfg.steam.lobbyFriendsOnly = _stricmp(lt, "friends") == 0;
    cfg.steam.verbose          = GetPrivateProfileIntA("Steam", "Verbose", 0, ini) != 0;
    cfg.steam.gameServer       = GetPrivateProfileIntA("Steam", "GameServer", 0, ini) != 0;
    GetPrivateProfileStringA("Server", "SessionName", cfg.steam.serverName, cfg.steam.serverName,
                             (DWORD)sizeof(cfg.steam.serverName), ini);
    SteamNet_LoadConfig(ini, cfg.steamNet);
#ifdef KSNETFIX_SERVER
    cfg.server = Server_LoadConfig(ini);
#endif

    if (cfg.catchUpTarget < 0) cfg.catchUpTarget = 0;
    if (cfg.catchUpMaxSpeed < 1.0) cfg.catchUpMaxSpeed = 1.0;
    if (cfg.catchUpMaxSpeed > 8.0) cfg.catchUpMaxSpeed = 8.0;
    if (cfg.catchUpGain < 0.0) cfg.catchUpGain = 0.0;
    if (cfg.smoothTarget < 0.0) cfg.smoothTarget = 0.0;
    if (cfg.smoothTarget > 2.0) cfg.smoothTarget = 2.0;
    if (cfg.smoothMinSpeed < 0.8) cfg.smoothMinSpeed = 0.8;
    if (cfg.smoothMinSpeed > 1.0) cfg.smoothMinSpeed = 1.0;
    if (cfg.smoothMaxSpeed < 1.0) cfg.smoothMaxSpeed = 1.0;
    if (cfg.smoothMaxSpeed > 1.5) cfg.smoothMaxSpeed = 1.5;
    if (cfg.ackEveryTurns < 1) cfg.ackEveryTurns = 1;
    if (cfg.ackEveryTurns > 16) cfg.ackEveryTurns = 16;
    if (cfg.maxTurnsAhead < 0 || cfg.maxTurnsAhead > 12) cfg.maxTurnsAhead = 5;
    if (cfg.maxTurnSpread < 0 || cfg.maxTurnSpread > 16) cfg.maxTurnSpread = 6;
    if (cfg.logInterval < 1) cfg.logInterval = 1;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_logLock;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_trace = INVALID_HANDLE_VALUE;

static HANDLE OpenLog(const char* file) {
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s%s", g_dir, file);
    return CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static void WriteLine(HANDLE h, const char* text) {
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1200];
    int n = _snprintf(line, sizeof(line) - 2, "[%02d:%02d:%02d.%03d] %s", st.wHour, st.wMinute, st.wSecond,
                      st.wMilliseconds, text);
    if (n < 0) n = (int)sizeof(line) - 3;
    if (n == 0 || line[n - 1] != '\n') line[n++] = '\n';
    DWORD w;
    EnterCriticalSection(&g_logLock);
    WriteFile(h, line, (DWORD)n, &w, nullptr);
    LeaveCriticalSection(&g_logLock);
}

void Log(const char* fmt, ...) {
    if (!cfg.log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    WriteLine(g_log, buf);
}

// ---------------------------------------------------------------------------
// Code patching helpers
// ---------------------------------------------------------------------------
bool Match(uint32_t va, const uint8_t* bytes, size_t n) {
    __try {
        return memcmp(reinterpret_cast<void*>(static_cast<uintptr_t>(va)), bytes, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteCode(uint32_t va, const void* bytes, size_t n) {
    void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(va));
    DWORD old;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}

bool WriteRel32(uint32_t site, uint8_t opcode, const void* target, size_t pad) {
    uint8_t b[16];
    b[0] = opcode;
    int32_t rel = (int32_t)((uintptr_t)target - (site + 5));
    memcpy(b + 1, &rel, 4);
    for (size_t i = 0; i < pad; i++) b[5 + i] = 0x90;
    return WriteCode(site, b, 5 + pad);
}

static void Le32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

bool MatchCall(uint32_t site, uint32_t target) {
    uint8_t call[5] = {0xE8};
    Le32(call + 1, target - (site + 5));
    return Match(site, call, sizeof(call));
}

static bool VerifyCore(const GameAddrs& a) {
    static const uint8_t timer[] = {0x0F, 0x31, 0xC3};
    static const uint8_t calib[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x4C, 0x53, 0x56, 0x57};
    static const uint8_t trace[] = {0xC3, 0x90, 0x90, 0x90};
    uint8_t ack[9] = {0x8B, 0x35, 0, 0, 0, 0, 0xD1, 0xEE, 0x46};
    Le32(ack + 2, a.maxTurnsAhead);
    uint8_t gate[17] = {0x8B, 0x15, 0, 0, 0, 0, 0xA1, 0, 0, 0, 0, 0x8B, 0x0D, 0, 0, 0, 0};
    Le32(gate + 2, a.turnLen);
    Le32(gate + 7, a.turnBase);
    Le32(gate + 13, a.realTick);
    return Match(a.timerFn, timer, sizeof(timer)) && Match(a.calibFn, calib, sizeof(calib)) &&
           Match(a.traceFn, trace, sizeof(trace)) && Match(a.ackSite, ack, sizeof(ack)) &&
           Match(a.gateSite, gate, sizeof(gate)) && MatchCall(a.pumpCallSite, a.pumpFn);
}

static bool VerifyTickHooks(const GameAddrs& a) {
    uint8_t rel[6] = {0xFF, 0x15};
    Le32(rel + 2, a.relSemIat);
    return MatchCall(a.tickStartSite, a.tickStartFn) && Match(a.relSemSite, rel, sizeof(rel));
}

static bool VerifyPacketHook(const GameAddrs& a) {
    static const uint8_t pro[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x68, 0x8A, 0x41, 0x10};
    return Match(a.packetFn, pro, sizeof(pro));
}

// ---------------------------------------------------------------------------
// 1. Precise engine clock
//
// The engine times everything with raw RDTSC scaled by a TSC-per-ms value that
// it calibrates over 0.1 ms QPC windows and biases low (average of the 5
// smallest of 101 samples). Machines therefore disagree on how long a tick is
// by ~0.05-0.2 %, and the host-paced lockstep degrades to the slowest clock.
// It also computes (tscPerMs * 1000) in 32 bits, which overflows on CPUs with
// a TSC above 4.29 GHz. We replace the clock with QPC-derived nanoseconds and a
// fixed 1,000,000 units/ms, so every machine runs identical integer math.
// ---------------------------------------------------------------------------
static uint64_t g_qpf;
static uint32_t g_qpcMul; // fast path when 1e9 % qpf == 0

static uint64_t __cdecl ClockNs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint64_t q = (uint64_t)c.QuadPart;
    if (g_qpcMul) return q * g_qpcMul;
    return (q / g_qpf) * 1000000000ull + ((q % g_qpf) * 1000000000ull) / g_qpf;
}

// Engine callers treat the clock like rdtsc: result in EDX:EAX. Preserve ECX
// as well to be strictly equivalent to the instruction we replace.
static __declspec(naked) void ClockThunk() {
    __asm {
        push ecx
        call ClockNs
        pop ecx
        ret
    }
}

static bool InstallPreciseClock() {
    LARGE_INTEGER f;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return false;
    g_qpf = (uint64_t)f.QuadPart;
    g_qpcMul = (1000000000ull % g_qpf == 0) ? (uint32_t)(1000000000ull / g_qpf) : 0;

    G<uint32_t>(A->tscPerMs) = 1000000;
    // Neutralise calibration: mov dword [tscPerMs], 1000000 / mov eax, 1 / ret
    uint8_t calib[16] = {0xC7, 0x05, 0, 0, 0, 0, 0x40, 0x42, 0x0F, 0x00, 0xB8, 1, 0, 0, 0, 0xC3};
    Le32(calib + 2, A->tscPerMs);
    if (!WriteCode(A->calibFn, calib, sizeof(calib))) return false;
    return WriteRel32(A->timerFn, 0xE9, (void*)&ClockThunk);
}

// ---------------------------------------------------------------------------
// 2. Ack cadence
//
// A client that has nothing else to send reports progress with a 1-byte 0xFE
// message only every (maxTurnsAhead/2)+1 = 2 executed turns. The host, however,
// refuses to generate a new turn once it has not heard from a peer for
// maxTurnsAhead (3) turns, leaving only ~1 turn (167 ms) of jitter tolerance.
// ---------------------------------------------------------------------------
static bool InstallAckCadence() {
    uint8_t b[9] = {0xBE, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90}; // mov esi, N ; nop x4
    Le32(b + 1, (uint32_t)cfg.ackEveryTurns);
    return WriteCode(A->ackSite, b, sizeof(b));
}

// ---------------------------------------------------------------------------
// 3. Engine net trace (optional)
// ---------------------------------------------------------------------------
static uint32_t g_imgLo, g_imgHi;

static bool FormatLooksValid(const char* fmt) {
    uint32_t p = (uint32_t)(uintptr_t)fmt;
    if (p < g_imgLo || p >= g_imgHi) return false;
    __try {
        for (int i = 0; i < 512; i++) {
            unsigned char c = (unsigned char)fmt[i];
            if (c == 0) return i > 0;
            if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

static int SafeFormat(char* buf, size_t n, const char* fmt, va_list ap) {
    __try {
        return _vsnprintf(buf, n, fmt, ap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static void __cdecl TraceHook(const char* fmt, ...) {
    if (g_trace == INVALID_HANDLE_VALUE || !FormatLooksValid(fmt)) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = SafeFormat(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[sizeof(buf) - 1] = 0;
    char line[1100];
    _snprintf(line, sizeof(line) - 1, "t=%u %s", G<uint32_t>(A->realTick), buf);
    line[sizeof(line) - 1] = 0;
    WriteLine(g_trace, line);
}

// ---------------------------------------------------------------------------
// 4. RNG monitor + isolation
//
// The simulation thread and the render thread are serialised by a semaphore:
// a batch of simulation ticks runs while it is held, the renderer runs while
// it is free. Anything the renderer draws from a simulation RNG stream is
// consumed a frame-rate dependent number of times, which differs between PCs
// and silently desynchronises the lockstep. We watch every stream across the
// boundaries and, in network games, give the renderer its own continuation of
// each stream so the simulation always resumes from its own state.
// ---------------------------------------------------------------------------
// x' = a*x + c mod 2^32 with a = 1 mod 4 and c odd has full period, so the number
// of steps between two states is unique and can be found bit by bit: the
// sequence taken mod 2^(j+1) has period 2^(j+1), hence once the states agree
// mod 2^j the remaining distance is either 0 or 2^j steps.
static const uint32_t kLcgA = 0x343FDu, kLcgC = 0x269EC3u;
static uint32_t g_jumpA[32], g_jumpC[32]; // F^(2^j)(x) = jumpA[j]*x + jumpC[j]

static void LcgInitJumps() {
    uint32_t a = kLcgA, c = kLcgC;
    for (int j = 0; j < 32; j++) {
        g_jumpA[j] = a;
        g_jumpC[j] = c;
        c = a * c + c;
        a = a * a;
    }
}

static uint32_t LcgDistance(uint32_t from, uint32_t to) {
    uint32_t k = 0, x = from;
    for (int j = 0; j < 32; j++) {
        uint32_t m = (j == 31) ? 0xFFFFFFFFu : ((1u << (j + 1)) - 1);
        if ((x & m) != (to & m)) {
            x = g_jumpA[j] * x + g_jumpC[j];
            k |= 1u << j;
        }
    }
    return k; // x == to here
}

// Render-side consumption between two ticks is at most a few hundred thousand
// draws; a reseed lands at a uniformly random distance (P(< 2^20) ~ 0.02%).
static bool LcgReachable(uint32_t from, uint32_t to, uint32_t maxSteps, uint32_t* steps) {
    uint32_t k = LcgDistance(from, to);
    *steps = k;
    return k <= maxSteps;
}

static struct RngState {
    uint32_t* addr[8];
    uint32_t sim[8], render[8], atStart[8], atEnd[8];
    bool inside, haveEnd, swapped, renderInit;
    // statistics
    uint32_t tickUse[8], outsideUse[8], outsideReseed[8];
    uint64_t outsideSteps[8];
    uint32_t batches;
    bool warnedShared[8];
} R;

static bool IsolationActive() {
    return cfg.rngIsolation && G<int>(A->gameMode) == 2 && (G<uint32_t>(A->netFlags) & NF_GAME_ACTIVE) &&
           G<uint32_t>(A->turnsExecuted) >= 2;
}

static void __cdecl OnTickStart() {
    if (R.inside) return;
    R.inside = true;
    // Outside network games the command pump runs on the render thread, so
    // the inside/outside classification is only meaningful in mode 2.
    bool net = G<int>(A->gameMode) == 2;
    for (int i = 0; i < 8; i++) {
        uint32_t cur = *R.addr[i];
        bool reseed = false;
        if (net && R.haveEnd && cur != R.atEnd[i]) {
            uint32_t k = 0;
            if (LcgReachable(R.atEnd[i], cur, 1u << 20, &k)) {
                R.outsideUse[i]++;
                R.outsideSteps[i] += k;
            } else {
                R.outsideReseed[i]++;
                reseed = true;
            }
        }
        if (R.swapped) {
            if (reseed) R.sim[i] = cur; // a genuine reseed outside the tick: both sequences restart from it
            R.render[i] = cur;
            *R.addr[i] = R.sim[i];
        }
        R.atStart[i] = *R.addr[i];
    }
    R.swapped = false;
}

static void __cdecl OnTickEnd() {
    if (!R.inside) return;
    R.inside = false;
    bool net = G<int>(A->gameMode) == 2;
    if (net) R.batches++;
    bool iso = IsolationActive();
    for (int i = 0; i < 8; i++) {
        uint32_t cur = *R.addr[i];
        if (net && cur != R.atStart[i]) R.tickUse[i]++;
        R.sim[i] = cur;
        if (iso) {
            if (!R.renderInit) R.render[i] = cur;
            *R.addr[i] = R.render[i];
        }
        R.atEnd[i] = *R.addr[i];
    }
    if (iso) R.renderInit = true;
    R.swapped = iso;
    R.haveEnd = true;
}

// Naked thunks jump to the original targets through these globals.
static uint32_t g_tickStartTarget;
static uint32_t g_relSemIatAddr;

static __declspec(naked) void TickStartHook() {
    __asm {
        pushad
        pushfd
        call OnTickStart
        popfd
        popad
        jmp dword ptr [g_tickStartTarget]
    }
}

static __declspec(naked) void RelSemHook() {
    __asm {
        pushad
        pushfd
        call OnTickEnd
        popfd
        popad
        mov eax, g_relSemIatAddr
        jmp dword ptr [eax]         // tail-call ReleaseSemaphore with the caller's args
    }
}

static const char* kStreamNames[8] = {"0x670", "0x674", "0x678", "0x67C", "0x680", "0x688", "0x68C", "0x690"};

static void ReportRng() {
    if (!cfg.rngMonitor) return;
    char line[900];
    size_t len = (size_t)_snprintf(line, sizeof(line), "rng batches=%u:", R.batches);
    for (int i = 0; i < 8; i++) {
        const char* cls = R.tickUse[i] && R.outsideUse[i] >= 3 ? "SHARED"
                          : R.tickUse[i]                   ? "sim"
                          : R.outsideUse[i]                ? "render"
                                                           : "idle";
        int w = _snprintf(line + len, sizeof(line) - len, " [%s %s tick=%u out=%u(avg %.1f) reseed=%u]", kStreamNames[i],
                          cls, R.tickUse[i], R.outsideUse[i],
                          R.outsideUse[i] ? (double)R.outsideSteps[i] / R.outsideUse[i] : 0.0, R.outsideReseed[i]);
        if (w < 0) break;
        len += (size_t)w;
    }
    Log("%s", line);
    for (int i = 0; i < 8; i++) {
        if (R.tickUse[i] && R.outsideUse[i] >= 3 && !R.warnedShared[i]) {
            R.warnedShared[i] = true;
            Log("WARNING: RNG stream %s is consumed by both the simulation and the renderer - frame-rate dependent, "
                "a desync source in the stock game (%s here)",
                kStreamNames[i], cfg.rngIsolation ? "isolated" : "NOT isolated: set RngIsolation=1");
        }
    }
}

// ---------------------------------------------------------------------------
// 5. Desync detection
//
// Every game packet header carries (turnsExecuted, syncHash). The stock game
// always sends 0xFFFFFFFF there (its checksum code was compiled out). After each
// executed turn we publish a hash of the simulation RNG streams: 8-bit turn tag
// + 3 bits per stream. Receivers compare it against their own value for the
// same turn, restricted to streams the local simulation actually uses.
// ---------------------------------------------------------------------------
static struct DesyncState {
    CRITICAL_SECTION cs;
    struct Entry { uint32_t turn, hash; } local[256], remote[8][256];
    bool remoteValid[8][256];
    uint32_t lastTurn;
    bool haveTurn;
    uint32_t checks, mismatches, legacyPackets, raceSkips;
    uint32_t firstBad[8];
    bool bad[8], legacy[8];
} D;

static uint32_t MakeHash(uint32_t turn) {
    uint32_t h = (turn & 0xFF) << 24;
    for (int i = 0; i < 8; i++) {
        uint32_t v = *R.addr[i] * 0x9E3779B1u;
        h |= (v >> 29) << (3 * i);
    }
    return h;
}

static uint8_t CompareMask() {
    uint8_t m = 0;
    for (int i = 0; i < 8; i++)
        if (R.tickUse[i]) m |= (uint8_t)(1 << i);
    return m & (uint8_t)cfg.desyncStreamMask;
}

// D.cs held
static void CompareLocked(int slot, uint32_t turn, uint32_t localH, uint32_t remoteH) {
    uint8_t mask = CompareMask();
    uint8_t diff = 0;
    for (int i = 0; i < 8; i++)
        if ((mask & (1 << i)) && (((localH >> (3 * i)) ^ (remoteH >> (3 * i))) & 7)) diff |= (uint8_t)(1 << i);
    D.checks++;
    if (!diff) return;
    D.mismatches++;
    if (!D.bad[slot]) {
        D.bad[slot] = true;
        D.firstBad[slot] = turn;
        Log("DESYNC DETECTED with player slot %d at turn %u: simulation RNG streams differ (mask 0x%02X, local %08X "
            "remote %08X). The game states have diverged from here on.",
            slot, turn, diff, localH, remoteH);
    }
}

static void DesyncReset() {
    EnterCriticalSection(&D.cs);
    memset(D.local, 0, sizeof(D.local));
    memset(D.remote, 0, sizeof(D.remote));
    memset(D.remoteValid, 0, sizeof(D.remoteValid));
    D.haveTurn = false;
    D.checks = D.mismatches = D.legacyPackets = D.raceSkips = 0;
    memset(D.bad, 0, sizeof(D.bad));
    memset(D.legacy, 0, sizeof(D.legacy));
    LeaveCriticalSection(&D.cs);
}

// simulation thread, inside the tick, after NetPump executed (at most) one turn
static void DesyncOnTurn() {
    uint32_t turn = G<uint32_t>(A->turnsExecuted);
    if (D.haveTurn && turn == D.lastTurn) return;
    uint32_t h = MakeHash(turn);
    EnterCriticalSection(&D.cs);
    D.lastTurn = turn;
    D.haveTurn = true;
    D.local[turn & 0xFF].turn = turn;
    D.local[turn & 0xFF].hash = h;
    G<uint32_t>(A->syncHash) = h;
    for (int s = 0; s < 8; s++) {
        if (D.remoteValid[s][turn & 0xFF] && D.remote[s][turn & 0xFF].turn == turn) {
            CompareLocked(s, turn, h, D.remote[s][turn & 0xFF].hash);
            D.remoteValid[s][turn & 0xFF] = false;
        }
    }
    LeaveCriticalSection(&D.cs);
}

static void __cdecl OnPacket(void* in, int slot, const uint8_t* pkt) {
    (void)in;
    if (!cfg.desyncCheck || slot < 0 || slot > 7 || !pkt) return;
    uint32_t turn, hash;
    __try {
        turn = *(const uint32_t*)(pkt + 8);
        hash = *(const uint32_t*)(pkt + 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    EnterCriticalSection(&D.cs);
    if (hash == 0xFFFFFFFFu) {
        D.legacyPackets++;
        if (!D.legacy[slot]) {
            D.legacy[slot] = true;
            Log("player slot %d does not run KSNetFix (no sync hash) - desync detection unavailable for it", slot);
        }
        LeaveCriticalSection(&D.cs);
        return;
    }
    // The hash carries the low 8 bits of the turn it describes; a packet built
    // between turn execution and our hash update carries the previous turn's
    // hash, so decode the turn from the tag instead of assuming it.
    uint32_t lag = (turn - (hash >> 24)) & 0xFF;
    if (lag > 4) {
        D.raceSkips++;
    } else {
        turn -= lag;
        if (D.haveTurn && turn <= D.lastTurn) {
            const auto& e = D.local[turn & 0xFF];
            if (e.turn == turn && D.lastTurn - turn < 200) CompareLocked(slot, turn, e.hash, hash);
        } else {
            D.remote[slot][turn & 0xFF].turn = turn;
            D.remote[slot][turn & 0xFF].hash = hash;
            D.remoteValid[slot][turn & 0xFF] = true;
        }
    }
    LeaveCriticalSection(&D.cs);
}

static uint32_t g_packetResume;

static __declspec(naked) void PacketHook() {
    // thiscall(ecx=CInNet, dpnid, slot, type, packet, dp)
    __asm {
        pushad
        pushfd
        push dword ptr [esp + 0x24 + 0x10]      // packet
        push dword ptr [esp + 0x24 + 0x08 + 4]  // slot
        push ecx
        call OnPacket
        add esp, 12
        popfd
        popad
        push ebp                                // relocated prologue
        mov ebp, esp
        sub esp, 0x68
        jmp dword ptr [g_packetResume]
    }
}

// ---------------------------------------------------------------------------
// 6. Turn catch-up + session statistics (runs on the simulation thread,
//    right after the engine's NetPump for this tick)
// ---------------------------------------------------------------------------
struct DpnConnectionInfo {
    DWORD dwSize, dwRoundTripLatencyMS, dwThroughputBPS, dwPeakThroughputBPS;
    DWORD dwBytesSentGuaranteed, dwPacketsSentGuaranteed, dwBytesSentNonGuaranteed, dwPacketsSentNonGuaranteed;
    DWORD dwBytesRetried, dwPacketsRetried, dwBytesDropped, dwPacketsDropped;
    DWORD dwMessagesTransmittedHighPriority, dwMessagesTimedOutHighPriority;
    DWORD dwMessagesTransmittedNormalPriority, dwMessagesTimedOutNormalPriority;
    DWORD dwMessagesTransmittedLowPriority, dwMessagesTimedOutLowPriority;
    DWORD dwBytesReceivedGuaranteed, dwPacketsReceivedGuaranteed, dwBytesReceivedNonGuaranteed;
    DWORD dwPacketsReceivedNonGuaranteed, dwMessagesReceived;
};

struct Stats {
    uint32_t ticks, simTicks, stallTicks, pausedTicks, catchUpTicks;
    uint32_t backlogSum, backlogMax;
    double speedSum;
    uint32_t turnsAtStart;
    DWORD startMs;
};

static struct Session {
    bool active = false;
    bool host = false;
    Stats total{}, win{};
    DWORD lastReport = 0;
    double catchUpAcc = 0.0;
    double queueAvg = 0.0;          // smooth catch-up: moving average of the turn queue depth
} S;

typedef uint64_t(__cdecl* EngineClockFn)();
static uint64_t EngineNow() { return ((EngineClockFn)(uintptr_t)A->timerFn)(); }

typedef void(__cdecl* PumpFn)();
static PumpFn g_origPump;

// Smooth catch-up leaves a real backlog (a hitch, a loading pause) to the extra-tick path.
static const int kBurstBacklog = 3;

static int CountTurnMarkers(uint8_t* in) {
    __try {
        uint32_t fl = *(uint32_t*)(in + IN_FLAGS);
        if (!(fl & 1) || (fl & 0x20)) return 0;
        int rd = *(int*)(in + IN_READ), wr = *(int*)(in + IN_WRITE);
        if (rd < 0 || rd >= (int)IN_RINGLEN || wr < 0 || wr >= (int)IN_RINGLEN) return 0;
        int n = (wr - rd + IN_RINGLEN) % IN_RINGLEN, c = 0;
        for (int i = 0; i < n; i++) {
            uint8_t* m = *(uint8_t**)(in + IN_RING + ((rd + i) % IN_RINGLEN) * 4);
            if (!m || *(int*)(m + MSG_SIZE) != 1) continue;
            uint8_t k = **(uint8_t**)(m + MSG_DATA);
            if (k == 0xFF || k == 0xFD || k == 0xFB || k == 0xFA || k == 0xF9) c++;
        }
        return c;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int QueryRtt(uint32_t dpnid) {
    void* dp = G<void*>(A->dpPeer);
    if (!dp || !dpnid) return -1;
    DpnConnectionInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.dwSize = sizeof(ci);
    typedef HRESULT(__stdcall * GetConnInfo)(void*, DWORD, DpnConnectionInfo*, DWORD);
    GetConnInfo fn = (*reinterpret_cast<GetConnInfo**>(dp))[34];
    __try {
        if (fn(dp, dpnid, &ci, 0) < 0) return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return (int)ci.dwRoundTripLatencyMS;
}

static void ReportRtts(char* out, size_t n) {
    out[0] = 0;
    size_t len = 0;
    if (!S.host) {
        int rtt = QueryRtt(G<uint32_t>(A->hostDpnid));
        _snprintf(out, n, "rtt(host)=%dms", rtt);
        return;
    }
    for (int i = 0; i < 8; i++) {
        uint8_t* p = (uint8_t*)(uintptr_t)(A->playerTable + i * 24);
        uint32_t id = *(uint32_t*)p, fl = *(uint32_t*)(p + 4);
        if (!(fl & 0x80) || !id) continue;
        int rtt = QueryRtt(id);
        if (rtt < 0) continue; // ourselves / not connected
        int w = _snprintf(out + len, n - len, "%sp%d=%dms", len ? " " : "rtt ", i, rtt);
        if (w < 0) break;
        len += (size_t)w;
    }
}

static void ReportWindow(const char* tag, const Stats& s, bool queryRtt = true) {
    if (!s.ticks) return;
    char rtt[256] = "";
    if (queryRtt) ReportRtts(rtt, sizeof(rtt));
    Log("%s ticks=%u sim=%u stall=%u (%.1f%%) paused=%u catchup=%u speed=%.3f backlog avg=%.2f max=%u turns=%u %s",
        tag, s.ticks, s.simTicks, s.stallTicks, 100.0 * s.stallTicks / s.ticks, s.pausedTicks, s.catchUpTicks,
        s.speedSum / s.ticks, (double)s.backlogSum / s.ticks, s.backlogMax, G<uint32_t>(A->turnsExecuted) - s.turnsAtStart,
        rtt);
    if (cfg.desyncCheck) {
        EnterCriticalSection(&D.cs);
        Log("sync: checks=%u mismatches=%u legacyPackets=%u raceSkips=%u compareMask=0x%02X", D.checks, D.mismatches,
            D.legacyPackets, D.raceSkips, CompareMask());
        LeaveCriticalSection(&D.cs);
    }
}

static void BeginSession() {
    S = Session();
    S.active = true;
    S.host = G<void*>(A->serverIn) != nullptr;
    S.total.startMs = S.win.startMs = S.lastReport = GetTickCount();
    S.total.turnsAtStart = S.win.turnsAtStart = G<uint32_t>(A->turnsExecuted);
    R.renderInit = false;
    DesyncReset();
    Log("session start: role=%s turnLen=%u ticks maxTurnsAhead=%u maxTurnSpread=%u tickPeriod=%u units",
        S.host ? "HOST" : "CLIENT", G<uint32_t>(A->turnLen), G<uint32_t>(A->maxTurnsAhead),
        G<uint32_t>(A->maxTurnSpread), G<uint32_t>(A->tickPeriod));
}

static void EndSession() {
    if (!S.active) return;
    ReportWindow("session end:", S.total);
    ReportRng();
    S.active = false;
}

static void Account(Stats& s, bool sim, bool stall, bool paused, bool caught, int backlog, double speed) {
    s.ticks++;
    s.speedSum += speed;
    if (sim) s.simTicks++;
    if (stall) s.stallTicks++;
    if (paused) s.pausedTicks++;
    if (caught) s.catchUpTicks++;
    s.backlogSum += (uint32_t)backlog;
    if ((uint32_t)backlog > s.backlogMax) s.backlogMax = (uint32_t)backlog;
}

static void AfterPump() {
    if (G<int>(A->gameMode) != 2 || !(G<uint32_t>(A->netFlags) & NF_GAME_ACTIVE)) {
        EndSession();
        return;
    }
    if (!S.active) BeginSession();

    if (S.host) {
        // The engine resets these to 3/4 whenever a session (re)initialises.
        uint32_t& ahead = G<uint32_t>(A->maxTurnsAhead);
        uint32_t& spread = G<uint32_t>(A->maxTurnSpread);
        if (cfg.maxTurnsAhead && ahead == 3) ahead = (uint32_t)cfg.maxTurnsAhead;
        if (cfg.maxTurnSpread && spread == 4) spread = (uint32_t)cfg.maxTurnSpread;
    }

    if (cfg.desyncCheck && R.addr[0]) DesyncOnTurn();

    uint32_t tick = G<uint32_t>(A->realTick);
    bool paused = G<uint32_t>(A->paused) != 0;
    bool sim = tick < G<uint32_t>(A->turnBase) + G<uint32_t>(A->turnLen) && !paused;
    bool stall = !sim && !paused;

    uint8_t* cin = G<uint8_t*>(A->clientIn);
    int backlog = cin ? CountTurnMarkers(cin) : 0;

    bool caught = false;
    double speed = 1.0;
    bool active = cfg.catchUp && cin && !paused && (!S.host || cfg.catchUpOnHost);
    int burstTarget = cfg.catchUpSmooth ? kBurstBacklog - 1 : cfg.catchUpTarget;
    if (active && cfg.catchUpSmooth && backlog < kBurstBacklog) {
        // Change the tick period a little instead of adding ticks: the loop sleeps until
        // nextTickTime after every tick, so moving it by (period/speed - period) spaces the
        // ticks evenly at `speed`. The average queue depth settles at SmoothTarget - each
        // turn is executed shortly after it arrives, and the simulation steps stay regular
        // (extra ticks run two steps inside one frame and then wait for the next turn,
        // which the joined player sees as choppy movement).
        S.catchUpAcc = 0.0;
        S.queueAvg += 0.03 * (backlog - S.queueAvg);
        speed = 1.0 + 0.4 * (S.queueAvg - cfg.smoothTarget);
        if (speed < cfg.smoothMinSpeed) speed = cfg.smoothMinSpeed;
        if (speed > cfg.smoothMaxSpeed) speed = cfg.smoothMaxSpeed;
        uint64_t period = G<uint64_t>(A->tickPeriod);
        G<uint64_t>(A->nextTickTime) += (uint64_t)((int64_t)(period / speed) - (int64_t)period);
    } else if (active && backlog > burstTarget) {
        // Run whole extra ticks. After each tick the loop re-checks the schedule
        // and keeps going *without* handing the frame to the renderer while it
        // is behind, so pushing the schedule back by more than one period makes
        // it execute one more NetPump + simulation step right away. Every tick
        // is still a complete engine tick, so the turn/tick contract holds.
        speed = 1.0 + (backlog - burstTarget) * cfg.catchUpGain;
        if (speed > cfg.catchUpMaxSpeed) speed = cfg.catchUpMaxSpeed;
        S.catchUpAcc += speed - 1.0;
        if (S.catchUpAcc >= 1.0) {
            S.catchUpAcc -= 1.0;
            uint64_t now = EngineNow();
            uint64_t period = G<uint64_t>(A->tickPeriod);
            uint64_t margin = 2ull * G<uint32_t>(A->tscPerMs); // the loop compares in whole ms
            uint64_t& next = G<uint64_t>(A->nextTickTime);
            if (now > period + margin && next > now - period - margin) next = now - period - margin;
            caught = true;
        }
    } else {
        S.catchUpAcc = 0.0;
    }

    Account(S.total, sim, stall, paused, caught, backlog, speed);
    Account(S.win, sim, stall, paused, caught, backlog, speed);

    DWORD now = GetTickCount();
    if (now - S.lastReport >= (DWORD)cfg.logInterval * 1000) {
        ReportWindow("stats:", S.win);
        ReportRng();
        S.win = Stats();
        S.win.startMs = now;
        S.win.turnsAtStart = G<uint32_t>(A->turnsExecuted);
        S.lastReport = now;
    }
}

static void __cdecl PumpHook() {
    g_origPump();
    AfterPump();
}

// ---------------------------------------------------------------------------
// 7. Keyboard text input
//
// For every DirectInput key event the stock game builds a fake WM_KEYDOWN,
// runs TranslateMessage on it and then takes the *first* message in the queue
// hoping it is the resulting WM_CHAR, finally removing one more message
// unconditionally. The real WM_KEYDOWN/WM_CHAR/WM_MOUSEMOVE messages Windows
// posts for the same input are in that queue too, so characters get lost,
// swallowed or attached to the next key ("press two keys to get one letter",
// "random letters"). We compute the characters directly from the DirectInput
// key state with ToUnicodeEx instead, keeping the engine's return format:
//   VK | char1 << 8 | char2 << 16 | 0x1000000 (left Alt) | 0x2000000 (Alt+char)
// ---------------------------------------------------------------------------
typedef UINT(__cdecl* KeyVkFn)(UINT* scanOut, UINT dik);

static UINT LayoutCodePage(HKL hkl) {
    char buf[16];
    if (GetLocaleInfoA(MAKELCID(LOWORD((UINT_PTR)hkl), SORT_DEFAULT), LOCALE_IDEFAULTANSICODEPAGE, buf, sizeof(buf)) > 0) {
        UINT cp = (UINT)atoi(buf);
        if (cp) return cp;
    }
    return CP_ACP;
}

static uint32_t __cdecl KeyToChar(uint32_t dik, char down, int prevDown) {
    (void)prevDown;
    UINT scan = 0;
    UINT vk = ((KeyVkFn)(uintptr_t)A->keyVkFn)(&scan, dik);
    if (!vk) return 0;
    const uint8_t* di = (const uint8_t*)(uintptr_t)A->diKeyState;
    auto dn = [&](int k) { return (di[k] & 0x80) != 0; };
    bool lalt = dn(0x38);
    uint32_t r = (vk & 0xFF) | (lalt ? 0x1000000u : 0);
    if (!down) return r;

    BYTE ks[256];
    memset(ks, 0, sizeof(ks));
    if (dn(0x2A)) ks[VK_LSHIFT] = 0x80;
    if (dn(0x36)) ks[VK_RSHIFT] = 0x80;
    if (dn(0x1D)) ks[VK_LCONTROL] = 0x80;
    if (dn(0x9D)) ks[VK_RCONTROL] = 0x80;
    if (lalt) ks[VK_LMENU] = 0x80;
    if (dn(0xB8)) { // AltGr = Ctrl+Alt for Windows layouts (Polish: ą ć ę ł ń ó ś ź ż)
        ks[VK_RMENU] = 0x80;
        ks[VK_LCONTROL] = 0x80;
    }
    if (ks[VK_LSHIFT] | ks[VK_RSHIFT]) ks[VK_SHIFT] = 0x80;
    if (ks[VK_LCONTROL] | ks[VK_RCONTROL]) ks[VK_CONTROL] = 0x80;
    if (ks[VK_LMENU] | ks[VK_RMENU]) ks[VK_MENU] = 0x80;
    if (di[0x3A] & 1) ks[VK_CAPITAL] = 0x01; // toggles the engine keeps in the DIK array
    if (di[0x45] & 1) ks[VK_NUMLOCK] = 0x01;
    ks[vk & 0xFF] |= 0x80;

    HKL hkl = GetKeyboardLayout(0);
    WCHAR w[8];
    // flag 4: do not touch the thread's dead-key state (Windows 10 1607+)
    int n = ToUnicodeEx(vk, scan, ks, w, 8, 4, hkl);
    if (n <= 0) return r; // no character or a dead key
    char mb[8];
    int m = WideCharToMultiByte(LayoutCodePage(hkl), 0, w, n, mb, sizeof(mb), nullptr, nullptr);
    if (m <= 0) return r;
    r |= (uint32_t)(uint8_t)mb[0] << 8;
    if (m > 1) r |= (uint32_t)(uint8_t)mb[1] << 16;
    if (lalt) r |= 0x2000000u;
    return r;
}

static bool InstallKeyboardFix() {
    if (!MatchCall(A->keyConvSite, A->keyConvFn)) return false;
    static const uint8_t vkPro[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x8B, 0x45, 0x0C, 0x83, 0xE0, 0x7F};
    if (!Match(A->keyVkFn, vkPro, sizeof(vkPro))) return false;
    return WriteRel32(A->keyConvSite, 0xE8, (void*)&KeyToChar);
}

// ---------------------------------------------------------------------------
// 8. Unique identity per instance (testing with AllowMultipleInstances)
//
// On joining, a client sends message 0x60010 with a 64-bit identity derived
// from its serial key; the host rejects it as "invalid serial number" when it
// equals the host's own or another player's. Two instances on one PC read the
// same key, so the second one is always rejected. In testing mode each process
// perturbs the identity it sends with its PID.
// ---------------------------------------------------------------------------
static uint32_t g_idHiAddr, g_idSalt;

static __declspec(naked) void IdHiThunk() {
    __asm {
        mov eax, g_idHiAddr
        mov eax, dword ptr [eax]
        xor eax, g_idSalt
        ret
    }
}

static bool InstallUniqueIdentity() {
    uint8_t chk[8] = {0xA1, 0, 0, 0, 0, 0x89, 0x43, 0x0C};
    Le32(chk + 1, A->installIdHi);
    if (!Match(A->idSendSite, chk, sizeof(chk))) return false;
    g_idHiAddr = A->installIdHi;
    g_idSalt = (GetCurrentProcessId() << 8) | 0x4B;
    return WriteRel32(A->idSendSite, 0xE8, (void*)&IdHiThunk);
}

// ---------------------------------------------------------------------------
// 9. Steam transport: route the game's DirectPlay 8 peer through Steam
// ---------------------------------------------------------------------------
void** FindImport(void* mod, const char* dll, const char* func) {
    auto* base = (uint8_t*)mod;
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char*)(base + d->Name), dll) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* iat = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) == 0) return (void**)&iat->u1.Function;
        }
    }
    return nullptr;
}

static bool InstallSteamTransport() {
    void** slot = FindImport(GetModuleHandleA(nullptr), "ole32.dll", "CoCreateInstance");
    if (!slot) return false;
    Steam_Configure(cfg.steam, (HCoCreate)*slot);
    void* hook = (void*)&Steam_CoCreateInstance;
    return WriteCode((uint32_t)(uintptr_t)slot, &hook, sizeof(hook));
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------
static void Install() {
    HMODULE exe = GetModuleHandleA(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)exe;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + dos->e_lfanew);
    if ((uintptr_t)exe != 0x400000) return;
    g_imgLo = 0x400000;
    g_imgHi = 0x400000 + nt->OptionalHeader.SizeOfImage;

    if (VerifyCore(kEx1))
        A = &kEx1;
    else if (VerifyCore(kEx2))
        A = &kEx2;

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, sizeof(exePath));
    if (!A) {
        Log("KSNetFix: unrecognised executable %s - running as plain dinput8 proxy", exePath);
        return;
    }
    Log("KSNetFix " KSNETFIX_VERSION " attached to %s [%s]", exePath, A->name);
    if (!cfg.enabled) {
        Log("disabled in ksnetfix.ini");
        return;
    }

    for (int i = 0; i < 8; i++) R.addr[i] = (uint32_t*)(uintptr_t)(A->rngBase + kRngOffsets[i]);
    LcgInitJumps();

    if (cfg.timerResolution) {
        timeBeginPeriod(1);
        Log("timer resolution: 1 ms");
    }
    if (cfg.preciseClock) {
        bool ok = InstallPreciseClock();
        Log("precise clock: %s (QPC %llu Hz, fast path x%u, 1000000 units/ms)", ok ? "ok" : "FAILED", g_qpf, g_qpcMul);
    }
    if (cfg.ackEveryTurns) Log("ack every %d turn(s): %s", cfg.ackEveryTurns, InstallAckCadence() ? "ok" : "FAILED");

    g_origPump = (PumpFn)(uintptr_t)A->pumpFn;
    Log("net pump hook: %s", WriteRel32(A->pumpCallSite, 0xE8, (void*)&PumpHook) ? "ok" : "FAILED");
    if (cfg.catchUpSmooth)
        Log("catch-up: %s smooth target=%.2f speed=%.2f..%.2fx (burst from %d turns: gain=%.2f maxSpeed=%.1fx) host=%d",
            cfg.catchUp ? "on" : "off", cfg.smoothTarget, cfg.smoothMinSpeed, cfg.smoothMaxSpeed, kBurstBacklog, cfg.catchUpGain,
            cfg.catchUpMaxSpeed, cfg.catchUpOnHost);
    else
        Log("catch-up: %s extra ticks target=%d gain=%.2f maxSpeed=%.1fx host=%d", cfg.catchUp ? "on" : "off",
            cfg.catchUpTarget, cfg.catchUpGain, cfg.catchUpMaxSpeed, cfg.catchUpOnHost);
    Log("host limits: maxTurnsAhead=%d maxTurnSpread=%d", cfg.maxTurnsAhead, cfg.maxTurnSpread);

    if (cfg.rngMonitor || cfg.rngIsolation || cfg.desyncCheck) {
        if (VerifyTickHooks(*A)) {
            g_tickStartTarget = A->tickStartFn;
            g_relSemIatAddr = A->relSemIat;
            bool ok = WriteRel32(A->tickStartSite, 0xE8, (void*)&TickStartHook) &&
                      WriteRel32(A->relSemSite, 0xE8, (void*)&RelSemHook, 1);
            Log("tick boundary hooks: %s (rng isolation=%d monitor=%d)", ok ? "ok" : "FAILED", cfg.rngIsolation,
                cfg.rngMonitor);
        } else {
            Log("tick boundary hooks: signature mismatch - RNG isolation/monitor disabled");
            cfg.rngIsolation = cfg.rngMonitor = 0;
        }
    }
    if (cfg.desyncCheck) {
        if (VerifyPacketHook(*A)) {
            g_packetResume = A->packetFn + 6;
            Log("desync detection: %s", WriteRel32(A->packetFn, 0xE9, (void*)&PacketHook, 1) ? "ok" : "FAILED");
        } else {
            Log("desync detection: signature mismatch - disabled");
            cfg.desyncCheck = 0;
        }
    }
    if (cfg.allowMultipleInstances) {
        static const uint8_t chk[] = {0x3D, 0xB7, 0x00, 0x00, 0x00, 0x74, 0x0E};
        static const uint8_t nop2[] = {0x90, 0x90};
        bool ok = Match(A->mutexJeSite - 5, chk, sizeof(chk)) && WriteCode(A->mutexJeSite, nop2, 2);
        Log("multiple instances allowed (testing): %s", ok ? "ok" : "FAILED");
        Log("unique join identity per instance (testing): %s", InstallUniqueIdentity() ? "ok" : "FAILED");
    }
    if (cfg.keyboardFix) Log("keyboard text input fix: %s", InstallKeyboardFix() ? "ok" : "FAILED");
    if (cfg.steam.enabled)
        Log("steam transport (TCP/IP -> Steam, app id %u, %s): %s", cfg.steam.appId,
            cfg.steam.gameServer ? "anonymous game server" : (cfg.steam.lobbyFriendsOnly ? "friends-only lobbies" : "public lobbies"),
            InstallSteamTransport() ? "ok" : "FAILED");
    if (cfg.steamNet.enabled) {
        SteamNetGameAddrs g = {A == &kEx1 ? 0x0092C8D4u : 0x0093605Cu, A->installIdHi - 4};
        Log("steamnet (EarthNet -> local server%s): %s", A == &kEx1 ? "" : ", untested on this engine",
            SteamNet_Install(cfg.steamNet, g) ? "ok" : "FAILED");
    }
    if (cfg.netTrace) {
        g_trace = OpenLog("ksnettrace.log");
        Log("engine net trace -> ksnettrace.log: %s", WriteRel32(A->traceFn, 0xE9, (void*)&TraceHook) ? "ok" : "FAILED");
    }
#ifdef KSNETFIX_SERVER
    if (cfg.server) {
        ServerGameAddrs g = {A->gameMode, A->tscPerMs, A->playerTable};
        Log("dedicated server " KSSERVER_VERSION ": %s", Server_Install(A == &kEx1, g) ? "ok" : "FAILED");
    }
#endif
}

// ---------------------------------------------------------------------------
// dinput8 proxy
// ---------------------------------------------------------------------------
typedef HRESULT(WINAPI* DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
static DI8Create g_realCreate;

static DI8Create RealCreate() {
    if (!g_realCreate) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, "\\dinput8.dll");
        HMODULE h = LoadLibraryA(path);
        if (h) g_realCreate = (DI8Create)GetProcAddress(h, "DirectInput8Create");
    }
    return g_realCreate;
}

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, void* outer) {
    DI8Create real = RealCreate();
    if (!real) return E_FAIL;
    return real(hinst, ver, riid, out, outer);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logLock);
        InitializeCriticalSection(&D.cs);
        GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
        char* slash = strrchr(g_dir, '\\');
        if (slash) slash[1] = 0;
        LoadConfig();
        if (cfg.log) {
            char name[64] = "ksnetfix.log";
            if (cfg.allowMultipleInstances) _snprintf(name, sizeof(name), "ksnetfix_%lu.log", GetCurrentProcessId());
            g_log = OpenLog(name);
        }
        Install();
    } else if (reason == DLL_PROCESS_DETACH) {
        // No DirectPlay calls under the loader lock.
        if (A && S.active) ReportWindow("session end (exit):", S.total, false);
        if (A && cfg.enabled && cfg.rngMonitor) ReportRng();
        if (cfg.timerResolution && A && cfg.enabled) timeEndPeriod(1);
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
        if (g_trace != INVALID_HANDLE_VALUE) CloseHandle(g_trace);
    }
    return TRUE;
}
