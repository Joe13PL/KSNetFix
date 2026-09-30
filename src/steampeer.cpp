// KSNetFix Steam transport: an IDirectPlay8Peer implementation that carries the
// game's DirectPlay 8 session over Steam (lobbies for discovery, Steam
// Networking Messages over the Steam Datagram Relay for traffic). No ports to
// open, no VPN. Selected when the player picks the TCP/IP provider and
// [Steam] Enabled=1; otherwise the real DirectPlay is used untouched.
//
// Written from the DirectX 8/9 documentation of IDirectPlay8Peer and from the
// reverse-engineered usage in KnightShift (docs/NETCODE.md).

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "steam/steam_api.h"
#include "steam/steam_gameserver.h"
#include "steam/isteamnetworkingmessages.h"
#include "steam/isteamnetworkingutils.h"

#include "dp8.h"
#include "earthnet_core.h"
#include "steampeer.h"

extern void Log(const char* fmt, ...);

namespace {

const int kChannel = 7;
const char* kProtoKey = "ksnf";
const char* kProtoVer = "1";
const DWORD kConnectRetryMs = 1500, kConnectTimeoutMs = 20000, kPeerTimeoutMs = 25000, kPingMs = 1000;

SteamSettings S;
bool g_gs = false; // anonymous game-server mode active
static ISteamNetworkingMessages* NM() { return g_gs ? SteamGameServerNetworkingMessages() : SteamNetworkingMessages(); }
static void RunCB() { if (g_gs) SteamGameServer_RunCallbacks(); else SteamAPI_RunCallbacks(); }
HCoCreate g_origCoCreate;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
struct Lock {
    CRITICAL_SECTION* cs;
    explicit Lock(CRITICAL_SECTION* c) : cs(c) { EnterCriticalSection(cs); }
    ~Lock() { LeaveCriticalSection(cs); }
};

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring Widen(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}
std::string Hex(const void* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = ((const uint8_t*)p)[i];
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}
std::vector<uint8_t> Unhex(const char* s) {
    std::vector<uint8_t> v;
    if (!s) return v;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (; s[0] && s[1]; s += 2) {
        int a = nib(s[0]), b = nib(s[1]);
        if (a < 0 || b < 0) break;
        v.push_back((uint8_t)(a << 4 | b));
    }
    return v;
}

// binary writer / reader for our control messages
struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void guid(const GUID& g) { raw(&g, sizeof(g)); }
    void raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void blob(const std::vector<uint8_t>& v) { u32((uint32_t)v.size()); if (!v.empty()) raw(v.data(), v.size()); }
    void wstr(const std::wstring& s) { u32((uint32_t)s.size()); if (!s.empty()) raw(s.data(), s.size() * 2); }
};
struct Reader {
    const uint8_t* p;
    size_t n, i = 0;
    bool ok = true;
    Reader(const void* d, size_t sz) : p((const uint8_t*)d), n(sz) {}
    bool need(size_t k) { if (i + k > n) ok = false; return ok; }
    uint8_t u8() { return need(1) ? p[i++] : 0; }
    uint32_t u32() { uint32_t v = 0; if (need(4)) { memcpy(&v, p + i, 4); i += 4; } return v; }
    uint64_t u64() { uint64_t v = 0; if (need(8)) { memcpy(&v, p + i, 8); i += 8; } return v; }
    GUID guid() { GUID g{}; if (need(sizeof(g))) { memcpy(&g, p + i, sizeof(g)); i += sizeof(g); } return g; }
    std::vector<uint8_t> blob() {
        uint32_t k = u32();
        std::vector<uint8_t> v;
        if (k > (1u << 24) || !need(k)) return v;
        v.assign(p + i, p + i + k);
        i += k;
        return v;
    }
    std::wstring wstr() {
        uint32_t k = u32();
        std::wstring s;
        if (k > 4096 || !need(k * 2)) return s;
        s.assign((const wchar_t*)(p + i), k);
        i += k * 2;
        return s;
    }
};

enum MsgType : uint8_t {
    T_CONNECT_REQ = 1, T_CONNECT_ACK, T_CONNECT_NAK, T_PLAYER_ADD, T_PLAYER_DEL, T_DATA,
    T_PEER_INFO, T_TERMINATE, T_LEAVE, T_APPDESC, T_PING,
};

struct AppDesc {
    DWORD flags = 0, maxPlayers = 0;
    GUID inst{}, app{};
    std::wstring name, password;
    std::vector<uint8_t> resv, appResv;

    void From(const DPN_APPLICATION_DESC_* d) {
        flags = d->dwFlags;
        inst = d->guidInstance;
        app = d->guidApplication;
        maxPlayers = d->dwMaxPlayers;
        name = d->pwszSessionName ? d->pwszSessionName : L"";
        password = d->pwszPassword ? d->pwszPassword : L"";
        resv.assign((uint8_t*)d->pvReservedData, (uint8_t*)d->pvReservedData + (d->pvReservedData ? d->dwReservedDataSize : 0));
        appResv.assign((uint8_t*)d->pvApplicationReservedData,
                       (uint8_t*)d->pvApplicationReservedData +
                           (d->pvApplicationReservedData ? d->dwApplicationReservedDataSize : 0));
    }
    void Write(Writer& w) const {
        w.u32(flags); w.guid(inst); w.guid(app); w.u32(maxPlayers); w.wstr(name); w.blob(resv); w.blob(appResv);
    }
    void Read(Reader& r) {
        flags = r.u32(); inst = r.guid(); app = r.guid(); maxPlayers = r.u32(); name = r.wstr(); resv = r.blob();
        appResv = r.blob();
    }
    // Serialise into caller memory in the DirectPlay "struct followed by data" layout.
    size_t Size(bool withPassword) const {
        return sizeof(DPN_APPLICATION_DESC_) + (name.size() + 1) * 2 + (withPassword && !password.empty() ? (password.size() + 1) * 2 : 0) +
               resv.size() + appResv.size();
    }
    void Fill(DPN_APPLICATION_DESC_* d, DWORD curPlayers, bool withPassword) const {
        uint8_t* p = (uint8_t*)(d + 1);
        d->dwSize = sizeof(DPN_APPLICATION_DESC_);
        d->dwFlags = flags;
        d->guidInstance = inst;
        d->guidApplication = app;
        d->dwMaxPlayers = maxPlayers;
        d->dwCurrentPlayers = curPlayers;
        d->pwszSessionName = (WCHAR*)p;
        memcpy(p, name.c_str(), (name.size() + 1) * 2);
        p += (name.size() + 1) * 2;
        d->pwszPassword = nullptr;
        if (withPassword && !password.empty()) {
            d->pwszPassword = (WCHAR*)p;
            memcpy(p, password.c_str(), (password.size() + 1) * 2);
            p += (password.size() + 1) * 2;
        }
        d->pvReservedData = resv.empty() ? nullptr : p;
        d->dwReservedDataSize = (DWORD)resv.size();
        if (!resv.empty()) memcpy(p, resv.data(), resv.size());
        p += resv.size();
        d->pvApplicationReservedData = appResv.empty() ? nullptr : p;
        d->dwApplicationReservedDataSize = (DWORD)appResv.size();
        if (!appResv.empty()) memcpy(p, appResv.data(), appResv.size());
    }
};

struct Player {
    DPNID id = 0;
    uint64_t steam = 0;
    std::wstring name;
    std::vector<uint8_t> data;
    bool local = false, host = false;
    void* ctx = nullptr;
    DWORD lastRecv = 0;
};

IDirectPlay8Address_* NewAddress(const std::wstring& hostname) {
    IDirectPlay8Address_* a = nullptr;
    HRESULT hr = g_origCoCreate(CLSID_DirectPlay8Address_, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectPlay8Address_, (void**)&a);
    if (FAILED(hr) || !a) {
        Log("steam: cannot create a DirectPlay address object (hr=%08X) - is the DirectPlay Windows feature installed?", (unsigned)hr);
        return nullptr;
    }
    a->SetSP(&CLSID_DP8SP_TCPIP_);
    a->SetDevice(&GUID_KSSteamDevice);
    a->AddComponent(L"hostname", hostname.c_str(), (DWORD)((hostname.size() + 1) * 2), DPNA_DATATYPE_STRING_);
    DWORD port = 0;
    a->AddComponent(L"port", &port, 4, DPNA_DATATYPE_DWORD_);
    return a;
}

std::wstring AddressHost(IDirectPlay8Address_* a) {
    if (!a) return std::wstring();
    WCHAR buf[260] = {0};
    DWORD size = sizeof(buf), type = 0;
    if (FAILED(a->GetComponentByName(L"hostname", buf, &size, &type))) return std::wstring();
    buf[259] = 0;
    return buf;
}

// "ks-lobby:<id>", "steam:<id>" or a bare 17-digit Steam/lobby id typed as IP.
uint64_t ParseSteamTarget(const std::wstring& h) {
    const wchar_t* s = h.c_str();
    if (!_wcsnicmp(s, L"ks-lobby:", 9)) s += 9;
    else if (!_wcsnicmp(s, L"steam:", 6)) s += 6;
    if (!*s) return 0;
    for (const wchar_t* q = s; *q; q++)
        if (*q < L'0' || *q > L'9') return 0;
    return _wcstoui64(s, nullptr, 10);
}

class SteamPeer;

// ---------------------------------------------------------------------------
// Steam service: owns the Steam thread, callbacks and the active peer
// ---------------------------------------------------------------------------
class SteamService {
  public:
    CRITICAL_SECTION cs;       // session state
    CRITICAL_SECTION sendCs;   // SendMessageToUser
    std::deque<std::function<void()>> events;
    SteamPeer* peer = nullptr; // active peer (the game uses one at a time)
    uint64_t me = 0;
    std::wstring myName;
    uint64_t inviteLobby = 0;

    SteamService() {
        InitializeCriticalSection(&cs);
        InitializeCriticalSection(&sendCs);
    }

    void Post(std::function<void()> f) {
        Lock l(&cs);
        events.push_back(std::move(f));
    }

    bool Send(uint64_t to, const std::vector<uint8_t>& m, bool reliable) {
        SteamNetworkingIdentity id;
        id.SetSteamID64(to);
        int fl = (reliable ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle) |
                 k_nSteamNetworkingSend_AutoRestartBrokenSession;
        Lock l(&sendCs);
        EResult r = NM()->SendMessageToUser(id, m.data(), (uint32)m.size(), fl, kChannel);
        return r == k_EResultOK;
    }

