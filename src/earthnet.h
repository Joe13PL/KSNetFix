// SteamNet - replacement for the dead EarthNet lobby (earthnet.cpp, earthnet_core.cpp).
// docs/STEAMNET_PROTOCOL.md (client protocol).
#pragma once
#include <stdint.h>

struct SteamNetSettings {
    bool enabled = true;
    wchar_t name[9] = L"SteamNet";  // shown instead of "EarthNet" (at most 8 characters)
    char address[64] = "steam";      // server list entry; also accepted: netserver.earthnet.de
    char channel[64] = "KnightShift";
    char welcome[256] = "Witaj w SteamNet!";
    bool trace = false;              // log every line exchanged with the game
    bool ranking = true;             // ranking on Steam leaderboards (needs the Steam transport)
};

void SteamNet_LoadConfig(const char* iniPath, SteamNetSettings& s);

struct SteamNetGameAddrs {
    uint32_t nameString;    // L"EarthNet" in the connection type list
    uint32_t identity;      // u64 identity from the CD key (login signature)
};

// Renames the menu entry and hooks the game's winsock lookups. Call from DllMain.
bool SteamNet_Install(const SteamNetSettings& s, const SteamNetGameAddrs& game);

// Where the ranking is kept (Steam leaderboards); without it the ranking stays empty.
namespace en { class RankingService; }
void SteamNet_SetRanking(en::RankingService* r);
