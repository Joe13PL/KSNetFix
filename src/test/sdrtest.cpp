// Steam Datagram Relay check for a given App ID.
//   sdrtest status <appid>            relay network / auth status, nearest POPs
//   sdrtest server <appid> <idfile>   anonymous Steam game server listening over SDR (echo)
//   sdrtest client <appid> <idfile>   user connects to that server, relays only (ICE disabled)
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "steam/steam_api.h"
#include "steam/steam_gameserver.h"
#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

static const char* Avail(ESteamNetworkingAvailability a) {
    switch (a) {
    case k_ESteamNetworkingAvailability_CannotTry: return "CannotTry";
    case k_ESteamNetworkingAvailability_Failed: return "Failed";
    case k_ESteamNetworkingAvailability_Previously: return "Previously";
    case k_ESteamNetworkingAvailability_Retrying: return "Retrying";
    case k_ESteamNetworkingAvailability_NeverTried: return "NeverTried";
    case k_ESteamNetworkingAvailability_Waiting: return "Waiting";
    case k_ESteamNetworkingAvailability_Attempting: return "Attempting";
    case k_ESteamNetworkingAvailability_Current: return "Current(OK)";
    default: return "Unknown";
    }
}
static void Pop(SteamNetworkingPOPID id, char* out) {
    out[0] = (char)(id >> 16); out[1] = (char)(id >> 8); out[2] = (char)id; out[3] = (char)(id >> 24); out[4] = 0;
    if (!out[3]) out[3] = 0;
}
static double Now() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }

static void SetApp(const char* app) {
    SetEnvironmentVariableA("SteamAppId", app);
    SetEnvironmentVariableA("SteamGameId", app);
}

static HSteamNetConnection g_conn = k_HSteamNetConnection_Invalid;
static bool g_server = false, g_connected = false, g_closed = false;

static void OnStatus(SteamNetConnectionStatusChangedCallback_t* c) {
    ISteamNetworkingSockets* s = g_server ? SteamGameServerNetworkingSockets() : SteamNetworkingSockets();
    printf("[%s] conn %u state %d -> %d  %s\n", g_server ? "server" : "client", c->m_hConn, (int)c->m_eOldState,
           (int)c->m_info.m_eState, c->m_info.m_szEndDebug);
    if (g_server && c->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting) {
        printf("[server] accept: %d\n", (int)s->AcceptConnection(c->m_hConn));
        g_conn = c->m_hConn;
    }
    if (c->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected) g_connected = true;
    if (c->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
        c->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        g_closed = true;
        s->CloseConnection(c->m_hConn, 0, nullptr, false);
    }
    fflush(stdout);
}

struct ClientCb {
    STEAM_CALLBACK(ClientCb, On, SteamNetConnectionStatusChangedCallback_t);
};
void ClientCb::On(SteamNetConnectionStatusChangedCallback_t* c) { OnStatus(c); }
struct ServerCb {
    STEAM_GAMESERVER_CALLBACK(ServerCb, On, SteamNetConnectionStatusChangedCallback_t);
};
void ServerCb::On(SteamNetConnectionStatusChangedCallback_t* c) { OnStatus(c); }

static void PollState(ISteamNetworkingSockets* s, HSteamNetConnection h, const char* who) {
    SteamNetConnectionInfo_t info;
    if (h && s->GetConnectionInfo(h, &info))
        printf("[%s] poll: state %d end %d '%s' %s\n", who, (int)info.m_eState, info.m_eEndReason, info.m_szEndDebug,
               info.m_szConnectionDescription);
    SteamNetAuthenticationStatus_t au;
    ESteamNetworkingAvailability a = s->GetAuthenticationStatus(&au);
    printf("[%s] auth: %s %s\n", who, Avail(a), au.m_debugMsg);
}

static void PrintConn(ISteamNetworkingSockets* s, HSteamNetConnection h, const char* who) {
    SteamNetConnectionInfo_t info;
    SteamNetConnectionRealTimeStatus_t rt;
    if (s->GetConnectionInfo(h, &info)) {
        char relay[8], remote[8];
        Pop(info.m_idPOPRelay, relay);
        Pop(info.m_idPOPRemote, remote);
        printf("[%s] %s | relay POP='%s' remote POP='%s' flags=0x%x\n", who, info.m_szConnectionDescription, relay, remote,
               (unsigned)info.m_nFlags);
    }
    if (s->GetConnectionRealTimeStatus(h, &rt, 0, nullptr) == k_EResultOK)
        printf("[%s] ping %d ms, quality local %.2f remote %.2f\n", who, rt.m_nPing, rt.m_flConnectionQualityLocal,
               rt.m_flConnectionQualityRemote);
    fflush(stdout);
}