    void Run();

    // --- callbacks (dispatched by SteamAPI_RunCallbacks on the Steam thread)
    STEAM_CALLBACK(SteamService, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t);
    STEAM_CALLBACK(SteamService, OnSessionFailed, SteamNetworkingMessagesSessionFailed_t);
    STEAM_CALLBACK(SteamService, OnLobbyJoinRequested, GameLobbyJoinRequested_t);
    STEAM_CALLBACK(SteamService, OnLobbyDataUpdate, LobbyDataUpdate_t);

    CCallResult<SteamService, LobbyCreated_t> crCreate;
    CCallResult<SteamService, LobbyEnter_t> crEnter;
    CCallResult<SteamService, LobbyMatchList_t> crList;
    CCallback<SteamService, SteamNetworkingMessagesSessionRequest_t, true>* gsReq = nullptr;
    CCallback<SteamService, SteamNetworkingMessagesSessionFailed_t, true>* gsFail = nullptr;
    void OnLobbyCreated(LobbyCreated_t* r, bool io);
    void OnLobbyEnter(LobbyEnter_t* r, bool io);
    void OnLobbyList(LobbyMatchList_t* r, bool io);
};

SteamService* g_svc;

// ---------------------------------------------------------------------------
// The IDirectPlay8Peer replacement
// ---------------------------------------------------------------------------
enum class St { Uninit, Idle, Hosting, Connecting, Connected };

class SteamPeer : public IDirectPlay8Peer_ {
  public:
    LONG refs = 1;
    St st = St::Uninit;
    uint32_t epoch = 1;
    PFNDPNMESSAGEHANDLER pfn = nullptr;
    void* userCtx = nullptr;
    DWORD nextHandle = 0x4B530001;
    DWORD nextId = 0x00100001;

    // local identity
    std::wstring myName;
    std::vector<uint8_t> myData;

    // session
    AppDesc desc;
    std::map<DPNID, Player> players;
    DPNID localId = 0, hostId = 0;
    uint64_t hostSteam = 0, lobby = 0;
    std::map<DPNHANDLE, std::vector<uint8_t>*> buffers;

    // connect op
    DPNHANDLE hConnect = 0;
    void* connectCtx = nullptr;
    void* connectPlayerCtx = nullptr;
    std::vector<uint8_t> connectData;
    std::wstring connectPassword;
    DWORD connectStart = 0, connectLastReq = 0;
    uint64_t connectTarget = 0; // lobby or user

    // enum op
    bool enumActive = false, enumDirected = false;
    DPNHANDLE hEnum = 0;
    void* enumCtx = nullptr;
    GUID enumApp{};
    uint64_t enumTarget = 0, enumLobby = 0; // directed enum: only this lobby may answer
    DWORD enumStart = 0, enumTimeout = 0, enumLastQuery = 0;
    IDirectPlay8Address_* enumDevice = nullptr;

    DWORD lastPing = 0;
    bool locPublished = false;

    DPNHANDLE NewHandle() { return nextHandle++; }

    // deliver to the game; never with the service lock held
    HRESULT Deliver(DWORD msg, void* data) { return pfn ? pfn(userCtx, msg, data) : S_OK; }

    // Post an event that is dropped if the session changes before it runs.
    void PostSession(std::function<void()> f) {
        uint32_t e = epoch;
        g_svc->Post([this, e, f]() {
            {
                Lock l(&g_svc->cs);
                if (e != epoch || st == St::Uninit) return;
            }
            f();
        });
    }

    Player* Find(DPNID id) {
        auto it = players.find(id);
        return it == players.end() ? nullptr : &it->second;
    }
    Player* FindSteam(uint64_t s) {
        for (auto& kv : players)
            if (kv.second.steam == s && !kv.second.local) return &kv.second;
        return nullptr;
    }

    void DeliverReceive(DPNID from, void* ctx, const uint8_t* d, size_t n) {
        auto* buf = new std::vector<uint8_t>(d, d + n);
        DPNHANDLE h;
        {
            Lock l(&g_svc->cs);
            h = NewHandle();
            buffers[h] = buf;
        }
        DPNMSG_RECEIVE_ m{sizeof(m), from, ctx, buf->data(), (DWORD)buf->size(), h};
        HRESULT hr = Deliver(DPN_MSGID_RECEIVE_, &m);
        if (hr != DPNSUCCESS_PENDING_) {
            Lock l(&g_svc->cs);
            auto it = buffers.find(h);
            if (it != buffers.end()) {
                delete it->second;
                buffers.erase(it);
            }
        }
    }

    void DeliverCreate(DPNID id) {
        void* ctx;
        {
            Lock l(&g_svc->cs);
            Player* p = Find(id);
            if (!p) return;
            ctx = p->ctx;
        }
        DPNMSG_CREATE_PLAYER_ m{sizeof(m), id, ctx};
        Deliver(DPN_MSGID_CREATE_PLAYER_, &m);
        Lock l(&g_svc->cs);
        if (Player* p = Find(id)) p->ctx = m.pvPlayerContext;
    }

    void DeliverDestroy(DPNID id, DWORD reason) {
        void* ctx;
        {
            Lock l(&g_svc->cs);
            Player* p = Find(id);
            if (!p) return;
            ctx = p->ctx;
        }
        DPNMSG_DESTROY_PLAYER_ m{sizeof(m), id, ctx, reason};
        Deliver(DPN_MSGID_DESTROY_PLAYER_, &m);
        Lock l(&g_svc->cs);
        players.erase(id);
    }

    std::vector<uint8_t> Msg(MsgType t, const Writer& body) {
        std::vector<uint8_t> m;
        m.reserve(4 + body.b.size());
        m.push_back(t);
        m.push_back(1);
        m.push_back(0);
        m.push_back(0);
        m.insert(m.end(), body.b.begin(), body.b.end());
        return m;
    }

    void UpdateLobbyCount() {
        if (!lobby || st != St::Hosting) return;
        char b[16];
        sprintf(b, "%u", (unsigned)players.size());
        SteamMatchmaking()->SetLobbyData(CSteamID(lobby), "curp", b);
    }

    void PublishLobby() {
        if (!lobby) return;
        CSteamID L(lobby);
        auto* mm = SteamMatchmaking();
        char b[64];
        mm->SetLobbyData(L, kProtoKey, kProtoVer);
        mm->SetLobbyData(L, "name", Narrow(desc.name).c_str());
        mm->SetLobbyData(L, "app", Hex(&desc.app, sizeof(GUID)).c_str());
        mm->SetLobbyData(L, "inst", Hex(&desc.inst, sizeof(GUID)).c_str());
        sprintf(b, "%u", (unsigned)desc.maxPlayers);
        mm->SetLobbyData(L, "maxp", b);
        sprintf(b, "%u", (unsigned)desc.flags);
        mm->SetLobbyData(L, "flags", b);
        mm->SetLobbyData(L, "resv", Hex(desc.resv.data(), desc.resv.size()).c_str());
        mm->SetLobbyData(L, "aresv", Hex(desc.appResv.data(), desc.appResv.size()).c_str());
        sprintf(b, "%llu", (unsigned long long)g_svc->me);
        mm->SetLobbyData(L, "host", b);
        SteamNetworkPingLocation_t loc;
        if (SteamNetworkingUtils()->GetLocalPingLocation(loc) >= 0) {
            char ls[k_cchMaxSteamNetworkingPingLocationString];
            SteamNetworkingUtils()->ConvertPingLocationToString(loc, ls, sizeof(ls));
            mm->SetLobbyData(L, "loc", ls);
            locPublished = true;
        }
        UpdateLobbyCount();
    }

