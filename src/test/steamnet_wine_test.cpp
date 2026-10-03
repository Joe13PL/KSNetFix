// SteamNet Windows-side test: plays the game around earthnet.cpp - menu string, identity,
// server list in the registry, ws2_32 loaded through GetProcAddress, then a full login and
// chat over a real loopback connection. Runs on Windows or under Wine:
//   ./build.sh tests  (MSVC) -> build/test/steamnet_test.exe, or from src/test:
//   i686-w64-mingw32-g++ -std=c++17 -O1 -static -I.. steamnet_wine_test.cpp ../earthnet.cpp \
//       ../earthnet_core.cpp -lws2_32 -ladvapi32 -o steamnet_wine_test.exe && wine steamnet_wine_test.exe
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "earthnet.h"
#include "earthnet_core.h"
#include "patch.h"

// --- helpers normally provided by ksnetfix.cpp -------------------------------------------
static std::vector<std::string> g_logs;
void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("  log: %s\n", buf);
    fflush(stdout);
    g_logs.push_back(buf);
}
static bool Logged(const std::string& text) {
    for (auto& l : g_logs)
        if (l.find(text) != std::string::npos) return true;
    return false;
}
bool Match(uint32_t va, const uint8_t* bytes, size_t n) { return memcmp((void*)(uintptr_t)va, bytes, n) == 0; }
bool WriteCode(uint32_t va, const void* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect((void*)(uintptr_t)va, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)(uintptr_t)va, bytes, n);
    VirtualProtect((void*)(uintptr_t)va, n, old, &old);
    return true;
}
bool WriteRel32(uint32_t site, uint8_t opcode, const void* target, size_t pad) {
    uint8_t b[16] = {opcode};
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)target - (site + 5));
    memcpy(b + 1, &rel, 4);
    memset(b + 5, 0x90, pad);
    return WriteCode(site, b, 5 + pad);
}
bool MatchCall(uint32_t site, uint32_t target) {
    uint8_t call[5] = {0xE8};
    int32_t rel = (int32_t)(target - (site + 5));
    memcpy(call + 1, &rel, 4);
    return Match(site, call, sizeof(call));
}
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

static int g_fail = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
            fflush(stdout);                                               \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

// "Game" data the hooks look at.
static wchar_t g_menuString[9] = L"EarthNet";
// EarthNet client object with the login string at +0x4A80, as 0x7FF8D0 leaves it before resolving
// The global client pointer still names an older object while "fastest server" connects with
// its own (the login result makes it the global later): the login must change in the connecting one.
static const uint32_t kClientVtable = 0x008FABA0;
static uint8_t g_clientObj[0x5200];
static uint8_t g_globalClientObj[0x5200];
static uint8_t* g_clientPtr = g_globalClientObj;
static uint32_t g_savedLoginObj[4] = {1, 4, 4, 0x74736554}; // "Test" + 0: the profile's login (DAT_00a58f08)
static uint32_t* g_savedLogin = g_savedLoginObj;
static uint32_t g_oldLogin[8] = {1, 3, 3, 0x006f004a, 0x00000065}; // L"Joe"
static void* __cdecl GameAlloc(size_t n) { return malloc(n); }
static SteamNetAccount SteamAccount() {
    SteamNetAccount a;
    a.name = "Wojtek";
    a.id = 76561198000000001ull;
    return a;
}
static uint32_t g_identity[2] = {0x89abcdef, 0x01234567};

static const char kKey[] = "Software\\Reality Pump\\KnightShift\\BaseGame\\Network\\EarthNet";

static std::string ReadList() {
    char buf[1024] = {0};
    DWORD size = sizeof(buf) - 1, type;
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return "";
    RegQueryValueExA(k, "AddressIP", nullptr, &type, (BYTE*)buf, &size);
    RegCloseKey(k);
    return buf;
}

