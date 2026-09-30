// SteamNet - replacement for the dead EarthNet lobby (earthnet.cpp, earthnet_core.cpp).
// docs/STEAMNET_PROTOCOL.md (client protocol).
#pragma once
#include <stdint.h>
#include <string>

struct SteamNetSettings {
    bool enabled = true;
    wchar_t name[9] = L"SteamNet";  // shown instead of "EarthNet" (at most 8 characters)
    char address[64] = "steam";      // server list entry; also accepted: netserver.earthnet.de
    char channel[64] = "KnightShift";
    char welcome[256] = "Witaj w SteamNet!";
    bool trace = false;              // log every line exchanged with the game
    bool ranking = true;             // ranking on Steam leaderboards (needs the Steam transport)
    bool steamLogin = true;          // log in with the Steam account name (no login window)
};

void SteamNet_LoadConfig(const char* iniPath, SteamNetSettings& s);

struct SteamNetGameAddrs {
    uint32_t nameString;    // L"EarthNet" in the connection type list
    uint32_t identity;      // u64 identity from the CD key (login signature)
    uint32_t client = 0;    // global holding the EarthNet client object (0 = no Steam login)
    uint32_t memAlloc = 0;  // the game's allocator, void* __cdecl(size_t)
};

// Renames the menu entry and hooks the game's winsock lookups. Call from DllMain.
bool SteamNet_Install(const SteamNetSettings& s, const SteamNetGameAddrs& game);

// Where the ranking is kept (Steam leaderboards); without it the ranking stays empty.
namespace en { class RankingService; }
void SteamNet_SetRanking(en::RankingService* r);
// The Steam account SteamNet logs in with: name in the game's ANSI code page ("" = Steam not
// available) and the SteamID, which keys saved RPG heroes so a new Steam name keeps them.
struct SteamNetAccount {
    std::string name;
    uint64_t id = 0;
};
void SteamNet_SetAccount(SteamNetAccount (*account)());