    // ------------------------------------------------------------ COM
    HRESULT __stdcall QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectPlay8Peer_)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG __stdcall Release() override {
        LONG r = InterlockedDecrement(&refs);
        if (r == 0) {
            if (st != St::Uninit) Close(0);
            Lock l(&g_svc->cs);
            if (g_svc->peer == this) g_svc->peer = nullptr;
            // Kept alive on purpose: queued Steam-thread events may still reference it.
        }
        return (ULONG)r;
    }

    // ------------------------------------------------------------ setup
    HRESULT __stdcall Initialize(PVOID ctx, PFNDPNMESSAGEHANDLER f, DWORD) override {
        Lock l(&g_svc->cs);
        if (st != St::Uninit) return DPNERR_ALREADYINITIALIZED_;
        if (!f) return E_INVALIDARG;
        userCtx = ctx;
        pfn = f;
        st = St::Idle;
        epoch++;
        g_svc->peer = this;
        return S_OK;
    }

    HRESULT __stdcall EnumServiceProviders(const GUID* sp, const GUID*, DPN_SERVICE_PROVIDER_INFO_* buf, PDWORD cb,
                                           PDWORD count, DWORD) override {
        if (!cb || !count) return E_POINTER;
        const wchar_t* name;
        GUID g;
        if (!sp) {
            name = L"Steam (KSNetFix)";
            g = CLSID_DP8SP_TCPIP_;
        } else if (IsEqualGUID(*sp, CLSID_DP8SP_TCPIP_)) {
            name = L"Steam Datagram Relay";
            g = GUID_KSSteamDevice;
        } else {
            *count = 0;
            *cb = 0;
            return S_OK; // no serial / modem / IPX
        }
        DWORD need = sizeof(DPN_SERVICE_PROVIDER_INFO_) + (DWORD)(wcslen(name) + 1) * 2;
        if (!buf || *cb < need) {
            *cb = need;
            *count = 1;
            return DPNERR_BUFFERTOOSMALL_;
        }
        memset(buf, 0, sizeof(*buf));
        buf->guid = g;
        buf->pwszName = (WCHAR*)(buf + 1);
        wcscpy(buf->pwszName, name);
        *count = 1;
        return S_OK;
    }

    HRESULT __stdcall CancelAsyncOperation(DPNHANDLE h, DWORD flags) override {
        bool doEnum = false, doConnect = false;
        DPNHANDLE he = 0, hc = 0;
        void *ce = nullptr, *cc = nullptr;
        {
            Lock l(&g_svc->cs);
            if (enumActive && (h == hEnum || (!h && (flags & (DPNCANCEL_ENUM_ | DPNCANCEL_ALL_OPERATIONS_))))) {
                doEnum = true;
                enumActive = false;
                he = hEnum;
                ce = enumCtx;
            }
            if (st == St::Connecting && (h == hConnect || (!h && (flags & (DPNCANCEL_CONNECT_ | DPNCANCEL_ALL_OPERATIONS_))))) {
                doConnect = true;
                hc = hConnect;
                cc = connectCtx;
                st = St::Idle;
                epoch++;
                if (lobby) SteamMatchmaking()->LeaveLobby(CSteamID(lobby));
                lobby = 0;
            }
        }
        if (doEnum) {
            g_svc->Post([this, he, ce]() {
                DPNMSG_ASYNC_OP_COMPLETE_ m{sizeof(m), he, ce, DPNERR_USERCANCEL_};
                Deliver(DPN_MSGID_ASYNC_OP_COMPLETE_, &m);
            });
        }
        if (doConnect) {
            g_svc->Post([this, hc, cc]() {
                DPNMSG_CONNECT_COMPLETE_ m{sizeof(m), hc, cc, DPNERR_USERCANCEL_, nullptr, 0, 0};
                Deliver(DPN_MSGID_CONNECT_COMPLETE_, &m);
            });
        }
        return (doEnum || doConnect || !h) ? S_OK : DPNERR_INVALIDHANDLE_;
    }

    // ------------------------------------------------------------ connect
    HRESULT __stdcall Connect(const DPN_APPLICATION_DESC_* app, IDirectPlay8Address_* host, IDirectPlay8Address_*,
                              const void*, const void*, const void* data, DWORD dataSize, void* playerCtx,
                              void* asyncCtx, DPNHANDLE* h, DWORD) override {
        if (!app || !host) return E_POINTER;
        uint64_t target = ParseSteamTarget(AddressHost(host));
        Lock l(&g_svc->cs);
        if (st == St::Uninit) return DPNERR_UNINITIALIZED_;
        if (st != St::Idle) return DPNERR_ALREADYCONNECTED_;
        if (!target) return DPNERR_NOCONNECTION_;
        desc.From(app);
        connectPassword = desc.password;
        connectData.assign((const uint8_t*)data, (const uint8_t*)data + (data ? dataSize : 0));
        connectCtx = asyncCtx;
        connectPlayerCtx = playerCtx;
        connectTarget = target;
        hConnect = NewHandle();
        if (h) *h = hConnect;
        st = St::Connecting;
        epoch++;
        connectStart = GetTickCount();
        connectLastReq = 0;
        hostSteam = 0;
        lobby = 0;
        Log("steam: connecting to %s %llu", CSteamID(target).IsLobby() ? "lobby" : "user", (unsigned long long)target);
        PostSession([this, target]() {
            if (CSteamID(target).IsLobby()) {
                SteamAPICall_t c = SteamMatchmaking()->JoinLobby(CSteamID(target));
                g_svc->crEnter.Set(c, g_svc, &SteamService::OnLobbyEnter);
            } else {
                Lock l2(&g_svc->cs);
                hostSteam = target; // direct connection to a user
            }
        });
        return DPNSUCCESS_PENDING_;
    }

    void SendConnectRequest() { // cs held
        Writer w;
        w.guid(desc.app);
        w.guid(desc.inst);
        w.wstr(myName);
        w.wstr(connectPassword);
        w.blob(connectData);
        w.blob(myData);
        g_svc->Send(hostSteam, Msg(T_CONNECT_REQ, w), true);
        connectLastReq = GetTickCount();
    }

    void FailConnect(HRESULT hr, std::vector<uint8_t> reply) {
        DPNHANDLE hc;
        void* cc;
        uint64_t L;
        {
            Lock l(&g_svc->cs);
            if (st != St::Connecting) return;
            hc = hConnect;
            cc = connectCtx;
            L = lobby;
            st = St::Idle;
            epoch++;
            lobby = 0;
            players.clear();
        }
        if (L) SteamMatchmaking()->LeaveLobby(CSteamID(L));
        Log("steam: connect failed hr=%08X", (unsigned)hr);
        DPNMSG_CONNECT_COMPLETE_ m{sizeof(m), hc, cc, hr, reply.empty() ? nullptr : reply.data(), (DWORD)reply.size(), 0};
        Deliver(DPN_MSGID_CONNECT_COMPLETE_, &m);
    }

    // ------------------------------------------------------------ data
    HRESULT __stdcall SendTo(DPNID to, const DPN_BUFFER_DESC_* bufs, DWORD n, DWORD, void* ctx, DPNHANDLE* h,
                             DWORD flags) override {
        Writer w;
        std::vector<uint8_t> payload;
        for (DWORD i = 0; i < n; i++)
            if (bufs[i].dwBufferSize) payload.insert(payload.end(), bufs[i].pBufferData, bufs[i].pBufferData + bufs[i].dwBufferSize);
        std::vector<uint64_t> remote;
        bool loop = false;
        DPNID from;
        void* fromCtx = nullptr;
        DPNHANDLE handle = 0;
        {
            Lock l(&g_svc->cs);
            if (st != St::Hosting && st != St::Connected) return DPNERR_NOCONNECTION_;
            from = localId;
            if (Player* me = Find(localId)) fromCtx = me->ctx;
            if (to == 0) {
                for (auto& kv : players)
                    if (!kv.second.local) remote.push_back(kv.second.steam);
                loop = !(flags & DPNSEND_NOLOOPBACK_);
            } else {
                Player* p = Find(to);
                if (!p) return DPNERR_INVALIDPLAYER_;
                if (p->local) loop = true;
                else remote.push_back(p->steam);
            }
            if (!(flags & DPNOP_SYNC_)) handle = NewHandle();
        }
        if (!remote.empty()) {
            w.u32(from);
            w.u32(to);
            w.raw(payload.data(), payload.size());
            auto m = Msg(T_DATA, w);
            bool rel = (flags & DPNSEND_GUARANTEED_) != 0;
            for (uint64_t s : remote) g_svc->Send(s, m, rel);
        }
        if (loop) {
            PostSession([this, from, fromCtx, payload]() { DeliverReceive(from, fromCtx, payload.data(), payload.size()); });
        }
        if (flags & DPNOP_SYNC_) return S_OK;
        if (h) *h = handle;
        if (!(flags & DPNSEND_NOCOMPLETE_)) {
            DWORD t0 = GetTickCount();
            PostSession([this, handle, ctx, t0]() {
                DPNMSG_SEND_COMPLETE_ m{sizeof(m), handle, ctx, S_OK, GetTickCount() - t0, 0, 0};
                Deliver(DPN_MSGID_SEND_COMPLETE_, &m);
            });
        }
        return DPNSUCCESS_PENDING_;
    }

    HRESULT __stdcall GetSendQueueInfo(DPNID, DWORD* msgs, DWORD* bytes, DWORD) override {
        if (msgs) *msgs = 0;
        if (bytes) *bytes = 0;
        return S_OK;
    }

    // ------------------------------------------------------------ host
    HRESULT __stdcall Host(const DPN_APPLICATION_DESC_* app, IDirectPlay8Address_**, DWORD, const void*, const void*,
                           void* playerCtx, DWORD) override {
        if (!app) return E_POINTER;
        DPNID me;
        {
            Lock l(&g_svc->cs);
            if (st == St::Uninit) return DPNERR_UNINITIALIZED_;
            if (st != St::Idle) return DPNERR_ALREADYCONNECTED_;
            desc.From(app);
            if (desc.maxPlayers == 0) desc.maxPlayers = 8;
            CoCreateGuid(&desc.inst);
            players.clear();
            localId = hostId = nextId++;
            Player p;
            p.id = localId;
            p.steam = g_svc->me;
            p.name = myName;
            p.data = myData;
            p.local = p.host = true;
            p.ctx = playerCtx;
            players[localId] = p;
            hostSteam = g_svc->me;
            lobby = 0;
            locPublished = false;
            st = St::Hosting;
            epoch++;
            me = localId;
        }
        Log("steam: hosting \"%s\" (max %u players)", Narrow(desc.name).c_str(), (unsigned)desc.maxPlayers);
        DeliverCreate(me); // DirectPlay indicates the local host player before Host() returns
        if (!g_gs) {
            PostSession([this]() {
                ELobbyType t = S.lobbyFriendsOnly ? k_ELobbyTypeFriendsOnly : k_ELobbyTypePublic;
                SteamAPICall_t c = SteamMatchmaking()->CreateLobby(t, (int)desc.maxPlayers);
                g_svc->crCreate.Set(c, g_svc, &SteamService::OnLobbyCreated);
            });
        } else {
            Log("steam: game server hosting - players join with SteamID %llu", (unsigned long long)g_svc->me);
        }
        return S_OK;
    }

    HRESULT __stdcall GetApplicationDesc(DPN_APPLICATION_DESC_* buf, DWORD* size, DWORD) override {
        if (!size) return E_POINTER;
        Lock l(&g_svc->cs);
        if (st != St::Hosting && st != St::Connected) return DPNERR_NOCONNECTION_;
        bool pw = st == St::Hosting;
        DWORD need = (DWORD)desc.Size(pw);
        if (!buf || *size < need) {
            *size = need;
            return DPNERR_BUFFERTOOSMALL_;
        }
        desc.Fill(buf, (DWORD)players.size(), pw);
        return S_OK;
    }

    HRESULT __stdcall SetApplicationDesc(const DPN_APPLICATION_DESC_* app, DWORD) override {
        if (!app) return E_POINTER;
        std::vector<uint64_t> to;
        std::vector<uint8_t> m;
        {
            Lock l(&g_svc->cs);
            if (st != St::Hosting) return DPNERR_NOTHOST_;
            GUID inst = desc.inst;
            desc.From(app);
            desc.inst = inst;
            PublishLobby();
            Writer w;
            desc.Write(w);
            m = Msg(T_APPDESC, w);
            for (auto& kv : players)
                if (!kv.second.local) to.push_back(kv.second.steam);
        }
        for (uint64_t s : to) g_svc->Send(s, m, true);
        return S_OK;
    }

    // ------------------------------------------------------------ groups (unused)
    HRESULT __stdcall CreateGroup(const void*, void*, void*, DPNHANDLE*, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall DestroyGroup(DPNID, void*, DPNHANDLE*, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall AddPlayerToGroup(DPNID, DPNID, void*, DPNHANDLE*, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall RemovePlayerFromGroup(DPNID, DPNID, void*, DPNHANDLE*, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall SetGroupInfo(DPNID, void*, void*, DPNHANDLE*, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall GetGroupInfo(DPNID, void*, DWORD*, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall EnumGroupMembers(DPNID, DPNID*, DWORD*, DWORD) override { return E_NOTIMPL; }
    HRESULT __stdcall GetGroupContext(DPNID, void**, DWORD) override { return E_NOTIMPL; }

    HRESULT __stdcall EnumPlayersAndGroups(DPNID* ids, DWORD* count, DWORD) override {
        if (!count) return E_POINTER;
        Lock l(&g_svc->cs);
        DWORD n = (DWORD)players.size();
        if (!ids || *count < n) {
            *count = n;
            return DPNERR_BUFFERTOOSMALL_;
        }
        DWORD i = 0;
        for (auto& kv : players) ids[i++] = kv.first;
        *count = n;
        return S_OK;
    }

    // ------------------------------------------------------------ player info
    HRESULT __stdcall SetPeerInfo(const DPN_PLAYER_INFO_* info, void* ctx, DPNHANDLE* h, DWORD flags) override {
        if (!info) return E_POINTER;
        std::vector<uint64_t> to;
        std::vector<uint8_t> m;
        DPNID me = 0;
        bool inSession;
        {
            Lock l(&g_svc->cs);
            if (info->dwInfoFlags & DPNINFO_NAME_) myName = info->pwszName ? info->pwszName : L"";
            if (info->dwInfoFlags & DPNINFO_DATA_)
                myData.assign((uint8_t*)info->pvData, (uint8_t*)info->pvData + (info->pvData ? info->dwDataSize : 0));
            inSession = st == St::Hosting || st == St::Connected;
            if (inSession) {
                me = localId;
                if (Player* p = Find(localId)) {
                    p->name = myName;
                    p->data = myData;
                }
                Writer w;
                w.u32(localId);
                w.wstr(myName);
                w.blob(myData);
                m = Msg(T_PEER_INFO, w);
                for (auto& kv : players)
                    if (!kv.second.local) to.push_back(kv.second.steam);
            }
        }
        for (uint64_t s : to) g_svc->Send(s, m, true);
        if (inSession) {
            PostSession([this, me]() {
                void* c = nullptr;
                {
                    Lock l(&g_svc->cs);
                    if (Player* p = Find(me)) c = p->ctx;
                }
                DPNMSG_PEER_INFO_ pm{sizeof(pm), me, c};
                Deliver(DPN_MSGID_PEER_INFO_, &pm);
            });
        }
        if (flags & DPNOP_SYNC_) return S_OK;
        DPNHANDLE hh;
        {
            Lock l(&g_svc->cs);
            hh = NewHandle();
        }
        if (h) *h = hh;
        g_svc->Post([this, hh, ctx]() {
            DPNMSG_ASYNC_OP_COMPLETE_ am{sizeof(am), hh, ctx, S_OK};
            Deliver(DPN_MSGID_ASYNC_OP_COMPLETE_, &am);
        });
        return DPNSUCCESS_PENDING_;
    }

    HRESULT __stdcall GetPeerInfo(DPNID id, DPN_PLAYER_INFO_* buf, DWORD* size, DWORD) override {
        if (!size) return E_POINTER;
        Lock l(&g_svc->cs);
        Player* p = Find(id);
        if (!p) return DPNERR_INVALIDPLAYER_;
        DWORD need = sizeof(DPN_PLAYER_INFO_) + (DWORD)(p->name.size() + 1) * 2 + (DWORD)p->data.size();
        if (!buf || *size < need) {
            *size = need;
            return DPNERR_BUFFERTOOSMALL_;
        }
        uint8_t* q = (uint8_t*)(buf + 1);
        buf->dwSize = sizeof(DPN_PLAYER_INFO_);
        buf->dwInfoFlags = DPNINFO_NAME_ | DPNINFO_DATA_;
        buf->pwszName = (PWSTR)q;
        memcpy(q, p->name.c_str(), (p->name.size() + 1) * 2);
        q += (p->name.size() + 1) * 2;
        buf->pvData = p->data.empty() ? nullptr : q;
        buf->dwDataSize = (DWORD)p->data.size();
        if (!p->data.empty()) memcpy(q, p->data.data(), p->data.size());
        buf->dwPlayerFlags = (p->local ? DPNPLAYER_LOCAL_ : 0) | (p->host ? DPNPLAYER_HOST_ : 0);
        return S_OK;
    }

    HRESULT __stdcall GetPeerAddress(DPNID id, IDirectPlay8Address_** addr, DWORD) override {
        if (!addr) return E_POINTER;
        uint64_t s;
        {
            Lock l(&g_svc->cs);
            Player* p = Find(id);
            if (!p) return DPNERR_INVALIDPLAYER_;
            s = p->steam;
        }
        *addr = NewAddress(L"steam:" + std::to_wstring(s));
        return *addr ? S_OK : E_FAIL;
    }

    HRESULT __stdcall GetLocalHostAddresses(IDirectPlay8Address_** addrs, DWORD* count, DWORD) override {
        if (!count) return E_POINTER;
        if (!addrs || *count < 1) {
            *count = 1;
            return DPNERR_BUFFERTOOSMALL_;
        }
        addrs[0] = NewAddress(L"steam:" + std::to_wstring(g_svc->me));
        *count = 1;
        return addrs[0] ? S_OK : E_FAIL;
    }

    // ------------------------------------------------------------ teardown
    HRESULT __stdcall Close(DWORD) override {
        std::vector<DPNID> ids;
        std::vector<uint64_t> to;
        St was;
        uint64_t L;
        bool enumWas;
        DPNHANDLE he = 0, hc = 0;
        void *ce = nullptr, *cc = nullptr;
        {
            Lock l(&g_svc->cs);
            if (st == St::Uninit) return DPNERR_UNINITIALIZED_;
            was = st;
            L = lobby;
            enumWas = enumActive;
            he = hEnum;
            ce = enumCtx;
            hc = hConnect;
            cc = connectCtx;
            enumActive = false;
            for (auto& kv : players) {
                ids.push_back(kv.first);
                if (!kv.second.local) to.push_back(kv.second.steam);
            }
            epoch++; // drop queued events of this session
        }
        if (was == St::Hosting) {
            Writer w;
            w.u32((uint32_t)DPNERR_CONNECTIONLOST_);
            w.blob(std::vector<uint8_t>());
            auto m = Msg(T_TERMINATE, w);
            for (uint64_t s : to) g_svc->Send(s, m, true);
        } else if (was == St::Connected) {
            Writer w;
            auto m = Msg(T_LEAVE, w);
            for (uint64_t s : to) g_svc->Send(s, m, true);
        }
        if (L) SteamMatchmaking()->LeaveLobby(CSteamID(L));
        if (was == St::Hosting || was == St::Connected) {
            SteamFriends()->ClearRichPresence();
            for (DPNID id : ids) DeliverDestroy(id, DPNDESTROYPLAYERREASON_NORMAL_);
        }
        if (enumWas) {
            DPNMSG_ASYNC_OP_COMPLETE_ m{sizeof(m), he, ce, DPNERR_USERCANCEL_};
            Deliver(DPN_MSGID_ASYNC_OP_COMPLETE_, &m);
        }
        if (was == St::Connecting) {
            DPNMSG_CONNECT_COMPLETE_ m{sizeof(m), hc, cc, DPNERR_USERCANCEL_, nullptr, 0, 0};
            Deliver(DPN_MSGID_CONNECT_COMPLETE_, &m);
        }
        Lock l(&g_svc->cs);
        players.clear();
        for (auto& kv : buffers) delete kv.second;
        buffers.clear();
        if (enumDevice) {
            enumDevice->Release();
            enumDevice = nullptr;
        }
        lobby = 0;
        st = St::Uninit;
        epoch++;
        Log("steam: session closed");
        return S_OK;
    }

    // ------------------------------------------------------------ enumeration
    HRESULT __stdcall EnumHosts(DPN_APPLICATION_DESC_* app, IDirectPlay8Address_* host, IDirectPlay8Address_* dev,
                                void*, DWORD, DWORD count, DWORD, DWORD timeout, void* ctx, DPNHANDLE* h,
                                DWORD) override {
        uint64_t target = host ? ParseSteamTarget(AddressHost(host)) : 0;
        Lock l(&g_svc->cs);
        if (st == St::Uninit) return DPNERR_UNINITIALIZED_;
        if (enumActive) return DPNERR_CONNECTING_;
        enumActive = true;
        hEnum = NewHandle();
        if (h) *h = hEnum;
        enumCtx = ctx;
        enumApp = app ? app->guidApplication : GUID{};
        enumTarget = target;
        enumLobby = CSteamID(target).IsLobby() ? target : 0;
        enumDirected = host != nullptr && count != INFINITE;
        enumStart = GetTickCount();
        enumTimeout = (timeout && timeout != INFINITE) ? timeout : (enumDirected ? 6000 : INFINITE);
        enumLastQuery = 0;
        if (enumDevice) enumDevice->Release();
        enumDevice = nullptr;
        if (dev) dev->Duplicate(&enumDevice);
        return DPNSUCCESS_PENDING_;
    }

    // Build a response from lobby metadata (Steam thread).
    void EmitLobby(uint64_t L) {
        auto* mm = SteamMatchmaking();
        CSteamID id(L);
        if (S.verbose)
            Log("steam: lobby %llu ksnf='%s' name='%s' app=%s", (unsigned long long)L, mm->GetLobbyData(id, kProtoKey),
                mm->GetLobbyData(id, "name"), mm->GetLobbyData(id, "app"));
        if (strcmp(mm->GetLobbyData(id, kProtoKey), kProtoVer) != 0) return;
        AppDesc d;
        auto app = Unhex(mm->GetLobbyData(id, "app"));
        auto inst = Unhex(mm->GetLobbyData(id, "inst"));
        if (app.size() != sizeof(GUID) || inst.size() != sizeof(GUID)) return;
        memcpy(&d.app, app.data(), sizeof(GUID));
        memcpy(&d.inst, inst.data(), sizeof(GUID));
        d.name = Widen(mm->GetLobbyData(id, "name"));
        d.maxPlayers = (DWORD)atoi(mm->GetLobbyData(id, "maxp"));
        d.flags = (DWORD)strtoul(mm->GetLobbyData(id, "flags"), nullptr, 10);
        d.resv = Unhex(mm->GetLobbyData(id, "resv"));
        d.appResv = Unhex(mm->GetLobbyData(id, "aresv"));
        DWORD cur = (DWORD)atoi(mm->GetLobbyData(id, "curp"));
        if (!cur) cur = (DWORD)mm->GetNumLobbyMembers(id);
        DWORD rtt = 0;
        SteamNetworkPingLocation_t loc;
        const char* ls = mm->GetLobbyData(id, "loc");
        if (*ls && SteamNetworkingUtils()->ParsePingLocationString(ls, loc)) {
            int p = SteamNetworkingUtils()->EstimatePingTimeFromLocalHost(loc);
            if (p > 0) rtt = (DWORD)p;
        }
        DPNHANDLE he;
        void* ce;
        IDirectPlay8Address_* dev = nullptr;
        {
            Lock l(&g_svc->cs);
            if (!enumActive) return;
            if (!IsEqualGUID(enumApp, GUID{}) && !IsEqualGUID(enumApp, d.app)) return;
            if (enumDirected && L != enumLobby) return; // "connect to address" must not pick a random lobby
            he = hEnum;
            ce = enumCtx;
            if (enumDevice) enumDevice->Duplicate(&dev);
        }
        (void)he;
        if (L == g_svc->inviteLobby) d.name = L"» " + d.name; // mark Steam invites
        std::vector<uint8_t> mem(d.Size(false));
        auto* ad = (DPN_APPLICATION_DESC_*)mem.data();
        d.Fill(ad, cur, false);
        IDirectPlay8Address_* sender = NewAddress(L"ks-lobby:" + std::to_wstring(L));
        if (!dev) dev = NewAddress(L"");
        if (!sender) {
            if (dev) dev->Release();
            return;
        }
        DPNMSG_ENUM_HOSTS_RESPONSE_ m{sizeof(m), sender, dev, ad, nullptr, 0, ce, rtt};
        Deliver(DPN_MSGID_ENUM_HOSTS_RESPONSE_, &m);
        sender->Release();
        if (dev) dev->Release();
    }

    // ------------------------------------------------------------ kick / buffers / misc
    HRESULT __stdcall DestroyPeer(DPNID id, const void* data, DWORD size, DWORD) override {
        uint64_t victim;
        std::vector<uint64_t> others;
        {
            Lock l(&g_svc->cs);
            if (st != St::Hosting) return DPNERR_NOTHOST_;
            Player* p = Find(id);
            if (!p || p->local) return DPNERR_INVALIDPLAYER_;
            victim = p->steam;
            for (auto& kv : players)
                if (!kv.second.local && kv.first != id) others.push_back(kv.second.steam);
        }
        Writer w;
        w.u32((uint32_t)DPNERR_HOSTTERMINATEDSESSION_);
        w.blob(std::vector<uint8_t>((const uint8_t*)data, (const uint8_t*)data + (data ? size : 0)));
        g_svc->Send(victim, Msg(T_TERMINATE, w), true);
        Writer d;
        d.u32(id);
        d.u32(DPNDESTROYPLAYERREASON_HOSTDESTROYEDPLAYER_);
        auto m = Msg(T_PLAYER_DEL, d);
        for (uint64_t s : others) g_svc->Send(s, m, true);
        PostSession([this, id]() {
            DeliverDestroy(id, DPNDESTROYPLAYERREASON_HOSTDESTROYEDPLAYER_);
            Lock l(&g_svc->cs);
            UpdateLobbyCount();
        });
        return S_OK;
    }

    HRESULT __stdcall ReturnBuffer(DPNHANDLE h, DWORD) override {
        Lock l(&g_svc->cs);
        auto it = buffers.find(h);
        if (it == buffers.end()) return DPNERR_INVALIDHANDLE_;
        delete it->second;
        buffers.erase(it);
        return S_OK;
    }

    HRESULT __stdcall GetPlayerContext(DPNID id, void** ctx, DWORD) override {
        if (!ctx) return E_POINTER;
        Lock l(&g_svc->cs);
        Player* p = Find(id);
        if (!p) return DPNERR_INVALIDPLAYER_;
        *ctx = p->ctx;
        return S_OK;
    }

    HRESULT __stdcall GetCaps(void* caps, DWORD) override {
        if (!caps) return E_POINTER;
        DWORD sz = *(DWORD*)caps;
        memset(caps, 0, sz);
        *(DWORD*)caps = sz;
        return S_OK;
    }
    HRESULT __stdcall SetCaps(const void*, DWORD) override { return S_OK; }
    HRESULT __stdcall SetSPCaps(const GUID*, const DPN_SP_CAPS_*, DWORD) override { return S_OK; }
    HRESULT __stdcall GetSPCaps(const GUID*, DPN_SP_CAPS_* c, DWORD) override {
        if (!c) return E_POINTER;
        DWORD sz = c->dwSize ? c->dwSize : sizeof(*c);
        memset(c, 0, sz);
        c->dwSize = sz;
        c->dwNumThreads = 2;
        c->dwDefaultEnumCount = 5;
        c->dwDefaultEnumRetryInterval = 1500;
        c->dwDefaultEnumTimeout = 1500;
        c->dwMaxEnumPayloadSize = 983;
        c->dwBuffersPerThread = 1;
        c->dwSystemBufferSize = 8192;
        return S_OK;
    }

    HRESULT __stdcall GetConnectionInfo(DPNID id, DPN_CONNECTION_INFO_* info, DWORD) override {
        if (!info) return E_POINTER;
        uint64_t s;
        {
            Lock l(&g_svc->cs);
            Player* p = Find(id);
            if (!p || p->local) return DPNERR_INVALIDPLAYER_;
            s = p->steam;
        }
        SteamNetworkingIdentity ident;
        ident.SetSteamID64(s);
        SteamNetConnectionRealTimeStatus_t q;
        memset(&q, 0, sizeof(q));
        NM()->GetSessionConnectionInfo(ident, nullptr, &q);
        DWORD sz = info->dwSize ? info->dwSize : sizeof(*info);
        memset(info, 0, sz);
        info->dwSize = sz;
        info->dwRoundTripLatencyMS = q.m_nPing > 0 ? (DWORD)q.m_nPing : 0;
        info->dwThroughputBPS = (DWORD)q.m_flOutBytesPerSec;
        return S_OK;
    }

    HRESULT __stdcall RegisterLobby(DPNHANDLE, void*, DWORD) override { return S_OK; }

    HRESULT __stdcall TerminateSession(void* data, DWORD size, DWORD) override {
        std::vector<uint64_t> to;
        {
            Lock l(&g_svc->cs);
            if (st != St::Hosting) return DPNERR_NOTHOST_;
            for (auto& kv : players)
                if (!kv.second.local) to.push_back(kv.second.steam);
        }
        Writer w;
        w.u32((uint32_t)DPNERR_HOSTTERMINATEDSESSION_);
        w.blob(std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + (data ? size : 0)));
        auto m = Msg(T_TERMINATE, w);
        for (uint64_t s : to) g_svc->Send(s, m, true);
        std::vector<uint8_t> copy((uint8_t*)data, (uint8_t*)data + (data ? size : 0));
        PostSession([this, copy]() { LocalTerminate(DPNERR_HOSTTERMINATEDSESSION_, copy); });
        return S_OK;
    }

    // Session ended from the outside (host gone, kicked, terminated).
    void LocalTerminate(HRESULT hr, std::vector<uint8_t> data) {
        std::vector<DPNID> ids;
        uint64_t L;
        {
            Lock l(&g_svc->cs);
            if (st != St::Hosting && st != St::Connected) return;
            for (auto& kv : players) ids.push_back(kv.first);
            L = lobby;
            lobby = 0;
            st = St::Idle;
        }
        Log("steam: session terminated hr=%08X", (unsigned)hr);
        DPNMSG_TERMINATE_SESSION_ m{sizeof(m), hr, data.empty() ? nullptr : data.data(), (DWORD)data.size()};
        Deliver(DPN_MSGID_TERMINATE_SESSION_, &m);
        for (DPNID id : ids) DeliverDestroy(id, DPNDESTROYPLAYERREASON_SESSIONTERMINATED_);
        if (L) SteamMatchmaking()->LeaveLobby(CSteamID(L));
        SteamFriends()->ClearRichPresence();
        Lock l(&g_svc->cs);
        epoch++;
    }

    // ------------------------------------------------------------ incoming (Steam thread)
    void OnMessage(uint64_t from, const uint8_t* d, size_t n) {
        if (n < 4) return;
        Reader r(d + 4, n - 4);
        switch ((MsgType)d[0]) {
        case T_CONNECT_REQ: OnConnectReq(from, r); break;
        case T_CONNECT_ACK: OnConnectAck(from, r); break;
        case T_CONNECT_NAK: {
            {
                Lock l(&g_svc->cs);
                if (st != St::Connecting || from != hostSteam) return;
            }
            HRESULT hr = (HRESULT)r.u32();
            FailConnect(hr, r.blob());
            break;
        }
        case T_PLAYER_ADD: {
            Player p;
            p.id = r.u32();
            p.steam = r.u64();
            p.name = r.wstr();
            p.data = r.blob();
            if (!r.ok) return;
            {
                Lock l(&g_svc->cs);
                if (st != St::Connected || from != hostSteam || Find(p.id)) return;
                p.lastRecv = GetTickCount();
                players[p.id] = p;
            }
            DeliverCreate(p.id);
            break;
        }
        case T_PLAYER_DEL: {
            DPNID id = r.u32();
            DWORD reason = r.u32();
            {
                Lock l(&g_svc->cs);
                if (st != St::Connected || from != hostSteam) return;
                Player* p = Find(id);
                if (!p || p->local) return;
            }
            DeliverDestroy(id, reason ? reason : DPNDESTROYPLAYERREASON_NORMAL_);
            break;
        }
        case T_DATA: {
            DPNID fromId = r.u32();
            r.u32(); // addressed id
            if (!r.ok) return;
            void* ctx;
            DPNID sender;
            {
                Lock l(&g_svc->cs);
                if (st != St::Hosting && st != St::Connected) return;
                Player* p = FindSteam(from);
                if (!p) return;
                p->lastRecv = GetTickCount();
                sender = p->id;
                ctx = p->ctx;
            }
            (void)fromId;
            DeliverReceive(sender, ctx, d + 4 + 8, n - 4 - 8);
            break;
        }
        case T_PEER_INFO: {
            DPNID id = r.u32();
            std::wstring name = r.wstr();
            auto data = r.blob();
            void* ctx;
            {
                Lock l(&g_svc->cs);
                Player* p = Find(id);
                if (!p || p->steam != from) return;
                p->name = name;
                p->data = data;
                ctx = p->ctx;
            }
            DPNMSG_PEER_INFO_ m{sizeof(m), id, ctx};
            Deliver(DPN_MSGID_PEER_INFO_, &m);
            break;
        }
        case T_TERMINATE: {
            {
                Lock l(&g_svc->cs);
                if (from != hostSteam || (st != St::Connected && st != St::Connecting)) return;
                if (st == St::Connecting) {
                    // treated as a refusal
                }
            }
            HRESULT hr = (HRESULT)r.u32();
            auto data = r.blob();
            St s;
            {
                Lock l(&g_svc->cs);
                s = st;
            }
            if (s == St::Connecting) FailConnect(DPNERR_HOSTREJECTEDCONNECTION_, data);
            else LocalTerminate(hr, data);
            break;
        }
        case T_LEAVE: {
            DPNID id;
            std::vector<uint64_t> others;
            {
                Lock l(&g_svc->cs);
                if (st != St::Hosting) return;
                Player* p = FindSteam(from);
                if (!p) return;
                id = p->id;
                for (auto& kv : players)
                    if (!kv.second.local && kv.second.steam != from) others.push_back(kv.second.steam);
            }
            Writer w;
            w.u32(id);
            w.u32(DPNDESTROYPLAYERREASON_NORMAL_);
            auto m = Msg(T_PLAYER_DEL, w);
            for (uint64_t s : others) g_svc->Send(s, m, true);
            DeliverDestroy(id, DPNDESTROYPLAYERREASON_NORMAL_);
            Lock l(&g_svc->cs);
            UpdateLobbyCount();
            break;
        }
        case T_APPDESC: {
            {
                Lock l(&g_svc->cs);
                if (st != St::Connected || from != hostSteam) return;
                AppDesc nd;
                nd.Read(r);
                if (!r.ok) return;
                desc = nd;
            }
            Deliver(DPN_MSGID_APPLICATION_DESC_, nullptr);
            break;
        }
        case T_PING: {
            Lock l(&g_svc->cs);
            if (Player* p = FindSteam(from)) p->lastRecv = GetTickCount();
            break;
        }
        }
    }

    void OnConnectReq(uint64_t from, Reader& r) {
        GUID app = r.guid();
        GUID inst = r.guid();
        std::wstring name = r.wstr();
        std::wstring pw = r.wstr();
        auto userData = r.blob();
        auto playerData = r.blob();
        if (!r.ok) return;
        auto nak = [&](HRESULT hr, const std::vector<uint8_t>& reply) {
            Writer w;
            w.u32((uint32_t)hr);
            w.blob(reply);
            g_svc->Send(from, Msg(T_CONNECT_NAK, w), true);
        };
        {
            Lock l(&g_svc->cs);
            if (st != St::Hosting) {
                nak(DPNERR_NOCONNECTION_, {});
                return;
            }
            if (Player* p = FindSteam(from)) { // retransmitted request: resend the ack
                SendAck(from, p->id, {});
                return;
            }
            if (!IsEqualGUID(app, desc.app)) { nak(DPNERR_INVALIDAPPLICATION_, {}); return; }
            (void)inst;
            if ((desc.flags & DPNSESSION_REQUIREPASSWORD_) && pw != desc.password) { nak(DPNERR_INVALIDPASSWORD_, {}); return; }
            if (players.size() >= desc.maxPlayers) { nak(DPNERR_SESSIONFULL_, {}); return; }
        }
        IDirectPlay8Address_* pa = NewAddress(L"steam:" + std::to_wstring(from));
        IDirectPlay8Address_* da = NewAddress(L"");
        DPNMSG_INDICATE_CONNECT_ ic{sizeof(ic), userData.empty() ? nullptr : userData.data(), (DWORD)userData.size(),
                                    nullptr, 0, nullptr, nullptr, pa, da};
        HRESULT hr = Deliver(DPN_MSGID_INDICATE_CONNECT_, &ic);
        if (pa) pa->Release();
        if (da) da->Release();
        std::vector<uint8_t> reply((uint8_t*)ic.pvReplyData, (uint8_t*)ic.pvReplyData + (ic.pvReplyData ? ic.dwReplyDataSize : 0));
        auto returnReply = [&]() {
            if (!ic.pvReplyData) return;
            DPNMSG_RETURN_BUFFER_ rb{sizeof(rb), S_OK, ic.pvReplyData, ic.pvReplyContext};
            Deliver(DPN_MSGID_RETURN_BUFFER_, &rb);
        };
        if (hr != S_OK) {
            Log("steam: game refused connection from %llu (hr=%08X)", (unsigned long long)from, (unsigned)hr);
            nak(DPNERR_HOSTREJECTEDCONNECTION_, reply);
            returnReply();
            return;
        }
        DPNID id;
        std::vector<uint64_t> others;
        Player np;
        {
            Lock l(&g_svc->cs);
            if (st != St::Hosting) return;
            id = nextId++;
            np.id = id;
            np.steam = from;
            np.name = name;
            np.data = playerData;
            np.ctx = ic.pvPlayerContext;
            np.lastRecv = GetTickCount();
            for (auto& kv : players)
                if (!kv.second.local) others.push_back(kv.second.steam);
            players[id] = np;
            SendAck(from, id, reply);
            UpdateLobbyCount();
        }
        Writer w;
        w.u32(np.id);
        w.u64(np.steam);
        w.wstr(np.name);
        w.blob(np.data);
        auto m = Msg(T_PLAYER_ADD, w);
        for (uint64_t s : others) g_svc->Send(s, m, true);
        Log("steam: player %llu joined as %08X", (unsigned long long)from, (unsigned)id);
        returnReply();
        DeliverCreate(id);
    }

    // The player list leaves out the joining player itself: the joiner creates its own
    // entry as the local player.
    void SendAck(uint64_t to, DPNID id, const std::vector<uint8_t>& reply) { // cs held
        Writer w;
        w.u32(id);
        w.u32(hostId);
        desc.Write(w);
        w.blob(reply);
        w.u32((uint32_t)(players.size() - (Find(id) ? 1 : 0)));
        for (auto& kv : players) {
            const Player& p = kv.second;
            if (p.id == id) continue;
            w.u32(p.id);
            w.u64(p.steam);
            w.u32(p.host ? 1 : 0);
            w.wstr(p.name);
            w.blob(p.data);
        }
        g_svc->Send(to, Msg(T_CONNECT_ACK, w), true);
    }

    void OnConnectAck(uint64_t from, Reader& r) {
        DPNID me = r.u32();
        DPNID host = r.u32();
        AppDesc nd;
        nd.Read(r);
        auto reply = r.blob();
        uint32_t n = r.u32();
        std::vector<Player> list;
        for (uint32_t i = 0; i < n && r.ok && i < 64; i++) {
            Player p;
            p.id = r.u32();
            p.steam = r.u64();
            p.host = r.u32() != 0;
            p.name = r.wstr();
            p.data = r.blob();
            list.push_back(p);
        }
        if (!r.ok) return;
        DPNHANDLE hc;
        void* cc;
        std::vector<DPNID> order;
        {
            Lock l(&g_svc->cs);
            if (st != St::Connecting || from != hostSteam) return;
            std::wstring pw = desc.password;
            desc = nd;
            desc.password = pw;
            players.clear();
            localId = me;
            hostId = host;
            Player self;
            self.id = me;
            self.steam = g_svc->me;
            self.name = myName;
            self.data = myData;
            self.local = true;
            self.ctx = connectPlayerCtx;
            players[me] = self;
            order.push_back(me);
            for (auto& p : list) {
                // Older hosts also list the joiner; replacing the local entry with that copy
                // dropped DPNPLAYER_LOCAL and created the local player twice, so the game lost
                // track of its own lobby slot (dead "Ready", other players' heroes in our slot).
                if (p.id == me || p.steam == g_svc->me) continue;
                p.lastRecv = GetTickCount();
                players[p.id] = p;
                if (p.id == host) order.insert(order.begin() + 1, p.id);
                else order.push_back(p.id);
            }
            hc = hConnect;
            cc = connectCtx;
            st = St::Connected;
        }
        Log("steam: connected as %08X to host %08X (%u players)", (unsigned)me, (unsigned)host, (unsigned)list.size());
        char rp[64];
        sprintf(rp, "+connect_lobby %llu", (unsigned long long)lobby);
        if (lobby) SteamFriends()->SetRichPresence("connect", rp);
        SteamFriends()->SetRichPresence("status", "KnightShift - multiplayer");
        for (DPNID id : order) DeliverCreate(id);
        DPNMSG_CONNECT_COMPLETE_ m{sizeof(m), hc, cc, S_OK, reply.empty() ? nullptr : reply.data(), (DWORD)reply.size(), me};
        Deliver(DPN_MSGID_CONNECT_COMPLETE_, &m);
    }

    void OnPeerLost(uint64_t s) {
        DPNID id = 0;
        bool hostLost = false, amHost = false;
        std::vector<uint64_t> others;
        {
            Lock l(&g_svc->cs);
            if (st == St::Connecting && s == hostSteam) {
                // handled by the connect timeout
                return;
            }
            Player* p = FindSteam(s);
            if (!p) return;
            id = p->id;
            hostLost = p->host;
            amHost = st == St::Hosting;
            if (amHost)
                for (auto& kv : players)
                    if (!kv.second.local && kv.second.steam != s) others.push_back(kv.second.steam);
        }
        Log("steam: lost connection to %llu (%08X)", (unsigned long long)s, (unsigned)id);
        if (hostLost) {
            LocalTerminate(DPNERR_CONNECTIONLOST_, {});
            return;
        }
        if (amHost) {
            Writer w;
            w.u32(id);
            w.u32(DPNDESTROYPLAYERREASON_CONNECTIONLOST_);
            auto m = Msg(T_PLAYER_DEL, w);
            for (uint64_t o : others) g_svc->Send(o, m, true);
        }
        DeliverDestroy(id, DPNDESTROYPLAYERREASON_CONNECTIONLOST_);
        Lock l(&g_svc->cs);
        UpdateLobbyCount();
    }

    // ------------------------------------------------------------ timers (Steam thread)
    void Tick() {
        DWORD now = GetTickCount();
        std::vector<uint64_t> pingTo, lost;
        bool connectTimeout = false, enumQuery = false, enumDone = false;
        DPNHANDLE he = 0;
        void* ce = nullptr;
        uint64_t directed = 0;
        bool isDirected = false;
        {
            Lock l(&g_svc->cs);
            if (st == St::Connecting && hostSteam) {
                if (now - connectLastReq >= kConnectRetryMs) SendConnectRequest();
            }
            if (st == St::Connecting && now - connectStart > kConnectTimeoutMs) connectTimeout = true;
            if ((st == St::Hosting || st == St::Connected) && now - lastPing >= kPingMs) {
                lastPing = now;
                // The relay network needs a few seconds to measure our location; publish it once known.
                if (st == St::Hosting && lobby && !locPublished) PublishLobby();
                for (auto& kv : players) {
                    if (kv.second.local) continue;
                    if (st == St::Connected && !kv.second.host) continue; // clients keep alive with the host
                    pingTo.push_back(kv.second.steam);
                    if (now - kv.second.lastRecv > kPeerTimeoutMs) lost.push_back(kv.second.steam);
                }
            }
            if (enumActive) {
                if (enumTimeout != INFINITE && now - enumStart > enumTimeout) {
                    enumActive = false;
                    enumDone = true;
                    he = hEnum;
                    ce = enumCtx;
                } else if (!enumLastQuery || now - enumLastQuery >= (enumDirected ? 1500u : 3000u)) {
                    enumLastQuery = now;
                    enumQuery = true;
                    directed = enumTarget;
                    isDirected = enumDirected;
                }
            }
        }
        if (!pingTo.empty()) {
            Writer w;
            auto m = Msg(T_PING, w);
            for (uint64_t s : pingTo) g_svc->Send(s, m, false);
        }
        for (uint64_t s : lost) OnPeerLost(s);
        if (connectTimeout) FailConnect(DPNERR_TIMEDOUT_, {});
        if (enumQuery) StartEnumQuery(isDirected, directed);
        if (enumDone) {
            DPNMSG_ASYNC_OP_COMPLETE_ m{sizeof(m), he, ce, S_OK};
            Deliver(DPN_MSGID_ASYNC_OP_COMPLETE_, &m);
        }
    }

    void StartEnumQuery(bool isDirected, uint64_t directed) {
        auto* mm = SteamMatchmaking();
        if (isDirected) {
            // The player typed a lobby id or a friend's SteamID64 as the "IP address".
            if (!directed) return;
            CSteamID t(directed);
            if (t.IsLobby()) {
                mm->RequestLobbyData(t);
            } else {
                FriendGameInfo_t gi;
                if (SteamFriends()->GetFriendGamePlayed(t, &gi) && gi.m_steamIDLobby.IsValid()) {
                    {
                        Lock l(&g_svc->cs);
                        enumLobby = gi.m_steamIDLobby.ConvertToUint64();
                    }
                    mm->RequestLobbyData(gi.m_steamIDLobby);
                }
            }
            return;
        }
        if (g_svc->inviteLobby) mm->RequestLobbyData(CSteamID(g_svc->inviteLobby));
        int nf = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
        for (int i = 0; i < nf; i++) {
            FriendGameInfo_t gi;
            CSteamID f = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
            if (SteamFriends()->GetFriendGamePlayed(f, &gi) && gi.m_steamIDLobby.IsValid() &&
                gi.m_gameID.AppID() == SteamUtils()->GetAppID())
                mm->RequestLobbyData(gi.m_steamIDLobby);
        }
        mm->AddRequestLobbyListStringFilter(kProtoKey, kProtoVer, k_ELobbyComparisonEqual);
        mm->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
        mm->AddRequestLobbyListResultCountFilter(50);
        SteamAPICall_t c = mm->RequestLobbyList();
        g_svc->crList.Set(c, g_svc, &SteamService::OnLobbyList);
    }
};

// ---------------------------------------------------------------------------
// SteamService implementation
// ---------------------------------------------------------------------------
void SteamService::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t* r) {
    uint64_t s = r->m_identityRemote.GetSteamID64();
    bool ok = false;
    {
        Lock l(&cs);
        if (peer && (peer->st == St::Hosting || peer->FindSteam(s) || peer->hostSteam == s)) ok = true;
    }
    if (ok) NM()->AcceptSessionWithUser(r->m_identityRemote);
}

void SteamService::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t* r) {
    uint64_t s = r->m_info.m_identityRemote.GetSteamID64();
    Log("steam: session with %llu failed (%d: %s)", (unsigned long long)s, (int)r->m_info.m_eEndReason, r->m_info.m_szEndDebug);
    SteamPeer* p;
    {
        Lock l(&cs);
        p = peer;
    }
    if (p) p->OnPeerLost(s);
}

void SteamService::OnLobbyJoinRequested(GameLobbyJoinRequested_t* r) {
    inviteLobby = r->m_steamIDLobby.ConvertToUint64();
    Log("steam: invited to lobby %llu - it is marked >> in the session list", (unsigned long long)inviteLobby);
}

void SteamService::OnLobbyDataUpdate(LobbyDataUpdate_t* r) {
    if (!r->m_bSuccess || r->m_ulSteamIDLobby != r->m_ulSteamIDMember) return;
    SteamPeer* p;
    {
        Lock l(&cs);
        p = peer;
    }
    if (p) p->EmitLobby(r->m_ulSteamIDLobby);
}

void SteamService::OnLobbyCreated(LobbyCreated_t* r, bool io) {
    SteamPeer* p;
    {
        Lock l(&cs);
        p = peer;
    }
    if (!p) return;
    if (io || r->m_eResult != k_EResultOK) {
        Log("steam: creating the lobby failed (%d) - nobody can find this session", io ? -1 : (int)r->m_eResult);
        return;
    }
    Lock l(&cs);
    if (p->st != St::Hosting) {
        SteamMatchmaking()->LeaveLobby(CSteamID(r->m_ulSteamIDLobby));
        return;
    }
    p->lobby = r->m_ulSteamIDLobby;
    p->PublishLobby();
    char rp[64];
    sprintf(rp, "+connect_lobby %llu", (unsigned long long)p->lobby);
    SteamFriends()->SetRichPresence("connect", rp);
    SteamFriends()->SetRichPresence("status", "KnightShift - hosting a game");
    Log("steam: lobby %llu created (%s)", (unsigned long long)p->lobby, S.lobbyFriendsOnly ? "friends only" : "public");
}

void SteamService::OnLobbyEnter(LobbyEnter_t* r, bool io) {
    SteamPeer* p;
    {
        Lock l(&cs);
        p = peer;
    }
    if (!p) return;
    if (io || r->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
        p->FailConnect(DPNERR_NOCONNECTION_, {});
        return;
    }
    Lock l(&cs);
    if (p->st != St::Connecting) {
        SteamMatchmaking()->LeaveLobby(CSteamID(r->m_ulSteamIDLobby));
        return;
    }
    p->lobby = r->m_ulSteamIDLobby;
    p->hostSteam = SteamMatchmaking()->GetLobbyOwner(CSteamID(p->lobby)).ConvertToUint64();
    const char* h = SteamMatchmaking()->GetLobbyData(CSteamID(p->lobby), "host");
    if (*h) p->hostSteam = _strtoui64(h, nullptr, 10);
    p->SendConnectRequest();
}

void SteamService::OnLobbyList(LobbyMatchList_t* r, bool io) {
    if (S.verbose) Log("steam: lobby search -> %u lobbies (io failure %d)", io ? 0 : r->m_nLobbiesMatching, (int)io);
    if (io) return;
    SteamPeer* p;
    {
        Lock l(&cs);
        p = peer;
    }
    if (!p) return;
    for (uint32 i = 0; i < r->m_nLobbiesMatching; i++) p->EmitLobby(SteamMatchmaking()->GetLobbyByIndex((int)i).ConvertToUint64());
}

void SteamService::Run() {
    SteamNetworkingMessage_t* msgs[64];
    for (;;) {
        RunCB();
        SteamPeer* p;
        {
            Lock l(&cs);
            p = peer;
        }
        for (;;) {
            int n = NM()->ReceiveMessagesOnChannel(kChannel, msgs, 64);
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                if (p) p->OnMessage(msgs[i]->m_identityPeer.GetSteamID64(), (const uint8_t*)msgs[i]->m_pData, (size_t)msgs[i]->m_cbSize);
                msgs[i]->Release();
            }
        }
        if (p) p->Tick();
        for (;;) {
            std::function<void()> f;
            {
                Lock l(&cs);
                if (events.empty()) break;
                f = std::move(events.front());
                events.pop_front();
            }
            f();
        }
        Sleep(1);
    }
}

DWORD WINAPI SteamThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // DirectPlay address objects are created here
    g_svc->Run();
    return 0;
}

