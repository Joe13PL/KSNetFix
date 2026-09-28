// Out-of-game test of the Steam DirectPlay 8 peer: "host" creates a session,
// "enum" lists sessions like the game's session screen does.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdarg.h>
#include "../dp8.h"
#include "../steampeer.h"

void Log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("[log] "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout);
}
static const GUID kApp = {0x12345678, 0x1111, 0x2222, {1, 2, 3, 4, 5, 6, 7, 8}};
static IDirectPlay8Peer_* g_dp;

HRESULT WINAPI Handler(PVOID, DWORD msg, PVOID p) {
    switch (msg) {
    case DPN_MSGID_CREATE_PLAYER_: {
        auto* m = (DPNMSG_CREATE_PLAYER_*)p;
        DWORD sz = 0;
        HRESULT hr = g_dp->GetPeerInfo(m->dpnidPlayer, nullptr, &sz, 0);
        auto* info = (DPN_PLAYER_INFO_*)calloc(1, sz);
        info->dwSize = sizeof(DPN_PLAYER_INFO_);
        hr = g_dp->GetPeerInfo(m->dpnidPlayer, info, &sz, 0);
        printf("CREATE_PLAYER %08X name=%ls flags=%u (GetPeerInfo hr=%08X)\n", m->dpnidPlayer, info->pwszName, info->dwPlayerFlags, (unsigned)hr);
        free(info);
        break;
    }
    case DPN_MSGID_DESTROY_PLAYER_: printf("DESTROY_PLAYER %08X reason %u\n", ((DPNMSG_DESTROY_PLAYER_*)p)->dpnidPlayer, ((DPNMSG_DESTROY_PLAYER_*)p)->dwReason); break;
    case DPN_MSGID_ENUM_HOSTS_RESPONSE_: {
        auto* m = (DPNMSG_ENUM_HOSTS_RESPONSE_*)p;
        auto* d = m->pApplicationDescription;
        WCHAR host[260] = {0}; DWORD sz = sizeof(host), type = 0;
        m->pAddressSender->GetComponentByName(L"hostname", host, &sz, &type);
        IDirectPlay8Address_* dup = nullptr;
        m->pAddressSender->Duplicate(&dup);
        printf("ENUM_HOSTS_RESPONSE \"%ls\" players %u/%u rtt %ums addr=%ls resv=%u bytes isEqual(dup)=%d\n", d->pwszSessionName,
               d->dwCurrentPlayers, d->dwMaxPlayers, m->dwRoundTripLatencyMS, host, d->dwApplicationReservedDataSize,
               dup ? dup->IsEqual(m->pAddressSender) == S_OK : -1);
        if (dup) dup->Release();
        break;
    }
    case DPN_MSGID_ASYNC_OP_COMPLETE_: printf("ASYNC_OP_COMPLETE hr=%08X\n", (unsigned)((DPNMSG_ASYNC_OP_COMPLETE_*)p)->hResultCode); break;
    default: printf("msg %08X\n", msg);
    }
    fflush(stdout);
    return S_OK;
}

int main(int argc, char** argv) {
    CoInitialize(nullptr);
    SteamSettings s; s.enabled = true; s.appId = 480; s.verbose = true;
    Steam_Configure(s, (HCoCreate)&CoCreateInstance);
    if (FAILED(Steam_CoCreateInstance(CLSID_DirectPlay8Peer_, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectPlay8Peer_, (void**)&g_dp)) || !g_dp) {
        printf("no peer\n"); return 1;
    }
    printf("Initialize: %08X\n", (unsigned)g_dp->Initialize(nullptr, Handler, 0));
    DWORD cb = 0, n = 0;
    HRESULT hr = g_dp->EnumServiceProviders(nullptr, nullptr, nullptr, &cb, &n, 0);
    auto* sp = (DPN_SERVICE_PROVIDER_INFO_*)calloc(1, cb);
    hr = g_dp->EnumServiceProviders(nullptr, nullptr, sp, &cb, &n, 0);
    printf("EnumServiceProviders: %08X count=%u name=%ls\n", (unsigned)hr, n, sp->pwszName);
    DPN_PLAYER_INFO_ pi = {sizeof(pi), DPNINFO_NAME_, (PWSTR)L"Tester", nullptr, 0, 0};
    printf("SetPeerInfo: %08X\n", (unsigned)g_dp->SetPeerInfo(&pi, nullptr, nullptr, DPNOP_SYNC_));
    if (argc > 1 && !strcmp(argv[1], "host")) {
        BYTE resv[6] = {1, 2, 3, 4, 5, 6};
        DPN_APPLICATION_DESC_ ad = {sizeof(ad)};
        ad.dwFlags = 4; ad.guidApplication = kApp; ad.dwMaxPlayers = 8; ad.pwszSessionName = (WCHAR*)L"KSNetFix test sesja";
        ad.pvApplicationReservedData = resv; ad.dwApplicationReservedDataSize = 6;
        printf("Host: %08X\n", (unsigned)g_dp->Host(&ad, nullptr, 0, nullptr, nullptr, nullptr, 0));
        DWORD sz = 0;
        g_dp->GetApplicationDesc(nullptr, &sz, 0);
        auto* got = (DPN_APPLICATION_DESC_*)calloc(1, sz);
        HRESULT gh = g_dp->GetApplicationDesc(got, &sz, 0);
        printf("GetApplicationDesc: %08X name=%ls max=%u resv=%u\n", (unsigned)gh, got->pwszSessionName, got->dwMaxPlayers,
               got->dwApplicationReservedDataSize);
        Sleep(atoi(argc > 2 ? argv[2] : "25") * 1000);
    } else {
        IDirectPlay8Address_* dev = nullptr;
        DPN_APPLICATION_DESC_ ad = {sizeof(ad)};
        ad.guidApplication = kApp;
        DPNHANDLE h = 0;
        printf("EnumHosts: %08X\n", (unsigned)g_dp->EnumHosts(&ad, nullptr, dev, nullptr, 0, INFINITE, 0, INFINITE, nullptr, &h, 0));
        Sleep(12000);
        printf("Cancel: %08X\n", (unsigned)g_dp->CancelAsyncOperation(h, 0));
        Sleep(500);
    }
    printf("Close: %08X\n", (unsigned)g_dp->Close(0));
    g_dp->Release();
    fflush(stdout);
    ExitProcess(0);
}