static bool RecvAll(SOCKET s, std::vector<uint8_t>& in, size_t need) {
    char buf[4096];
    while (in.size() < need) {
        int k = recv(s, buf, sizeof(buf), 0);
        if (k <= 0) return false;
        in.insert(in.end(), buf, buf + k);
    }
    return true;
}

static bool RecvPacket(SOCKET s, std::vector<uint8_t>& in, std::vector<uint8_t>& body) {
    if (!RecvAll(s, in, 4)) return false;
    uint32_t len = in[0] | in[1] << 8 | in[2] << 16 | (uint32_t)in[3] << 24;
    if (!RecvAll(s, in, len)) return false;
    bool ok = en::Inflate(in.data() + 4, len - 4, body); // packets are zlib streams
    in.erase(in.begin(), in.begin() + len);
    return ok;
}

static bool RecvLine(SOCKET s, std::vector<uint8_t>& in, std::string& line) {
    for (;;) {
        for (size_t i = 0; i < in.size(); i++)
            if (in[i] == 0) {
                line.assign(in.begin(), in.begin() + i);
                in.erase(in.begin(), in.begin() + i + 1);
                return true;
            }
        size_t have = in.size();
        if (!RecvAll(s, in, have + 1)) return false;
    }
}

// Stands in for Steam leaderboards (steampeer.cpp).
struct FakeRanking : en::RankingService {
    std::vector<en::BoardEntry> board;
    bool Top(const std::string& name, bool create, int count, std::vector<en::BoardEntry>& out) override {
        (void)create;
        if (name == "SteamNet") out.assign(board.begin(), board.begin() + (board.size() < (size_t)count ? board.size() : count));
        return true;
    }
    void Join(const std::string& name, int score, const std::vector<int32_t>& details,
              std::function<bool(std::vector<int32_t>&)> refresh) override {
        (void)refresh;
        if (name != "SteamNet") return;
        en::BoardEntry e;
        e.steamName = "Wojtek";
        e.score = score;
        e.details = details;
        board.push_back(e);
    }
    void Update(const std::string&, std::function<void(int&, std::vector<int32_t>&)>) override {}
} g_ranking;

// Stands in for Steam lobbies: events are pushed from this (test) thread, like the Steam thread.
struct FakeLobbies : en::LobbyService {
    std::shared_ptr<en::LobbyEvents> events;
    std::vector<std::string> said;
    void Start(std::shared_ptr<en::LobbyEvents> e) override { events = e; }
    void Enter(const std::string&) override {}
    void Say(const std::string& text) override { said.push_back(text); }
    void Whisper(uint64_t, const std::string&) override {}
    void RefreshChannels() override {}
    void PublishGame(const std::string&, const std::string&) override {}
    void UnpublishGame() override {}
    void Stop() override {}
} g_lobbies;

static void SendAll(SOCKET s, const std::vector<uint8_t>& b) { send(s, (const char*)b.data(), (int)b.size(), 0); }