bool g_steamTried = false, g_steamOk = false;

bool EnsureSteam() {
    static std::mutex m; // the game thread (DirectPlay) and SteamNet's threads (ranking)
    std::lock_guard<std::mutex> lock(m);
    if (g_steamTried) return g_steamOk;
    g_steamTried = true;
    char dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char* sl = strrchr(dir, '\\')) sl[1] = 0;
    _snprintf(path, sizeof(path), "%ssteam_api.dll", dir);
    if (!LoadLibraryA(path)) {
        Log("steam: steam_api.dll not found next to the game - using DirectPlay");
        return false;
    }
    char appId[16];
    sprintf(appId, "%u", S.appId);
    SetEnvironmentVariableA("SteamAppId", appId);
    SetEnvironmentVariableA("SteamGameId", appId);
    if (S.gameServer) {
        g_gs = true;
        SteamErrMsg gerr = {0};
        if (SteamGameServer_InitEx(0, 27031, STEAMGAMESERVER_QUERY_PORT_SHARED, eServerModeNoAuthentication,
                                   "1.0.0.0", &gerr) != k_ESteamAPIInitResult_OK) {
            Log("steam: game-server init failed (%s) - using DirectPlay", gerr);
            g_gs = false;
            return false;
        }
        SteamGameServer()->SetProduct("KnightShift");
        SteamGameServer()->SetGameDescription("KnightShift RPG");
        SteamGameServer()->SetModDir("KnightShift");
        SteamGameServer()->SetDedicatedServer(true);
        SteamGameServer()->SetServerName(S.serverName);
        SteamGameServer()->SetMaxPlayerCount(8);
        SteamGameServer()->LogOnAnonymous();
        SteamNetworkingUtils()->InitRelayNetworkAccess();
        ULONGLONG t0 = GetTickCount64();
        while (!SteamGameServer()->BLoggedOn() && GetTickCount64() - t0 < 20000) {
            SteamGameServer_RunCallbacks();
            Sleep(50);
        }
        if (!SteamGameServer()->BLoggedOn()) {
            Log("steam: anonymous game-server logon failed - is Steam / steamclient available? Using DirectPlay");
            SteamGameServer_Shutdown();
            g_gs = false;
            return false;
        }
        g_svc = new SteamService();
        g_svc->me = SteamGameServer()->GetSteamID().ConvertToUint64();
        g_svc->myName = Widen(S.serverName);
        g_svc->gsReq = new CCallback<SteamService, SteamNetworkingMessagesSessionRequest_t, true>(
            g_svc, &SteamService::OnSessionRequest);
        g_svc->gsFail = new CCallback<SteamService, SteamNetworkingMessagesSessionFailed_t, true>(
            g_svc, &SteamService::OnSessionFailed);
        CreateThread(nullptr, 0, SteamThread, nullptr, 0, nullptr);
        Log("steam: anonymous game server ready, app id %u, SteamID %llu", S.appId, (unsigned long long)g_svc->me);
        Log("steam: players join by entering this SteamID in the address field: %llu", (unsigned long long)g_svc->me);
        g_steamOk = true;
        return true;
    }
    SteamErrMsg err = {0};
    ESteamAPIInitResult r = SteamAPI_InitEx(&err);
    if (r != k_ESteamAPIInitResult_OK) {
        Log("steam: SteamAPI init failed (%d: %s) - is Steam running? Using DirectPlay", (int)r, err);
        return false;
    }
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    g_svc = new SteamService();
    g_svc->me = SteamUser()->GetSteamID().ConvertToUint64();
    g_svc->myName = Widen(SteamFriends()->GetPersonaName());
    // launched from a Steam invite: "... +connect_lobby <id>"
    if (const char* c = strstr(GetCommandLineA(), "+connect_lobby ")) g_svc->inviteLobby = _strtoui64(c + 15, nullptr, 10);
    CreateThread(nullptr, 0, SteamThread, nullptr, 0, nullptr);
    Log("steam: ready as \"%s\" (%llu), app id %u, lobbies %s", SteamFriends()->GetPersonaName(),
        (unsigned long long)g_svc->me, S.appId, S.lobbyFriendsOnly ? "friends only" : "public");
    g_steamOk = true;
    return true;
}