static int Status(const char* app) {
    SetApp(app);
    SteamErrMsg err = {0};
    if (SteamAPI_InitEx(&err) != k_ESteamAPIInitResult_OK) { printf("SteamAPI init failed for %s: %s\n", app, err); return 1; }
    printf("app %s: SteamAPI OK, user %llu, owns app: %d\n", app, SteamUser()->GetSteamID().ConvertToUint64(),
           SteamApps()->BIsSubscribedApp((AppId_t)atoi(app)));
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    SteamRelayNetworkStatus_t st = {};
    SteamNetAuthenticationStatus_t au = {};
    double t0 = Now();
    ESteamNetworkingAvailability a = k_ESteamNetworkingAvailability_Unknown, b = k_ESteamNetworkingAvailability_Unknown;
    while (Now() - t0 < 25000) {
        SteamAPI_RunCallbacks();
        a = SteamNetworkingUtils()->GetRelayNetworkStatus(&st);
        b = SteamNetworkingSockets()->GetAuthenticationStatus(&au);
        if (a == k_ESteamNetworkingAvailability_Current && b == k_ESteamNetworkingAvailability_Current && !st.m_bPingMeasurementInProgress) break;
        if (a == k_ESteamNetworkingAvailability_Failed || a == k_ESteamNetworkingAvailability_CannotTry) break;
        Sleep(50);
    }
    printf("relay network: %s (config %s, any relay %s) after %.1fs - %s\n", Avail(a), Avail(st.m_eAvailNetworkConfig),
           Avail(st.m_eAvailAnyRelay), (Now() - t0) / 1000.0, st.m_debugMsg);
    printf("authentication (cert for P2P): %s - %s\n", Avail(b), au.m_debugMsg);
    int n = SteamNetworkingUtils()->GetPOPCount();
    SteamNetworkingPOPID pops[256];
    n = SteamNetworkingUtils()->GetPOPList(pops, n < 256 ? n : 256);
    struct P { int ms; SteamNetworkingPOPID id, via; } best[256];
    int k = 0;
    for (int i = 0; i < n; i++) {
        SteamNetworkingPOPID via = 0;
        int ms = SteamNetworkingUtils()->GetPingToDataCenter(pops[i], &via);
        if (ms > 0) best[k++] = {ms, pops[i], via};
    }
    qsort(best, k, sizeof(P), [](const void* x, const void* y) { return ((const P*)x)->ms - ((const P*)y)->ms; });
    printf("relay POPs known: %d, with measured ping: %d. Nearest:", n, k);
    for (int i = 0; i < k && i < 6; i++) { char a1[8]; Pop(best[i].id, a1); printf(" %s=%dms", a1, best[i].ms); }
    printf("\n");
    SteamNetworkPingLocation_t loc;
    float age = SteamNetworkingUtils()->GetLocalPingLocation(loc);
    printf("local ping location: %s\n", age >= 0 ? "available" : "not available");
    fflush(stdout);
    SteamAPI_Shutdown();
    return 0;
}

static int Server(const char* app, const char* idfile) {
    SetApp(app);
    g_server = true;
    SteamErrMsg err = {0};
    if (SteamGameServer_InitEx(0, 27031, STEAMGAMESERVER_QUERY_PORT_SHARED, eServerModeNoAuthentication, "1.0.0.0", &err) != k_ESteamAPIInitResult_OK) {
        printf("[server] SteamGameServer init failed for %s: %s\n", app, err);
        return 1;
    }
    SteamGameServer()->SetProduct("ksnetfix-sdrtest");
    SteamGameServer()->SetGameDescription("ksnetfix sdr test");
    SteamGameServer()->SetDedicatedServer(true);
    SteamGameServer()->LogOnAnonymous();
    double t0 = Now();
    while (!SteamGameServer()->BLoggedOn() && Now() - t0 < 30000) { SteamGameServer_RunCallbacks(); Sleep(50); }
    if (!SteamGameServer()->BLoggedOn()) { printf("[server] anonymous logon failed for app %s\n", app); return 1; }
    uint64 id = SteamGameServer()->GetSteamID().ConvertToUint64();
    printf("[server] logged on as game server %llu (app %s)\n", id, app);
    SteamGameServerNetworkingSockets()->InitAuthentication();
    ServerCb cb;
    SteamNetworkingConfigValue_t opt[2];
    opt[0].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable);
    opt[1].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)OnStatus);
    HSteamListenSocket ls = SteamGameServerNetworkingSockets()->CreateListenSocketP2P(0, 1, opt);
    printf("[server] P2P listen socket %u (relays only)\n", ls);
    FILE* f = fopen(idfile, "w"); fprintf(f, "%llu", id); fclose(f);
    fflush(stdout);
    t0 = Now();
    bool printed = false;
    double lastPoll = 0;
    while (Now() - t0 < 90000 && !g_closed) {
        SteamGameServer_RunCallbacks();
        if (Now() - lastPoll > 5000) { lastPoll = Now(); PollState(SteamGameServerNetworkingSockets(), g_conn, "server"); }
        if (g_conn) {
            SteamNetworkingMessage_t* m[16];
            int n = SteamGameServerNetworkingSockets()->ReceiveMessagesOnConnection(g_conn, m, 16);
            for (int i = 0; i < n; i++) {
                SteamGameServerNetworkingSockets()->SendMessageToConnection(g_conn, m[i]->m_pData, m[i]->m_cbSize, k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
                m[i]->Release();
            }
            if (g_connected && !printed) { PrintConn(SteamGameServerNetworkingSockets(), g_conn, "server"); printed = true; }
        }
        Sleep(1);
    }
    printf("[server] done\n");
    SteamGameServer_Shutdown();
    return 0;
}

