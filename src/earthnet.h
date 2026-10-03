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
    bool online = true;              // channels, players and chat shared through Steam lobbies
    bool banner = true;              // SteamNet banner above the lobby chat instead of EarthNet's
};

void SteamNet_LoadConfig(const char* iniPath, SteamNetSettings& s);

struct SteamNetGameAddrs {
    uint32_t nameString;    // L"EarthNet" in the connection type list
    uint32_t identity;      // u64 identity from the CD key (login signature)
    uint32_t client = 0;       // global holding the EarthNet client object (0 = no Steam login)
    uint32_t clientVtable = 0; // the client's vtable (checks a pointer really is a client)
    uint32_t lookupBuffer = 0; // offset of the host lookup buffer in the client object
    uint32_t memAlloc = 0;     // the game's allocator, void* __cdecl(size_t)
    uint32_t loginGlobal = 0;  // the saved login (ANSI string object; profile, "X entered the channel")
    uint32_t matchFlag = 0;    // non-zero while an EarthNet match is played (set by 0x82E890)
    uint32_t resultSetter = 0; // int __cdecl(int): the player's match result, 1 won / 0 lost / 2 not rated
    uint32_t resultGlobal = 0; // where it keeps the result (only for rated EarthNet matches)
    uint32_t quitCall = 0;     // its call from the in-game menu's "quit" (0: lost by leaving)
    uint32_t defeatCall = 0;   // its call from the local player's defeat (2 outside rated matches)
    uint32_t victoryCalls[2] = {0, 0}; // its calls from the local player's victory (2 outside rated matches)
    uint32_t fileOpen = 0;     // int __thiscall(file, name, flags): opens a game file (archives or disk)
};

// Renames the menu entry and hooks the game's winsock lookups. Call from DllMain.
bool SteamNet_Install(const SteamNetSettings& s, const SteamNetGameAddrs& game);

// Where the ranking is kept (Steam leaderboards); without it the ranking stays empty.
namespace en { class RankingService; class LobbyService; }
void SteamNet_SetRanking(en::RankingService* r);
// Channels, players and chat shared through Steam lobbies; without it SteamNet is offline.
void SteamNet_SetLobbies(en::LobbyService* l);
// The Steam account SteamNet logs in with: name in the game's ANSI code page ("" = Steam not
// available) and the SteamID, which keys saved RPG heroes so a new Steam name keeps them.
struct SteamNetAccount {
    std::string name;
    uint64_t id = 0;
};
void SteamNet_SetAccount(SteamNetAccount (*account)());
// Who is in the Steam game session: other players now (or when it ended for us) and how many
// left or dropped since it began; false when unknown. Decides the win of the last one in a match.
void SteamNet_SetMatchPeople(bool (*people)(int& opponents, int& departed));
// The lobby banner served instead of Banners\BannerDef.tex (the game's texture format, 640x128);
// written to SteamNet\Banner.tex in the game's output dir (the game folder) when the lobby first
// shows it. Call before the install.
void SteamNet_SetBanner(const void* tex, size_t size);