// ---------------------------------------------------------------------------
// Leaderboards for the SteamNet ranking. Each request runs on the Steam thread
// (call results are dispatched there); the SteamNet connection thread waits for it.
// ---------------------------------------------------------------------------
std::string Ansi(const char* utf8) {
    wchar_t w[256];
    char a[256];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 256)) return std::string();
    if (!WideCharToMultiByte(CP_ACP, 0, w, -1, a, sizeof(a), "?", nullptr)) return std::string();
    return a;
}

struct BoardOp {
    bool join = false, create = false;
    std::string board;
    int count = 10, score = 0;
    std::vector<int32> details;
    // results, valid once `done` is set
    bool ok = false;
    std::vector<en::BoardEntry> out;

    HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    LONG refs = 1; // requester; +1 while the Steam thread works on it
    SteamLeaderboard_t lb = 0;
    CCallResult<BoardOp, LeaderboardFindResult_t> crFind;
    CCallResult<BoardOp, LeaderboardScoresDownloaded_t> crDown;
    CCallResult<BoardOp, LeaderboardScoreUploaded_t> crUp;

    void Release() {
        if (InterlockedDecrement(&refs) == 0) {
            CloseHandle(done);
            delete this;
        }
    }
    void Finish(bool success) {
        ok = success;
        SetEvent(done);
        g_svc->Post([this] { Release(); }); // not from inside the call result that is running now
    }
    void Start() {
        ISteamUserStats* us = SteamUserStats();
        SteamAPICall_t c = !us ? k_uAPICallInvalid
                           : create ? us->FindOrCreateLeaderboard(board.c_str(), k_ELeaderboardSortMethodDescending,
                                                                  k_ELeaderboardDisplayTypeNumeric)
                                    : us->FindLeaderboard(board.c_str());
        if (c == k_uAPICallInvalid) {
            Log("steam: ranking \"%s\": leaderboard request refused", board.c_str());
            return Finish(false);
        }
        crFind.Set(c, this, &BoardOp::OnFind);
    }
    void OnFind(LeaderboardFindResult_t* r, bool io) {
        if (io || !r->m_bLeaderboardFound) {
            // FindLeaderboard of a board nobody wrote to yet: an empty ranking, not an error
            Log("steam: ranking \"%s\": %s", board.c_str(), io ? "Steam I/O failure" : create ? "cannot create the leaderboard" : "no leaderboard yet");
            return Finish(!io && !create && !join);
        }
        lb = r->m_hSteamLeaderboard;
        ISteamUserStats* us = SteamUserStats();
        Log("steam: ranking \"%s\": leaderboard %llu, %d entries", board.c_str(), (unsigned long long)lb,
            us->GetLeaderboardEntryCount(lb));
        SteamAPICall_t c;
        if (join) {
            CSteamID me = SteamUser()->GetSteamID();
            c = us->DownloadLeaderboardEntriesForUsers(lb, &me, 1);
        } else {
            c = us->DownloadLeaderboardEntries(lb, k_ELeaderboardDataRequestGlobal, 1, count);
        }
        crDown.Set(c, this, &BoardOp::OnDownloaded);
    }
    void OnDownloaded(LeaderboardScoresDownloaded_t* r, bool io) {
        if (io) {
            Log("steam: ranking \"%s\": download failed", board.c_str());
            return Finish(false);
        }
        ISteamUserStats* us = SteamUserStats();
        if (join) {
            if (r->m_cEntryCount > 0) {
                Log("steam: ranking \"%s\": already listed", board.c_str());
                return Finish(true);
            }
            SteamAPICall_t c = us->UploadLeaderboardScore(lb, k_ELeaderboardUploadScoreMethodKeepBest, score,
                                                          details.data(), (int)details.size());
            crUp.Set(c, this, &BoardOp::OnUploaded);
            return;
        }
        for (int i = 0; i < r->m_cEntryCount; i++) {
            LeaderboardEntry_t e;
            int32 d[k_cLeaderboardDetailsMax];
            if (!us->GetDownloadedLeaderboardEntry(r->m_hSteamLeaderboardEntries, i, &e, d, k_cLeaderboardDetailsMax)) continue;
            en::BoardEntry b;
            b.steamName = Ansi(SteamFriends()->GetFriendPersonaName(e.m_steamIDUser));
            b.score = e.m_nScore;
            b.details.assign(d, d + (e.m_cDetails < k_cLeaderboardDetailsMax ? e.m_cDetails : k_cLeaderboardDetailsMax));
            out.push_back(b);
        }
        Log("steam: ranking \"%s\": %d entries downloaded", board.c_str(), (int)out.size());
        Finish(true);
    }
    void OnUploaded(LeaderboardScoreUploaded_t* r, bool io) {
        bool success = !io && r->m_bSuccess;
        Log("steam: ranking \"%s\": %s", board.c_str(), success ? "player added" : "upload refused");
        Finish(success);
    }
};

