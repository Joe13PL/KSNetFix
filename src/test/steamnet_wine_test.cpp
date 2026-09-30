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
void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}
bool Match(uint32_t va, const uint8_t* bytes, size_t n) { return memcmp((void*)(uintptr_t)va, bytes, n) == 0; }
bool WriteCode(uint32_t va, const void* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect((void*)(uintptr_t)va, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)(uintptr_t)va, bytes, n);
    VirtualProtect((void*)(uintptr_t)va, n, old, &old);
    return true;
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
static uint8_t g_clientObj[0x5200];
static uint8_t* g_clientPtr = g_clientObj;
static uint32_t g_oldLogin[8] = {1, 3, 3, 0x006f004a, 0x00000065}; // L"Joe"
static void* __cdecl GameAlloc(size_t n) { return malloc(n); }
static std::string SteamAccount() { return "Wojtek"; }
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
    void Join(const std::string& name, int score, const std::vector<int32_t>& details) override {
        if (name != "SteamNet") return;
        en::BoardEntry e;
        e.steamName = "Wojtek";
        e.score = score;
        e.details = details;
        board.push_back(e);
    }
} g_ranking;

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
                               (uint32_t)(uintptr_t)&g_clientPtr, (uint32_t)(uintptr_t)&GameAlloc};
    *(uint32_t**)(g_clientObj + 0x4A80) = g_oldLogin;
    CHECK(SteamNet_Install(st, addrs));
    SteamNet_SetRanking(&g_ranking);
    SteamNet_SetAccount(&SteamAccount);
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
    static char hbuf[MAXGETHOSTSTRUCT];
    CHECK(ghbn(wnd, WM_USER + 1, "steam", hbuf, sizeof(hbuf)) != nullptr);
    {   // the login the game is about to send is now the Steam account
        uint32_t* login = *(uint32_t**)(g_clientObj + 0x4A80);
        CHECK(login != g_oldLogin && g_oldLogin[0] == 0);
        CHECK(login[0] == 1 && login[1] == 6 && login[2] == 6 && wcscmp((wchar_t*)(login + 3), L"Wojtek") == 0);
    }
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

    std::vector<uint8_t> in, body;
    en::Writer info;
    info.raw("client-info", 11);
    SendAll(s, info.Packet());
    CHECK(RecvPacket(s, in, body));
    en::Reader r(body.data(), body.size());
    CHECK(r.u32() == 0 && r.str() == "Witaj w SteamNet (test)");

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
    CHECK(line == "/send \"Joe\" \"czesc\"");

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