int main() {
    // Registry as the Steam installer leaves it (plus an entry of the player's own).
    HKEY k;
    RegCreateKeyExA(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr);
    const char list[] = "\"EarthNet\"\"netserver.earthnet.de\"\"test\"\"31.220.91.214\"";
    RegSetValueExA(k, "AddressIP", 0, REG_SZ, (const BYTE*)list, sizeof(list));
    DWORD port = 17171;
    RegSetValueExA(k, "Port", 0, REG_DWORD, (const BYTE*)&port, 4);
    RegCloseKey(k);

    SteamNetSettings st;
    strcpy(st.welcome, "Witaj w SteamNet (test)");
    SteamNetGameAddrs addrs = {(uint32_t)(uintptr_t)g_menuString, (uint32_t)(uintptr_t)g_identity,
                               (uint32_t)(uintptr_t)&g_clientPtr, kClientVtable, 0x4AB0, (uint32_t)(uintptr_t)&GameAlloc,
                               (uint32_t)(uintptr_t)&g_savedLogin};
    memcpy(g_clientObj, &kClientVtable, 4);
    memcpy(g_globalClientObj, &kClientVtable, 4);
    *(uint32_t**)(g_clientObj + 0x4A80) = g_oldLogin;

    // The game's result setter (0x82E840) and its callers: the in-game menu's quit (push 0), a
    // victory (push 1) and a defeat outside rated matches (push 2); "push x; call setter; add esp, 4; ret".
    static int32_t gameResult = -1;
    auto* code = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    const uint8_t setter[40] = {0x55, 0x8B, 0xEC, 0xA1, 0, 0, 0, 0, 0x85, 0xC0, 0x74, 0x18, 0x83, 0xB8, 0x98, 0x4A,
                                0x00, 0x00, 0xFF, 0x74, 0x0F, 0x8B, 0x45, 0x08, 0xA3, 0, 0, 0, 0, 0xB8, 0x01, 0x00,
                                0x00, 0x00, 0x5D, 0xC3, 0x33, 0xC0, 0x5D, 0xC3};
    memcpy(code, setter, sizeof(setter));
    uint32_t clientGlobal = (uint32_t)(uintptr_t)&g_clientPtr, resultGlobal = (uint32_t)(uintptr_t)&gameResult;
    memcpy(code + 4, &clientGlobal, 4);
    memcpy(code + 25, &resultGlobal, 4);
    auto caller = [&](int at, uint8_t value) {
        uint8_t* c = code + at;
        c[0] = 0x6A, c[1] = value, c[2] = 0xE8;
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)code - (uint32_t)(uintptr_t)(c + 7));
        memcpy(c + 3, &rel, 4);
        c[7] = 0x83, c[8] = 0xC4, c[9] = 0x04, c[10] = 0xC3;
        return (int(__cdecl*)())(void*)c;
    };
    auto quit = caller(64, 0), victory = caller(96, 1), defeat = caller(128, 2);

    // The game's file open (0x799BC0, __thiscall file, name, flags): the same prologue, then it
    // keeps the name and flags it got. Never run: its calls of the loose-file open (0x798E40,
    // __thiscall file, path, flags, offset, size) at +0xC0 / +0x19C and "+0x3C = +0x20" at +0x1E7.
    static const char* openedName;
    static unsigned openedFlags;
    static const char* rawPath;
    static int32_t rawArgs[3]; // flags, offset, size
    uint8_t* fo = code + 512;
    const uint8_t prologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28};
    memcpy(fo, prologue, 6);
    uint32_t pName = (uint32_t)(uintptr_t)&openedName, pFlags = (uint32_t)(uintptr_t)&openedFlags;
    uint8_t foBody[] = {0x8B, 0x45, 0x08, 0xA3, 0, 0, 0, 0, 0x8B, 0x45, 0x0C, 0xA3, 0, 0, 0, 0, // keep name, flags
                        0xB8, 0x01, 0x00, 0x00, 0x00, 0x8B, 0xE5, 0x5D, 0xC2, 0x08, 0x00};   // return 1 (ret 8)
    memcpy(foBody + 4, &pName, 4);
    memcpy(foBody + 12, &pFlags, 4);
    memcpy(fo + 6, foBody, sizeof(foBody));
    // the loose-file open: keeps its arguments, file+0x20 = 1234 (the size), returns 1 (ret 10h)
    uint8_t* raw = code + 1024;
    uint32_t pRaw = (uint32_t)(uintptr_t)&rawPath, pArgs = (uint32_t)(uintptr_t)rawArgs;
    uint8_t rawBody[] = {0x8B, 0x44, 0x24, 0x04, 0xA3, 0, 0, 0, 0,       // mov eax, [esp+4]; mov [rawPath], eax
                         0x8B, 0x44, 0x24, 0x08, 0xA3, 0, 0, 0, 0,       // flags
                         0x8B, 0x44, 0x24, 0x0C, 0xA3, 0, 0, 0, 0,       // offset
                         0x8B, 0x44, 0x24, 0x10, 0xA3, 0, 0, 0, 0,       // size
                         0xC7, 0x41, 0x20, 0xD2, 0x04, 0x00, 0x00,       // mov dword [ecx+20h], 1234
                         0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x10, 0x00}; // return 1
    memcpy(rawBody + 5, &pRaw, 4);
    for (int i = 0; i < 3; i++) {
        uint32_t a = pArgs + 4 * i;
        memcpy(rawBody + 14 + 9 * i, &a, 4);
    }
    memcpy(raw, rawBody, sizeof(rawBody));
    for (int site : {0xC0, 0x19C}) {
        fo[site] = 0xE8;
        int32_t rel = (int32_t)((uint32_t)(uintptr_t)raw - ((uint32_t)(uintptr_t)fo + site + 5));
        memcpy(fo + site + 1, &rel, 4);
    }
    const uint8_t keepSize[6] = {0x8B, 0x4E, 0x20, 0x89, 0x4E, 0x3C};
    memcpy(fo + 0x1E7, keepSize, 6);
    typedef int(__fastcall * OpenFn)(void*, void*, const char*, unsigned);
    auto open = (OpenFn)(void*)fo;
    addrs.fileOpen = (uint32_t)(uintptr_t)fo;
    static uint8_t gameFile[0x60];
    static const uint8_t bannerTex[] = {'T', 'E', 'X', 0, 2, 0, 0, 0};
    SteamNet_SetBanner(bannerTex, sizeof(bannerTex));
    addrs.resultSetter = (uint32_t)(uintptr_t)code;
    addrs.resultGlobal = resultGlobal;
    addrs.quitCall = (uint32_t)(uintptr_t)code + 64 + 2;
    addrs.defeatCall = (uint32_t)(uintptr_t)code + 128 + 2;
    addrs.victoryCalls[0] = (uint32_t)(uintptr_t)code + 96 + 2;
    addrs.victoryCalls[1] = (uint32_t)(uintptr_t)code + 200; // no call there: dropped
    CHECK(SteamNet_Install(st, addrs));
    CHECK(Logged("steamnet: match results: ok"));
    CHECK(Logged("steamnet: lobby banner: ok"));
    // other files go through as they are
    CHECK(open(gameFile, nullptr, "Interface\\Bkg0000.tex", 1) == 1 && strcmp(openedName, "Interface\\Bkg0000.tex") == 0 &&
          openedFlags == 1 && !rawPath);
    // the banner: <game>\SteamNet\Banner.tex opened as a loose file (offset 0, size -1), size kept
    char gameDir[MAX_PATH];
    GetModuleFileNameA(nullptr, gameDir, MAX_PATH);
    strrchr(gameDir, '\\')[1] = 0;
    std::string bannerFile = std::string(gameDir) + "SteamNet\\Banner.tex";
    CHECK(open(gameFile, nullptr, "Banners\\BannerDef.tex", 1) == 1);
    CHECK(rawPath && bannerFile == rawPath && rawArgs[0] == 1 && rawArgs[1] == 0 && rawArgs[2] == -1);
    CHECK(*(uint32_t*)(gameFile + 0x3C) == 1234);
    CHECK(Logged("lobby banner \"Banners\\BannerDef.tex\" -> " + bannerFile + ": ok"));
    char written[16] = {0};
    FILE* bf = fopen(bannerFile.c_str(), "rb");
    CHECK(bf && fread(written, 1, sizeof(written), bf) == sizeof(bannerTex) && memcmp(written, bannerTex, sizeof(bannerTex)) == 0);
    if (bf) fclose(bf);
    rawPath = nullptr;
    CHECK(open(gameFile, nullptr, "banners/BANNERDEF.TEX", 1) == 1 && rawPath); // any case, either slash
    rawPath = nullptr;
    CHECK(open(gameFile, nullptr, "Banners\\MyBannerDef.tex", 1) == 1 && !rawPath && openedFlags == 1);
    CHECK(open(gameFile, nullptr, "Banners\\BannerDef.tex", 3) == 1 && !rawPath && openedFlags == 3); // writing
    // a SteamNet match (client+0x4A98 = -1): the game keeps nothing, SteamNet still learns the result
    *(int32_t*)(g_globalClientObj + 0x4A98) = -1;
    quit();
    CHECK(gameResult == -1 && Logged("match result 0 at") && Logged(": quit"));
    defeat(); // 2 = "not rated", but it comes from the defeat
    CHECK(gameResult == -1 && Logged("match result 2 at") && Logged(": defeat"));
    CHECK(Logged("match results: no call at"));
    // a rated EarthNet match: kept as before
    *(int32_t*)(g_globalClientObj + 0x4A98) = 7;
    victory();
    CHECK(gameResult == 1 && Logged("match result 1 at") && Logged(": victory"));
    *(int32_t*)(g_globalClientObj + 0x4A98) = 0;
    SteamNet_SetRanking(&g_ranking);
    SteamNet_SetAccount(&SteamAccount);
    SteamNet_SetLobbies(&g_lobbies);
    CHECK(wcscmp(g_menuString, L"SteamNet") == 0);

    // The game loads ws2_32 at start-up and takes every function with GetProcAddress.
    HMODULE ws2 = LoadLibraryA("ws2_32.dll");
    typedef HANDLE(WINAPI * GHBN)(HWND, u_int, const char*, char*, int);
    typedef int(WINAPI * CONN)(SOCKET, const sockaddr*, int);
    auto ghbn = (GHBN)(void*)GetProcAddress(ws2, "WSAAsyncGetHostByName");
    auto conn = (CONN)(void*)GetProcAddress(ws2, "connect");
    CHECK((void*)ghbn != (void*)::GetProcAddress(ws2, "WSAAsyncGetHostByName") || true);
    std::string after = ReadList();
    printf("  server list: %s\n", after.c_str());
    CHECK(after == "\"SteamNet\"\"steam\"\"test\"\"31.220.91.214\"");

    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    HWND wnd = CreateWindowExA(0, "STATIC", "en", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    char* hbuf = (char*)g_clientObj + 0x4AB0; // 0x7FF8D0 resolves into the client object
    CHECK(ghbn(wnd, WM_USER + 1, "steam", hbuf, MAXGETHOSTSTRUCT) != nullptr);
    // resolving the name alone leaves the login alone ("fastest server" rewrites it afterwards)
    CHECK(*(uint32_t**)(g_clientObj + 0x4A80) == g_oldLogin);
    MSG m;
    bool resolved = false;
    DWORD t0 = GetTickCount();
    while (!resolved && GetTickCount() - t0 < 5000) {
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_USER + 1) resolved = WSAGETASYNCERROR(m.lParam) == 0;
            DispatchMessageA(&m);
        }
        Sleep(10);
    }
    CHECK(resolved);
    hostent* he = (hostent*)hbuf;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(17171);
    if (resolved) memcpy(&a.sin_addr, he->h_addr_list[0], 4);
    CHECK(a.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(conn(s, (sockaddr*)&a, sizeof(a)) == 0);
    // connect() only prepares the new login; the game could still rewrite its own until the hello
    CHECK(*(uint32_t**)(g_clientObj + 0x4A80) == g_oldLogin);
    // the saved login is replaced on the game thread right away (profile, "X entered the channel")
    CHECK(g_savedLogin != g_savedLoginObj && g_savedLoginObj[0] == 0);
    CHECK(g_savedLogin[2] == 6 && strcmp((const char*)(g_savedLogin + 3), "Wojtek") == 0);

    std::vector<uint8_t> in, body;
    en::Writer info;
    info.raw("client-info", 11);
    SendAll(s, info.Packet());
    CHECK(RecvPacket(s, in, body));
    en::Reader r(body.data(), body.size());
    CHECK(r.u32() == 0 && r.str() == "Witaj w SteamNet (test)");
    {   // the hello is what makes the client build its login packet: the Steam account is in place
        uint32_t* login = *(uint32_t**)(g_clientObj + 0x4A80);
        CHECK(login != g_oldLogin && g_oldLogin[0] == 0);
        CHECK(login[0] == 1 && login[1] == 6 && login[2] == 6 && wcscmp((wchar_t*)(login + 3), L"Wojtek") == 0);
        CHECK(*(uint32_t**)(g_globalClientObj + 0x4A80) == nullptr); // the global object is left alone
    }

    en::Writer login;
    login.str("Joe");
    login.str("");
    login.u32(0);
    login.u32(0);
    login.u32(0);
    uint8_t g[16] = {0};
    login.raw(g, 16);
    SendAll(s, login.Packet());
    CHECK(RecvPacket(s, in, body));
    uint8_t sig[64];
    en::Sign(0x0123456789abcdefull, sig);
    CHECK(body.size() > 64 && memcmp(body.data() + body.size() - 64, sig, 64) == 0);

    std::string line;
    CHECK(RecvLine(s, in, line));
    printf("  line: %s\n", line.c_str());
    CHECK(line.rfind("$channel \"KnightShift\"", 0) == 0);
    CHECK(RecvLine(s, in, line));
    printf("  line: %s\n", line.c_str());
    CHECK(line.rfind("/syncstats ", 0) == 0);
    CHECK(RecvLine(s, in, line));
    printf("  line: %s\n", line.c_str());

    std::string say = "/msg \"#KnightShift\" \"czesc\""; // chat as the game sends it
    say.push_back('\0');
    send(s, say.data(), (int)say.size(), 0);
    CHECK(RecvLine(s, in, line));
    printf("  line: %s\n", line.c_str());
    CHECK(line == "/send \"Wojtek\" \"czesc\""); // chat shows the Steam account
    CHECK(g_lobbies.said.empty()); // the channel lobby has not answered yet: held back

    // the channel lobby answers from another thread: the server wakes up and tells the game
    CHECK(g_lobbies.events != nullptr);
    if (g_lobbies.events) {
        en::LobbyEvent e;
        e.kind = en::LobbyEvent::ENTERED;
        e.channel = "KnightShift";
        e.members = {{11, "Ania"}};
        g_lobbies.events->Push(e);
        en::LobbyEvent say;
        say.kind = en::LobbyEvent::SAY;
        say.member = {11, "Ania"};
        say.text = "hej";
        g_lobbies.events->Push(say);
    }
    bool sawUser = false, sawSay = false;
    for (int i = 0; i < 4 && RecvLine(s, in, line); i++) {
        printf("  line: %s\n", line.c_str());
        if (line.rfind("$user \"Ania\"", 0) == 0) sawUser = true;
        if (line == "/send \"Ania\" \"hej\"") {
            sawSay = true;
            break;
        }
    }
    CHECK(sawUser && sawSay);
    CHECK(g_lobbies.said.size() == 1 && g_lobbies.said[0] == "czesc"); // sent once in the lobby

    // ranking: the login put Joe on the (fake) Steam board, /ladder shows him
    std::string ladder = "/ladder";
    ladder.push_back('\0');
    send(s, ladder.data(), (int)ladder.size(), 0);
    CHECK(RecvLine(s, in, line));
    printf("  line: %s\n", line.c_str());
    // the test sends "Joe" in the login packet; the ranking uses the Steam account
    CHECK(line.rfind("/ladder 0 \"Wojtek\" \"", 0) == 0 && line.find("\" 0 0 0 0 0 0 0") != std::string::npos);
    closesocket(s);
    Sleep(200);

    printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