static int Client(const char* app, const char* idfile) {
    SetApp(app);
    SteamErrMsg err = {0};
    if (SteamAPI_InitEx(&err) != k_ESteamAPIInitResult_OK) { printf("[client] SteamAPI init failed: %s\n", err); return 1; }
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    SteamNetworkingSockets()->InitAuthentication();
    ClientCb cb;
    uint64 id = 0;
    for (int i = 0; i < 400 && !id; i++) {
        FILE* f = fopen(idfile, "r");
        if (f) { fscanf(f, "%llu", &id); fclose(f); }
        if (!id) Sleep(100);
    }
    if (!id) { printf("[client] no server id\n"); return 1; }
    SteamNetworkingIdentity ident;
    ident.SetSteamID64(id);
    SteamNetworkingConfigValue_t opt[2];
    opt[0].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable);
    opt[1].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)OnStatus);
    g_conn = SteamNetworkingSockets()->ConnectP2P(ident, 0, 1, opt);
    printf("[client] connecting to %llu over relays only (conn %u)\n", id, g_conn);
    fflush(stdout);
    double t0 = Now();
    double lastPoll = 0;
    while (!g_connected && !g_closed && Now() - t0 < 45000) {
        SteamAPI_RunCallbacks();
        if (Now() - lastPoll > 3000) { lastPoll = Now(); PollState(SteamNetworkingSockets(), g_conn, "client"); }
        Sleep(5);
    }
    if (!g_connected) { printf("[client] NOT CONNECTED after %.1fs\n", (Now() - t0) / 1000.0); SteamAPI_Shutdown(); return 2; }
    printf("[client] CONNECTED after %.1fs\n", (Now() - t0) / 1000.0);
    PrintConn(SteamNetworkingSockets(), g_conn, "client");
    double sum = 0, mn = 1e9, mx = 0;
    int got = 0;
    for (int i = 0; i < 30; i++) {
        double t = Now();
        SteamNetworkingSockets()->SendMessageToConnection(g_conn, &t, sizeof(t), k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
        double until = Now() + 1000;
        while (Now() < until) {
            SteamAPI_RunCallbacks();
            SteamNetworkingMessage_t* m;
            if (SteamNetworkingSockets()->ReceiveMessagesOnConnection(g_conn, &m, 1) == 1) {
                double sent = *(const double*)m->m_pData;
                double rtt = Now() - sent;
                sum += rtt; got++; if (rtt < mn) mn = rtt; if (rtt > mx) mx = rtt;
                m->Release();
                break;
            }
            Sleep(1);
        }
        Sleep(100);
    }
    printf("[client] echo over SDR: %d/30 replies, RTT avg %.1f ms (min %.1f, max %.1f)\n", got, got ? sum / got : 0, mn, mx);
    PrintConn(SteamNetworkingSockets(), g_conn, "client");
    SteamNetworkingSockets()->CloseConnection(g_conn, 0, "done", true);
    Sleep(500);
    SteamAPI_RunCallbacks();
    SteamAPI_Shutdown();
    return got ? 0 : 3;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 3 && !strcmp(argv[1], "status")) return Status(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "server")) return Server(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "client")) return Client(argv[2], argv[3]);
    printf("usage: sdrtest status <appid> | server <appid> <idfile> | client <appid> <idfile>\n");
    return 1;
}
