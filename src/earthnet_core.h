// SteamNet - EarthNet server emulator, protocol core (no Windows dependencies).
//
// The game's EarthNet client (see docs/STEAMNET_PROTOCOL.md) talks to one Session over a
// loopback TCP connection. The Session speaks the EarthNet wire protocol; everything
// about other players (channels, chat, games) comes from a Backend - offline for now,
// Steam lobbies later. All Session methods run on one thread.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace en {

// 64-byte login signature the client checks against its own identity (0x8069E0).
void Sign(uint64_t identity, uint8_t out[64]);

// Binary packets carry a zlib stream (the game's stream class 0x7995D0 with flags 0x6001 /
// 0x6002 inflates / deflates). We write stored blocks and read anything zlib produces.
std::vector<uint8_t> ZlibStore(const uint8_t* p, size_t n);
bool Inflate(const uint8_t* p, size_t n, std::vector<uint8_t>& out, size_t maxOut = 1 << 20);

// "text" with the characters the client would mangle (%, ") encoded as %XX.
std::string Quote(const std::string& s);
// Splits a client line into words; "quoted words" may contain spaces.
std::vector<std::string> Tokenize(const std::string& line);
// A Steam name (already in the game's ANSI code page) as a SteamNet nick: no control
// characters, no " or %, single spaces, at most 16 bytes. Empty if nothing is left.
std::string SanitizeNick(const std::string& ansi);

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }
    void raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void str(const std::string& s) { u32((uint32_t)s.size()); raw(s.data(), s.size()); }
    std::vector<uint8_t> Packet() const; // u32 total length + zlib(body), as on the wire
};

struct Reader {
    const uint8_t* p;
    size_t n, pos = 0;
    bool ok = true;
    Reader(const uint8_t* d, size_t len) : p(d), n(len) {}
    bool Need(size_t k) { if (pos + k > n) ok = false; return ok; }
    uint32_t u32() { if (!Need(4)) return 0; uint32_t v = p[pos] | p[pos + 1] << 8 | p[pos + 2] << 16 | (uint32_t)p[pos + 3] << 24; pos += 4; return v; }
    std::string str(size_t maxLen = 4096) {
        uint32_t k = u32();
        if (!ok || k > maxLen || !Need(k)) { ok = false; return std::string(); }
        std::string s((const char*)p + pos, k); pos += k; return s;
    }
    void skip(size_t k) { if (Need(k)) pos += k; }
};

// Text on the wire is in the game's ANSI code page; the Backend sees the same bytes.
struct LoginInfo {
    std::string login, password;
    bool reconnect = false;
};

// One ranking row as the ladder screen (0x82CF60) shows it; "games" is wins + losses + disconnects.
struct LadderRow {
    std::string nick;
    double lastPlayed = 0; // OLE automation date (days since 1899-12-30, UTC)
    int wins = 0, losses = 0, disconnects = 0, points = 0;
};
enum LadderPeriod { LADDER_ALL, LADDER_MONTH, LADDER_WEEK };

class Session;

class Backend {
  public:
    virtual ~Backend() {}
    // The player logged in and entered the first channel; the backend announces what it knows.
    virtual void OnLogin(Session& s) = 0;
    virtual void OnJoin(Session& s, const std::string& channel, const std::string& password) = 0;
    virtual void OnSay(Session& s, const std::string& text) = 0;
    virtual void OnWhisper(Session& s, const std::string& to, const std::string& text) = 0;
    virtual void OnLogout(Session& s) = 0;
    // "New RTS/RPG game": default approves at once; the client then hosts and calls OnGameHosted.
    virtual void OnHostRequest(Session& s, const std::string& name, const std::string& password);
    virtual void OnGameHosted(Session& s, const std::string& name, const std::string& guid) { (void)s; (void)name; (void)guid; }
    // Ranking tabs; at most 10 rows are shown (the client has room for no more).
    virtual std::vector<LadderRow> OnLadder(Session& s, LadderPeriod period) { (void)s; (void)period; return {}; }
    // Any other client command, for tracing and later features.
    virtual void OnCommand(Session& s, const std::vector<std::string>& words) { (void)s; (void)words; }
};

// Where RPG heroes (/setplayerdata) are kept between sessions.
class PlayerStore {
  public:
    virtual ~PlayerStore() {}
    virtual bool Load(const std::string& nick, const std::string& key, std::vector<uint8_t>& data) = 0;
    virtual void Save(const std::string& nick, const std::string& key, const std::vector<uint8_t>& data) = 0;
};

struct SessionConfig {
    std::string welcome = "SteamNet";
    std::string channel = "KnightShift";
    std::string askName = "Podaj nazwe gracza (dowolna) i kliknij OK."; // no % (format string)
    uint64_t identity = 0;          // the game's own identity (login signature)
    std::string accountNick;        // Steam account name; what other players see (empty: the login)
    std::string accountKey;         // stable account id for saved data, e.g. "steam_7656..." (empty: the nick)
    std::string previousLogin;      // the game's own login before SteamNet replaced it (old saved data)
    std::function<void()> beforeHello; // runs right before the hello reply (the client logs in on it)
};

class Session {
  public:
    typedef std::function<void(const uint8_t*, size_t)> SendFn;
    typedef std::function<void(const char*)> LogFn;

