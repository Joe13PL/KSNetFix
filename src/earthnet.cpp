// SteamNet - the game's EarthNet client talks to a local EarthNet server emulator.
//
// The EarthNet client resolves the server with WSAAsyncGetHostByName and connects with
// connect(), both taken from ws2_32.dll through GetProcAddress (table at 0x922A28, filled at
// start-up by 0x6FD3B0). We hook the game's GetProcAddress import, hand out wrappers for those
// two functions and send connections for "steam" / netserver.earthnet.de to a listener on
// 127.0.0.1. The protocol itself lives in earthnet_core.cpp.
//
// This version: rename to SteamNet, server list entry, login, one
// offline channel with chat, RPG heroes kept in <game>\SteamNet\. Steam lobbies come next.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <memory>
#include <string>
#include <vector>

#include "earthnet.h"
#include "earthnet_core.h"
#include "patch.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")



namespace {

SteamNetSettings S;
SteamNetGameAddrs A;
en::RankingService* g_ranking;
SteamNetAccount (*g_accountFn)();
SteamNetAccount g_account;    // Steam account the game logs in with (set in our connect())
std::string g_previousLogin;  // the game's own login it replaces
uint32_t* g_pendingLogin;     // login string waiting for the hello reply
uint8_t* g_loginClient;       // the client object connecting to SteamNet
void InstallSteamLogin();
char g_gameDir[MAX_PATH];

typedef FARPROC(WINAPI* GetProcAddressFn)(HMODULE, LPCSTR);
typedef HANDLE(WINAPI* AsyncGetHostByNameFn)(HWND, u_int, const char*, char*, int);
typedef int(WINAPI* ConnectFn)(SOCKET, const sockaddr*, int);

GetProcAddressFn g_realGetProcAddress;
AsyncGetHostByNameFn g_realGetHostByName;
ConnectFn g_realConnect;

CRITICAL_SECTION g_cs;
SOCKET g_listen = INVALID_SOCKET;
uint16_t g_serverPort;          // our listener
uint16_t g_earthNetPort = 17171; // HKCU ...\Network\EarthNet\Port
LONG g_pendingRedirects;

const char kEarthNetKey[] = "Software\\Reality Pump\\KnightShift\\BaseGame\\Network\\EarthNet";
const char kDeadServer[] = "netserver.earthnet.de";

std::string Narrow(const wchar_t* w) {
    char buf[64];
    int n = WideCharToMultiByte(CP_ACP, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

// ---------------------------------------------------------------------------
// Menu entry: L"EarthNet" -> S.name (same 8-character slot)
// ---------------------------------------------------------------------------
bool Rename() {
    static const wchar_t kOld[9] = L"EarthNet";
    if (!Match(A.nameString, (const uint8_t*)kOld, sizeof(kOld))) return false;
    wchar_t name[9] = {0};
    wcsncpy(name, S.name, 8);
    return WriteCode(A.nameString, name, sizeof(name));
}

// ---------------------------------------------------------------------------
// Server list: AddressIP = "name""address""name""address"...
// The dead EarthNet entry becomes ours; entries the player added stay.
// ---------------------------------------------------------------------------
void FixServerList() {
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kEarthNetKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        Log("steamnet: cannot open HKCU\\%s", kEarthNetKey);
        return;
    }
    DWORD port = 0, size = sizeof(port), type = 0;
    if (RegQueryValueExA(key, "Port", nullptr, &type, (BYTE*)&port, &size) == ERROR_SUCCESS && type == REG_DWORD &&
        port > 0 && port < 65536)
        g_earthNetPort = (uint16_t)port;

    char list[4096] = {0};
    size = sizeof(list) - 1;
    type = 0;
    if (RegQueryValueExA(key, "AddressIP", nullptr, &type, (BYTE*)list, &size) != ERROR_SUCCESS || type != REG_SZ)
        list[0] = 0;
    std::vector<std::string> items;
    for (const char* p = list; (p = strchr(p, '"')) != nullptr;) {
        const char* e = strchr(p + 1, '"');
        if (!e) break;
        items.push_back(std::string(p + 1, e));
        p = e + 1;
    }
    if (items.size() % 2) items.pop_back();
    std::string name = Narrow(S.name);
    bool have = false, changed = false;
    for (size_t i = 0; i + 1 < items.size(); i += 2) {
        if (_stricmp(items[i + 1].c_str(), kDeadServer) == 0) {
            items[i] = name;
            items[i + 1] = S.address;
            changed = true;
        }
        if (_stricmp(items[i + 1].c_str(), S.address) == 0) {
            have = true;
            if (items[i] != name) {
                items[i] = name;
                changed = true;
            }
        }
    }
    if (!have) {
        items.insert(items.begin(), {name, S.address});
        changed = true;
    }
    if (changed) {
        std::string out;
        for (auto& s : items) out += "\"" + s + "\"";
        LONG r = RegSetValueExA(key, "AddressIP", 0, REG_SZ, (const BYTE*)out.c_str(), (DWORD)out.size() + 1);
        Log("steamnet: server list -> %s (%s)", out.c_str(), r == ERROR_SUCCESS ? "ok" : "FAILED");
    }
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// Player data (RPG heroes): <game>\SteamNet\<nick>_<key>.dat
// ---------------------------------------------------------------------------
class FileStore : public en::PlayerStore {
  public:
    std::string Path(const std::string& nick, const std::string& key) {
        std::string f = nick + "_" + key;
        for (char& c : f)
            if (strchr("\\/:*?\"<>|", c) || (unsigned char)c < 32) c = '_';
        std::string dir = std::string(g_gameDir) + "SteamNet";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\" + f + ".dat";
    }
    bool Load(const std::string& nick, const std::string& key, std::vector<uint8_t>& data) override {
        FILE* f = fopen(Path(nick, key).c_str(), "rb");
        if (!f) return false;
        uint8_t buf[4096];
        size_t n;
        data.clear();
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
        fclose(f);
        return true;
    }
    void Save(const std::string& nick, const std::string& key, const std::vector<uint8_t>& data) override {
        std::string p = Path(nick, key), tmp = p + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return;
        bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = fclose(f) == 0 && ok;
        if (ok) MoveFileExA(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
};

// ---------------------------------------------------------------------------
// Local server
// ---------------------------------------------------------------------------
std::string Printable(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && s.size() < 400; i++) {
        uint8_t c = p[i];
        if (c >= 32 && c < 127)
            s += (char)c;
        else {
            char h[8];
            sprintf(h, "\\x%02X", c);
            s += h;
        }
    }
    return s;
}

DWORD WINAPI ConnectionThread(void* param) {
    SOCKET c = (SOCKET)(uintptr_t)param;
    en::SessionConfig cfg;
    cfg.welcome = S.welcome;
    cfg.channel = S.channel;
    cfg.identity = (uint64_t)G<uint32_t>(A.identity) | (uint64_t)G<uint32_t>(A.identity + 4) << 32;
    EnterCriticalSection(&g_cs);
    cfg.accountNick = g_account.name;
    if (g_account.id) cfg.accountKey = "steam_" + std::to_string(g_account.id);
    cfg.previousLogin = g_previousLogin;
    LeaveCriticalSection(&g_cs);
    cfg.beforeHello = [] { InstallSteamLogin(); };
    std::unique_ptr<en::Backend> backendPtr;
    if (g_ranking && S.ranking)
        backendPtr.reset(new en::RankedBackend(*g_ranking, [] { return (int64_t)time(nullptr); }));
    else
        backendPtr.reset(new en::LocalBackend);
    en::Backend& backend = *backendPtr;
    FileStore store;
    auto send_ = [c](const uint8_t* p, size_t n) {
        if (S.trace) Log("steamnet: >> %s", Printable(p, n).c_str());
        while (n > 0) {
            int k = send(c, (const char*)p, (int)n, 0);
            if (k <= 0) return;
            p += k;
            n -= (size_t)k;
        }
    };
    auto log = [](const char* t) { Log("steamnet: %s", t); };
    en::Session session(cfg, backend, store, send_, log);
    Log("steamnet: game connected");
    char buf[4096];
    for (;;) {
        int k = recv(c, buf, sizeof(buf), 0);
        if (k <= 0) break;
        if (S.trace) Log("steamnet: << %s", Printable((const uint8_t*)buf, (size_t)k).c_str());
        if (!session.OnData((const uint8_t*)buf, (size_t)k)) break;
    }
    session.OnClosed();
    closesocket(c);
    Log("steamnet: game disconnected");
    return 0;
}

DWORD WINAPI ListenThread(void*) {
    for (;;) {
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            Log("steamnet: accept failed (%d)", WSAGetLastError());
            return 0;
        }
        HANDLE t = CreateThread(nullptr, 0, ConnectionThread, (void*)(uintptr_t)c, 0, nullptr);
        if (t)
            CloseHandle(t);
        else
            closesocket(c);
    }
}

bool EnsureServer() {
    EnterCriticalSection(&g_cs);
    if (g_listen == INVALID_SOCKET) {
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = sizeof(a);
        if (s != INVALID_SOCKET && bind(s, (sockaddr*)&a, sizeof(a)) == 0 && listen(s, 4) == 0 &&
            getsockname(s, (sockaddr*)&a, &len) == 0) {
            HANDLE t = CreateThread(nullptr, 0, ListenThread, nullptr, 0, nullptr);
            if (t) {
                CloseHandle(t);
                g_listen = s;
                g_serverPort = ntohs(a.sin_port);
                Log("steamnet: server listening on 127.0.0.1:%u", g_serverPort);
            }
        }
        if (g_listen == INVALID_SOCKET) {
            Log("steamnet: cannot start the local server (%d)", WSAGetLastError());
            if (s != INVALID_SOCKET) closesocket(s);
        }
    }
    bool ok = g_listen != INVALID_SOCKET;
    LeaveCriticalSection(&g_cs);
    return ok;
}

// ---------------------------------------------------------------------------
// winsock wrappers handed to the game
// ---------------------------------------------------------------------------
bool IsOurHost(const char* name) {
    return name && (_stricmp(name, S.address) == 0 || _stricmp(name, kDeadServer) == 0 || _stricmp(name, "steamnet") == 0);
}

// Logging in with the Steam account. The client builds its login packet (0x803110) from the
// string at +0x4A80 as soon as it gets the server's hello, so that is when we swap in the Steam
// name (InstallSteamLogin, on the SteamNet connection thread). Earlier is too early: 0x7FF8D0 stores
// the login first, and on the "fastest server" path it runs again after the lookup and after our
// connect(), writing the profile's login back (2.5.6, 2.5.7). The string itself is made here, in our
// connect() on the game thread, with the game's allocator. The game keeps the name in its profile.
// The client object that connects. Not always the global one: the login result (0x81F9C0) makes
// the connecting object the global only when it succeeds ("fastest server" works on its own
// object). 0x7FF8D0 passes a buffer inside the object to the host lookup.
void CaptureClient(char* lookupBuf) {
    uint8_t* c = nullptr;
    if (A.lookupBuffer && A.clientVtable && lookupBuf) {
        c = (uint8_t*)lookupBuf - A.lookupBuffer;
        if (IsBadReadPtr(c, 4) || G<uint32_t>((uint32_t)(uintptr_t)c) != A.clientVtable) c = nullptr;
    }
    EnterCriticalSection(&g_cs);
    g_loginClient = c;
    LeaveCriticalSection(&g_cs);
}

uint8_t* LoginClient() {
    EnterCriticalSection(&g_cs);
    uint8_t* c = g_loginClient;
    LeaveCriticalSection(&g_cs);
    return c ? c : A.client ? G<uint8_t*>(A.client) : nullptr;
}

void PrepareSteamAccount() {
    SteamNetAccount acc;
    if (S.steamLogin && g_accountFn && A.client && A.memAlloc) acc = g_accountFn();
    if (acc.name.empty()) {
        if (S.steamLogin && g_accountFn) Log("steamnet: Steam account not available - the game's own login is used");
        EnterCriticalSection(&g_cs);
        g_account = SteamNetAccount();
        g_previousLogin.clear();
        g_pendingLogin = nullptr;
        LeaveCriticalSection(&g_cs);
        return;
    }
    std::string previous;
    uint8_t* client = LoginClient();
    uint32_t* cur = client ? G<uint32_t*>((uint32_t)(uintptr_t)(client + 0x4A80)) : nullptr;
    if (cur && cur[2] > 0 && cur[2] < 64) {
        char a[256] = {0};
        WideCharToMultiByte(CP_ACP, 0, (const wchar_t*)(cur + 3), (int)cur[2], a, sizeof(a) - 1, "?", nullptr);
        previous = a;
    }
    wchar_t w[64];
    int n = MultiByteToWideChar(CP_ACP, 0, acc.name.c_str(), (int)acc.name.size(), w, 63);
    uint32_t* str = nullptr;
    if (n > 0) {
        // wide string object: refcount, capacity, length, characters, 0 (as 0x7FF8D0 builds it)
        typedef void*(__cdecl * AllocFn)(size_t);
        str = (uint32_t*)((AllocFn)(uintptr_t)A.memAlloc)(12 + 2 * (n + 1));
        if (str) {
            str[0] = 1;
            str[1] = (uint32_t)n;
            str[2] = (uint32_t)n;
            memcpy(str + 3, w, 2 * n);
            ((wchar_t*)(str + 3))[n] = 0;
        }
    }
    EnterCriticalSection(&g_cs);
    g_account = acc;
    g_previousLogin = previous;
    g_pendingLogin = str; // an unused one from an earlier attempt is simply left allocated
    LeaveCriticalSection(&g_cs);
}

// Right before the hello reply: the client is waiting for it and builds the login packet from
// +0x4A80 when it arrives, so nothing touches that string now.
void InstallSteamLogin() {
    EnterCriticalSection(&g_cs);
    uint32_t* str = g_pendingLogin;
    g_pendingLogin = nullptr;
    std::string name = g_account.name;
    LeaveCriticalSection(&g_cs);
    uint8_t* client = LoginClient();
    if (!str || !client) return;
    uint32_t*& login = G<uint32_t*>((uint32_t)(uintptr_t)(client + 0x4A80));
    uint32_t* old = login;
    char was[128] = {0};
    if (old && old[2] > 0 && old[2] < 64)
        WideCharToMultiByte(CP_ACP, 0, (const wchar_t*)(old + 3), (int)old[2], was, sizeof(was) - 1, "?", nullptr);
    login = str;
    if (old && old != str) old[0]--; // stays allocated if someone still holds it; a tiny leak otherwise
    Log("steamnet: logging in as the Steam account \"%s\" (the game had \"%s\")", name.c_str(), was);
}

HANDLE WINAPI HookGetHostByName(HWND wnd, u_int msg, const char* name, char* buf, int buflen) {
    if (IsOurHost(name) && EnsureServer()) {
        CaptureClient(buf);
        InterlockedIncrement(&g_pendingRedirects);
        Log("steamnet: \"%s\" -> local server", name);
        return g_realGetHostByName(wnd, msg, "127.0.0.1", buf, buflen);
    }
    return g_realGetHostByName(wnd, msg, name, buf, buflen);
}

int WINAPI HookConnect(SOCKET s, const sockaddr* addr, int len) {
    if (addr && len >= (int)sizeof(sockaddr_in) && addr->sa_family == AF_INET && g_listen != INVALID_SOCKET) {
        sockaddr_in a = *(const sockaddr_in*)addr;
        uint16_t port = ntohs(a.sin_port);
        if (a.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && (port == g_earthNetPort || port == g_earthNetPort + 10)) {
            LONG left = InterlockedDecrement(&g_pendingRedirects);
            if (left >= 0) {
                PrepareSteamAccount();
                a.sin_port = htons(g_serverPort);
                return g_realConnect(s, (const sockaddr*)&a, sizeof(a));
            }
            InterlockedIncrement(&g_pendingRedirects);
        }
    }
    return g_realConnect(s, addr, len);
}

FARPROC WINAPI HookGetProcAddress(HMODULE mod, LPCSTR name) {
    FARPROC f = g_realGetProcAddress(mod, name);
    if (!f || IS_INTRESOURCE(name) || mod != GetModuleHandleA("ws2_32.dll")) return f;
    if (strcmp(name, "WSAAsyncGetHostByName") == 0) {
        g_realGetHostByName = (AsyncGetHostByNameFn)f;
        FixServerList(); // ws2_32 is loaded once at start-up (0x6FD3B0)
        return (FARPROC)&HookGetHostByName;
    }
    if (strcmp(name, "connect") == 0) {
        g_realConnect = (ConnectFn)f;
        return (FARPROC)&HookConnect;
    }
    return f;
}

void ReadIniString(const char* ini, const char* key, char* out, DWORD size) {
    char def[256];
    strncpy(def, out, sizeof(def) - 1);
    def[sizeof(def) - 1] = 0;
    GetPrivateProfileStringA("SteamNet", key, def, out, size, ini);
}

} // namespace

void SteamNet_LoadConfig(const char* ini, SteamNetSettings& s) {
    s.enabled = GetPrivateProfileIntA("SteamNet", "Enabled", 1, ini) != 0;
    char name[32];
    GetPrivateProfileStringA("SteamNet", "Name", "SteamNet", name, sizeof(name), ini);
    wchar_t w[32] = {0};
    MultiByteToWideChar(CP_ACP, 0, name, -1, w, 32);
    memset(s.name, 0, sizeof(s.name));
    wcsncpy(s.name, w[0] ? w : L"SteamNet", 8);
    ReadIniString(ini, "Address", s.address, sizeof(s.address));
    ReadIniString(ini, "Channel", s.channel, sizeof(s.channel));
    ReadIniString(ini, "Welcome", s.welcome, sizeof(s.welcome));
    s.trace = GetPrivateProfileIntA("SteamNet", "Trace", 0, ini) != 0;
    s.ranking = GetPrivateProfileIntA("SteamNet", "Ranking", 1, ini) != 0;
    s.steamLogin = GetPrivateProfileIntA("SteamNet", "SteamLogin", 1, ini) != 0;
}

void SteamNet_SetRanking(en::RankingService* r) { g_ranking = r; }
void SteamNet_SetAccount(SteamNetAccount (*account)()) { g_accountFn = account; }

bool SteamNet_Install(const SteamNetSettings& s, const SteamNetGameAddrs& game) {
    S = s;
    A = game;
    InitializeCriticalSection(&g_cs);
    GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH);
    char* slash = strrchr(g_gameDir, '\\');
    if (slash) slash[1] = 0;

    bool renamed = Rename();
    Log("steamnet: menu entry \"%s\": %s", Narrow(S.name).c_str(), renamed ? "ok" : "signature mismatch");
    void** slot = FindImport(GetModuleHandleA(nullptr), "kernel32.dll", "GetProcAddress");
    if (!slot) return false;
    g_realGetProcAddress = (GetProcAddressFn)*slot;
    void* hook = (void*)&HookGetProcAddress;
    return WriteCode((uint32_t)(uintptr_t)slot, &hook, sizeof(hook));
}