// Hands op to the Steam thread, which drops its reference when done. False if Steam is off.
bool PostBoardOp(BoardOp* op) {
    if (!S.enabled || !EnsureSteam() || g_gs) {
        Log("steam: ranking \"%s\": Steam not available ([Steam] Enabled=0 or Steam not running)", op->board.c_str());
        return false;
    }
    InterlockedIncrement(&op->refs);
    g_svc->Post([op] { op->Start(); });
    return true;
}

class SteamRanking : public en::RankingService {
  public:
    bool Top(const std::string& board, bool create, int count, std::vector<en::BoardEntry>& out) override {
        BoardOp* op = new BoardOp;
        op->board = board;
        op->create = create;
        op->count = count;
        bool ok = false;
        if (PostBoardOp(op)) {
            if (WaitForSingleObject(op->done, 8000) == WAIT_OBJECT_0) {
                ok = op->ok;
                if (ok) out = op->out;
            } else {
                Log("steam: ranking \"%s\": no answer from Steam in 8 s", board.c_str());
            }
        }
        op->Release();
        return ok;
    }
    void Join(const std::string& board, int score, const std::vector<int32_t>& details) override {
        BoardOp* op = new BoardOp;
        op->join = op->create = true;
        op->board = board;
        op->score = score;
        op->details.assign(details.begin(), details.end());
        PostBoardOp(op); // fire and forget
        op->Release();
    }
};

} // namespace

HRESULT __stdcall Steam_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID* ppv) {
    if (IsEqualGUID(clsid, CLSID_DirectPlay8Peer_) && S.enabled && EnsureSteam()) {
        SteamPeer* p = new SteamPeer();
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        Log("steam: game created a DirectPlay peer -> Steam transport");
        return hr;
    }
    return g_origCoCreate(clsid, outer, ctx, riid, ppv);
}

void Steam_Configure(const SteamSettings& s, HCoCreate orig) {
    S = s;
    g_origCoCreate = orig;
}

en::RankingService* Steam_Ranking() {
    static SteamRanking r;
    return &r;
}

std::string Steam_AccountName() {
    if (!S.enabled || !EnsureSteam() || g_gs) return std::string();
    std::string nick = en::SanitizeNick(Ansi(SteamFriends()->GetPersonaName()));
    if (nick.empty()) { // a name made only of characters the game cannot show
        char buf[32];
        sprintf(buf, "Gracz %05u", (unsigned)(g_svc->me % 100000));
        nick = buf;
    }
    return nick;
}
