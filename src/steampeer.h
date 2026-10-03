// KSNetFix Steam transport (see steampeer.cpp).
#pragma once
#include <windows.h>
#include <unknwn.h>
#include <string>

struct SteamSettings {
    bool enabled = false;
    unsigned appId = 254060;       // KnightShift; 480 = Spacewar (Valve's Steamworks test app)
    bool lobbyFriendsOnly = false;
    bool verbose = false;
    bool gameServer = false;       // anonymous Steam game server (no user account / Steam client)
    char serverName[64] = "KnightShift RPG Server";
};

typedef HRESULT(__stdcall* HCoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);

void Steam_Configure(const SteamSettings& s, HCoCreate orig);
HRESULT __stdcall Steam_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID* ppv);

// SteamNet ranking on Steam leaderboards (see earthnet_core.h).
namespace en { class RankingService; class LobbyService; }
en::RankingService* Steam_Ranking();
// SteamNet channels on Steam lobbies.
en::LobbyService* Steam_Lobbies();
// The Steam account name as a SteamNet nick ("" when Steam is off) and its SteamID.
std::string Steam_AccountName();
unsigned long long Steam_AccountId();
// The Steam game session (DirectPlay over Steam): other players in it now, or when it ended for us
// (-1 before the first one), and how many left or dropped since it began. false: Steam is off.
bool Steam_MatchPeople(int& opponents, int& departed);
