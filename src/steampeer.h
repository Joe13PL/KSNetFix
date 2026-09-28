// KSNetFix Steam transport (see steampeer.cpp).
#pragma once
#include <windows.h>
#include <unknwn.h>

struct SteamSettings {
    bool enabled = false;
    unsigned appId = 254060;       // KnightShift; 480 = Spacewar (Valve's Steamworks test app)
    bool lobbyFriendsOnly = false;
    bool verbose = false;
};

typedef HRESULT(__stdcall* HCoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);

void Steam_Configure(const SteamSettings& s, HCoCreate orig);
HRESULT __stdcall Steam_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID* ppv);