    Session(const SessionConfig& cfg, Backend& backend, PlayerStore& store, SendFn send, LogFn log);

    // Bytes from the game. Returns false when the connection should be closed.
    bool OnData(const uint8_t* p, size_t n);
    void OnClosed();

    const std::string& Nick() const { return nick_; }      // as the game logged in
    const std::string& PublicName() const { return cfg_.accountNick.empty() ? nick_ : cfg_.accountNick; }
    const std::string& Channel() const { return channel_; }
    bool LoggedIn() const { return state_ == LINES || state_ == BINARY; }

    // Server -> game. Names and texts are ANSI.
    void UserEntered(const std::string& nick, const std::string& guid = std::string());
    void UserLeft(const std::string& nick, const std::string& where = std::string());
    void ChannelAdded(const std::string& name, const std::string& topic = std::string());
    void ChannelRemoved(const std::string& name);
    void EnteredChannel(const std::string& name, const std::string& topic = std::string());
    void ChannelMessage(const std::string& from, const std::string& text);
    void WhisperFrom(const std::string& from, const std::string& text);
    void WhisperTo(const std::string& to, const std::string& text);
    void AdminMessage(const std::string& text);
    void GameAdded(const std::string& name, uint32_t ipv4, const std::string& guid);
    void GameUpdated(const std::string& name, int players, int maxPlayers, const std::string& level);
    void GameRemoved(const std::string& name);
    // Counters above the chat: players (logged in / all), games (open / all), channels.
    void Stats(int players, int allPlayers, int games, int allGames, int channels);
    void Ladder(LadderPeriod period, const std::vector<LadderRow>& rows);
    void Line(const std::string& line); // raw line (without the terminating zero)

  private:
    enum State { CLIENT_INFO, LOGIN, LINES, BINARY, CLOSED };

    void Packet(const Writer& w);
    bool OnPacket(const uint8_t* p, size_t n);
    void OnClientLine(const std::string& line);
    void OnBinary(const std::vector<uint8_t>& data);
    void Log(const char* fmt, ...);
    bool OwnName(const std::string& nick) const { return nick == nick_ || nick == PublicName(); }

    SessionConfig cfg_;
    Backend& backend_;
    PlayerStore& store_;
    SendFn send_;
    LogFn log_;
    State state_ = CLIENT_INFO;
    std::vector<uint8_t> in_;
    std::string nick_, channel_;
    // pending binary block after a /setplayerdata line
    size_t binNeed_ = 0;
    std::string binNick_, binKey_;
};

// Offline backend: one channel, the player alone; echoes chat. Stand-in until Steam lobbies.
class LocalBackend : public Backend {
  public:
    void OnLogin(Session& s) override;
    void OnJoin(Session& s, const std::string& channel, const std::string& password) override;
    void OnGameHosted(Session& s, const std::string& name, const std::string& guid) override;
    void OnSay(Session& s, const std::string& text) override;
    void OnWhisper(Session& s, const std::string& to, const std::string& text) override;
    void OnLogout(Session& s) override { (void)s; }

  private:
    void SendStats(Session& s);
    std::vector<std::string> channels_, games_;
};

// Online ranking kept on Steam leaderboards (steampeer.cpp). One entry per Steam account:
// score = points, details = RankingDetails(). Calls block the connection thread briefly.
struct BoardEntry {
    std::string steamName;
    int score = 0;
    std::vector<int32_t> details;
};
class RankingService {
  public:
    virtual ~RankingService() {}
    // Best `count` entries; create=false: a board that does not exist yet reads as empty.
    virtual bool Top(const std::string& board, bool create, int count, std::vector<BoardEntry>& out) = 0;
    // Puts the player on the board with score/details unless they are on it already; an existing
    // entry is rewritten (same score) when refresh changes its details. refresh may run on any thread.
    virtual void Join(const std::string& board, int score, const std::vector<int32_t>& details,
                      std::function<bool(std::vector<int32_t>&)> refresh) = 0;
};

// Board names: "SteamNet" (all time), "SteamNet 2026-09" (month), "SteamNet 2026-W40" (ISO week).
std::string RankingBoard(LadderPeriod period, int64_t unixTime);
// details: {1 (layout), wins, losses, disconnects, last game (unix time), nick in 16 bytes}
std::vector<int32_t> RankingDetails(const std::string& nick, int wins, int losses, int disconnects, int64_t lastGame);
LadderRow RankingRow(const BoardEntry& e);
// Puts nick into details of layout 1; false if they already had it (or another layout).
bool RankingSetNick(std::vector<int32_t>& details, const std::string& nick);

// LocalBackend plus the ranking from a RankingService.
class RankedBackend : public LocalBackend {
  public:
    RankedBackend(RankingService& r, std::function<int64_t()> now) : ranking_(r), now_(std::move(now)) {}
    void OnLogin(Session& s) override;
    std::vector<LadderRow> OnLadder(Session& s, LadderPeriod period) override;

  private:
    RankingService& ranking_;
    std::function<int64_t()> now_;
};

class MemoryStore : public PlayerStore {
  public:
    std::map<std::string, std::vector<uint8_t>> items;
    bool Load(const std::string& nick, const std::string& key, std::vector<uint8_t>& data) override;
    void Save(const std::string& nick, const std::string& key, const std::vector<uint8_t>& data) override;
};

} // namespace en
