// SteamNet protocol core test - builds and runs anywhere (no Windows, no Steam, no game):
//   g++ -std=c++17 -Wall -I.. earthnet_test.cpp ../earthnet_core.cpp -lz -o earthnet_test && ./earthnet_test
// zlib (the real library) packs the fake client's packets and unpacks the server's, like the game.
// A fake EarthNet client sends what KnightShift.ex1 sends and parses the replies in the order
// the game reads them (docs/STEAMNET_PROTOCOL.md). Reference signatures come from running the
// game's own 0x8069E0 under an x86 emulator.
#include "earthnet_core.h"

#include <stdio.h>
#include <zlib.h>

#include <algorithm>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
            g_fail++;                                                     \
        }                                                                 \
    } while (0)

static std::vector<uint8_t> Hex(const char* s) {
    std::vector<uint8_t> v;
    for (; s[0] && s[1]; s += 2) {
        char b[3] = {s[0], s[1], 0};
        v.push_back((uint8_t)strtoul(b, nullptr, 16));
    }
    return v;
}

static void TestSignature() {
    struct {
        uint64_t id;
        const char* sig;
    } ref[] = {
        {0x0123456789abcdefull, "3e592987d284cef74370f76f4e0483ec984d64d0bd32ad427c8c3b30da925ccd7bf2b8df69ec7a26513b839b92c4c54bd3e9dabca2382439754ce5d744f72283"},
        {0, "f435027db0c4300522350d685beaf6b0cba8426f2ca6a3c63e74a584dac7fd03b25bad0ee5b050437536d6a186459e2cd786e2332c29ec6dcda4250245dfd656"},
        {0xfedcba9876543210ull, "102220b3df0ef33fcfc83a9592b50a4ce93d5c1609ad2449f765a0f2eaf977a84dd6c3805dfd89dce9d37478368745854cc6449b1c45781ec71314e029382d06"},
        {0xdeadbeefull, "afb182a6ec7ae9f416a335a918ec3bc20f8fb9e821f08bd6600a756135efb62af5aabb3658ec0adce00b41b0d946059fa00c3f014b7d631837ac466614d5a97c"},
    };
    for (auto& r : ref) {
        uint8_t out[64];
        en::Sign(r.id, out);
        CHECK(Hex(r.sig) == std::vector<uint8_t>(out, out + 64));
    }
}

// Packet as the game sends it: u32 length + zlib stream (deflate level 6).
static std::vector<uint8_t> GamePacket(const en::Writer& w) {
    uLongf n = compressBound(w.b.size());
    std::vector<uint8_t> z(n);
    compress2(z.data(), &n, w.b.data(), w.b.size(), 6);
    z.resize(n);
    en::Writer p;
    p.u32((uint32_t)n + 4);
    p.raw(z.data(), z.size());
    return p.b;
}

static bool Unzip(const std::vector<uint8_t>& z, std::vector<uint8_t>& out) {
    out.assign(1 << 20, 0);
    uLongf n = out.size();
    if (uncompress(out.data(), &n, z.data(), z.size()) != Z_OK) return false;
    out.resize(n);
    return true;
}

static void TestZlib() {
    std::vector<uint8_t> data;
    for (int i = 0; i < 70000; i++) data.push_back((uint8_t)(i * 7 % 13 + (i / 1000)));
    for (int level : {0, 1, 6, 9}) {
        uLongf n = compressBound(data.size());
        std::vector<uint8_t> z(n);
        compress2(z.data(), &n, data.data(), data.size(), level);
        std::vector<uint8_t> out;
        CHECK(en::Inflate(z.data(), n, out));
        CHECK(out == data);
        z[n - 1] ^= 1; // bad checksum
        CHECK(!en::Inflate(z.data(), n, out));
    }
    // the client information packet from a real game (KnightShift 1.3, KSNetFix 2.5 log)
    auto info = Hex("789cb399ec94a113aae9b4554a68f3aa1aa50666060686007f9f75c202fc01767fa30198c90976");
    std::vector<uint8_t> out;
    CHECK(en::Inflate(info.data(), info.size(), out));
    CHECK(out.size() == 31 && memcmp(out.data() + 20, "POL", 3) == 0); // GUID, "POL" (u32 length + text), 8 bytes
    // our stored stream, read back by zlib
    for (size_t len : {(size_t)0, (size_t)5, (size_t)65535, (size_t)65536, data.size()}) {
        auto z = en::ZlibStore(data.data(), len);
        CHECK(Unzip(z, out) && out == std::vector<uint8_t>(data.begin(), data.begin() + len));
    }
}

// The client side, as far as the game's reading code goes.
struct FakeClient {
    std::vector<uint8_t> in;   // bytes from the server
    std::vector<std::string> lines;

    bool TakePacket(std::vector<uint8_t>& body) {
        if (in.size() < 4) return false;
        uint32_t len = in[0] | in[1] << 8 | in[2] << 16 | (uint32_t)in[3] << 24;
        if (in.size() < len) return false;
        std::vector<uint8_t> z(in.begin() + 4, in.begin() + len);
        in.erase(in.begin(), in.begin() + len);
        return Unzip(z, body);
    }
    void TakeLines() {
        for (;;) {
            auto z = std::find(in.begin(), in.end(), 0);
            if (z == in.end()) return;
            lines.push_back(std::string(in.begin(), z));
            in.erase(in.begin(), z + 1);
        }
    }
};

// The game's tokenizer for server lines (0x808210): quoted words, %XX decoded.
static std::vector<std::string> GameTokens(const std::string& l) {
    auto w = en::Tokenize(l);
    for (auto& s : w) {
        std::string d;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '%' && i + 2 < s.size() + 0 && i + 2 <= s.size() - 1) {
                char b[3] = {s[i + 1], s[i + 2], 0};
                d += (char)strtoul(b, nullptr, 16);
                i += 2;
            } else {
                d += s[i];
            }
        }
        s = d;
    }
    return w;
}

static void TestSession() {
    FakeClient c;
    en::SessionConfig cfg;
    cfg.welcome = "Witaj w SteamNet";
    cfg.channel = "KnightShift";
    cfg.identity = 0x0123456789abcdefull;
    en::LocalBackend backend;
    en::MemoryStore store;
    std::vector<std::string> log;
    en::Session s(cfg, backend, store, [&](const uint8_t* p, size_t n) { c.in.insert(c.in.end(), p, p + n); },
                  [&](const char* t) { log.push_back(t); });

    // 1. client information (contents do not matter), sent in two pieces
    en::Writer info;
    info.raw("KS-1.3-client-info", 18);
    auto pkt = GamePacket(info);
    CHECK(s.OnData(pkt.data(), 5));
    CHECK(c.in.empty());
    CHECK(s.OnData(pkt.data() + 5, pkt.size() - 5));
    std::vector<uint8_t> body;
    CHECK(c.TakePacket(body));
    {
        en::Reader r(body.data(), body.size()); // 0x802F30
        CHECK(r.u32() == 0);
        CHECK(r.str() == "Witaj w SteamNet");
        CHECK(r.ok && r.pos == body.size());
    }

    // 2a. first connect from the server list: empty name -> error, connection stays open
    en::Writer anon;
    anon.str("");
    anon.str("");
    anon.u32(0);
    anon.u32(0);
    anon.u32(0);
    uint8_t zero[16] = {0};
    anon.raw(zero, 16);
    auto apkt = GamePacket(anon);
    CHECK(s.OnData(apkt.data(), apkt.size()));
    CHECK(c.TakePacket(body));
    {
        en::Reader r(body.data(), body.size());
        CHECK(r.u32() != 0);
        std::string t = r.str();
        CHECK(!t.empty() && t[0] == '"' && t.find('%') == std::string::npos);
    }
    CHECK(!s.LoggedIn());

    // 2b. the login window sends the name on the same connection (0x819570)
    en::Writer login;
    login.str("Joe");
    login.str("haslo");
    login.u32(0);
    login.u32(1);
    uint8_t guid[16] = {1, 2, 3};
    login.raw(guid, 16);
    login.u32(0); // not a reconnect
    login.raw(guid, 16);
    pkt = GamePacket(login);
    CHECK(s.OnData(pkt.data(), pkt.size()));
    CHECK(c.TakePacket(body));
    {
        en::Reader r(body.data(), body.size()); // 0x803820, success path
        CHECK(r.u32() == 0);
        r.u32();
        CHECK(r.str() == "Witaj w SteamNet");
        CHECK(r.str() == "Witaj w SteamNet");
        r.skip(8);
        for (int i = 0; i < 9; i++) r.u32();
        for (int list = 0; list < 3; list++) {
            for (;;) {
                if (!r.Need(1)) break;
                uint8_t idx = r.p[r.pos++];
                if (idx == 0xFF) break;
                r.str();
            }
        }
        r.skip(1);
        CHECK(r.str() == "KnightShift");
        for (int i = 0; i < 2; i++) {
            CHECK(r.u32() == 0);
            r.skip(16);
        }
        CHECK(r.ok && r.pos + 64 == body.size());
        uint8_t sig[64];
        en::Sign(cfg.identity, sig);
        CHECK(memcmp(sig, body.data() + r.pos, 64) == 0);
    }
    CHECK(s.LoggedIn());
    CHECK(s.Nick() == "Joe");

    // 3. lines after login
    c.TakeLines();
    CHECK(c.lines.size() == 3);
    if (c.lines.size() == 3) {
        auto t = GameTokens(c.lines[0]);
        CHECK(t.size() == 5 && t[0] == "$channel" && t[1] == "KnightShift");
        CHECK(c.lines[1] == "/syncstats 1 1 1 0 0 0 0"); // all/logged players, channels, running/open games
        t = GameTokens(c.lines[2]);
        CHECK(t.size() == 3 && t[0] == "/send" && t[1] == "SteamNet");
    }
    c.lines.clear();

    // 4. chat, with characters that must be escaped
    std::string say = "/send \"czesc 100% \"ok\"\"";
    say.push_back('\0');
    CHECK(s.OnData((const uint8_t*)say.data(), say.size()));
    c.TakeLines();
    CHECK(c.lines.size() == 1);
    if (!c.lines.empty()) {
        auto t = GameTokens(c.lines[0]);
        CHECK(t.size() == 3 && t[0] == "/send" && t[1] == "Joe" && t[2] == "czesc 100% \"ok\"");
        printf("  chat line: %s\n", c.lines[0].c_str());
    }
    c.lines.clear();

    // 5. whisper to self
    std::string w = "/send \"/msg Joe tajne\"";
    w.push_back('\0');
    CHECK(s.OnData((const uint8_t*)w.data(), w.size()));
    c.TakeLines();
    CHECK(c.lines.size() == 2);
    if (c.lines.size() == 2) {
        auto t = GameTokens(c.lines[0]);
        CHECK(t.size() == 3 && t[0] == "/msgc" && t[1] == "Joe" && t[2] == "tajne");
        t = GameTokens(c.lines[1]);
        CHECK(t.size() == 5 && t[0] == "/msg" && t[2] == "Joe" && t[4] == "tajne");
    }
    c.lines.clear();

    // 6. RPG hero: /setplayerdata + raw bytes, then /getplayerdata returns them
    std::string set = "/setplayerdata \"Joe\" \"KS_RPG_ChData.1.0\" \"5\"";
    set.push_back('\0');
    set += std::string("\x01\x00\x02\x00\x03", 5);
    std::string get = "/getplayerdata \"Joe\" \"KS_RPG_ChData.1.0\"";
    get.push_back('\0');
    std::string both = set + get;
    CHECK(s.OnData((const uint8_t*)both.data(), both.size()));
    auto z = std::find(c.in.begin(), c.in.end(), 0);
    CHECK(z != c.in.end());
    if (z != c.in.end()) {
        std::string line(c.in.begin(), z);
        auto t = GameTokens(line);
        CHECK(t.size() == 4 && t[0] == "/getplayerdata" && t[3] == "5");
        std::vector<uint8_t> rest(z + 1, c.in.end());
        CHECK(rest == std::vector<uint8_t>({1, 0, 2, 0, 3}));
    }
    c.in.clear();

    // 7. unknown hero -> size 0
    std::string get2 = "/getplayerdata \"Ktos\" \"KS_RPG_ChData.1.0\"";
    get2.push_back('\0');
    CHECK(s.OnData((const uint8_t*)get2.data(), get2.size()));
    c.TakeLines();
    CHECK(c.lines.size() == 1 && GameTokens(c.lines[0]).size() == 4 && GameTokens(c.lines[0])[3] == "0");
    c.lines.clear();

    // 7b. chat as the game really sends it (0x8268C0): /msg "#channel" / /msg "nick"
    std::string m1 = "/msg \"#KnightShift\" \"czesc \"wszystkim\"\"";
    m1.push_back('\0');
    CHECK(s.OnData((const uint8_t*)m1.data(), m1.size()));
    c.TakeLines();
    CHECK(c.lines.size() == 1);
    if (!c.lines.empty()) {
        auto t = GameTokens(c.lines[0]);
        CHECK(t.size() == 3 && t[0] == "/send" && t[1] == "Joe" && t[2] == "czesc \"wszystkim\"");
    }
    c.lines.clear();
    std::string m2 = "/msg \"Joe\" \"tylko do mnie\"";
    m2.push_back('\0');
    CHECK(s.OnData((const uint8_t*)m2.data(), m2.size()));
    c.TakeLines();
    CHECK(c.lines.size() == 2 && GameTokens(c.lines[0])[0] == "/msgc" && GameTokens(c.lines[1])[0] == "/msg");
    c.lines.clear();

    // 7c. new game: request -> approval -> client hosts and registers -> game on the list
    std::string h1 = "/plays \"00000000-0000-0000-0000-000000000000\" \"RTS : test\" \"\"";
    h1.push_back('\0');
    CHECK(s.OnData((const uint8_t*)h1.data(), h1.size()));
    c.TakeLines();
    CHECK(c.lines.size() == 1 && c.lines[0] == "/plays \"RTS : test\" \"\"");
    c.lines.clear();
    std::string h2 = "/plays \"RTS : test\" \"\" \"04030201-0605-0807-090a-0b0c0d0e0f10\"";
    h2.push_back('\0');
    CHECK(s.OnData((const uint8_t*)h2.data(), h2.size()));
    c.TakeLines();
    CHECK(c.lines.size() == 2);
    if (c.lines.size() == 2) {
        auto t = GameTokens(c.lines[0]);
        CHECK(t.size() == 6 && t[0] == "$play" && t[1] == "RTS : test" && t[5] == "04030201-0605-0807-090a-0b0c0d0e0f10");
        CHECK(c.lines[1] == "/syncstats 1 1 1 0 1 0 0");
    }
    c.lines.clear();

    // 7d. ranking and profile update
    std::string l1 = "/ladderm";
    l1.push_back('\0');
    l1 += "/update \"Joe\" \"\" \"4294967295\" \"\" \"255\" \"255\" \"\"";
    l1.push_back('\0');
    CHECK(s.OnData((const uint8_t*)l1.data(), l1.size()));
    c.TakeLines();
    CHECK(c.lines.size() == 1 && c.lines[0] == "/ladderm 0"); // category 0, no rows
    c.lines.clear();
    std::vector<en::LadderRow> rows(12);
    rows[0].nick = "Test";
    rows[0].lastPlayed = 46295.75;
    rows[0].wins = 12, rows[0].losses = 9, rows[0].disconnects = 3, rows[0].points = 150;
    for (size_t i = 1; i < rows.size(); i++) rows[i].nick = "Gracz " + std::to_string(i), rows[i].points = -5;
    s.Ladder(en::LADDER_WEEK, rows);
    c.TakeLines();
    CHECK(c.lines.size() == 1);
    if (c.lines.size() == 1) {
        auto t = GameTokens(c.lines[0]);
        CHECK(t.size() == 2 + 10 * 9); // at most 10 rows of 9 words (0x80A6F0 has room for 10)
        CHECK(c.lines[0].rfind("/ladderw 0 \"Test\" \"46295.750000\" 12 9 3 150 0 0 0 \"Gracz 1\" ", 0) == 0);
        CHECK(t.size() > 16 && t[16] == "0"); // negative points are sent as 0
    }
    c.lines.clear();

    // 8. channel change (also how the client leaves a game room: the hosted game goes away)
    std::string j = "/join \"Polanie\" \"\"";
    j.push_back('\0');
    CHECK(s.OnData((const uint8_t*)j.data(), j.size()));
    c.TakeLines();
    // no $user for the player: the client lists itself when it enters the channel (0x823540)
    CHECK(c.lines.size() == 4);
    if (c.lines.size() == 4) {
        CHECK(GameTokens(c.lines[1])[0] == "/join" && GameTokens(c.lines[1])[1] == "Polanie");
        CHECK(c.lines[2] == "&play \"RTS : test\"");
        CHECK(c.lines[3] == "/syncstats 1 1 2 0 0 0 0");
    }
    CHECK(s.OwnGuid() == "00030201-0000-0000-0000-000000000000"); // the end of the login packet
    c.lines.clear();
    CHECK(s.OnData((const uint8_t*)j.data(), j.size())); // again: no duplicate $channel
    c.TakeLines();
    CHECK(c.lines.size() == 2 && GameTokens(c.lines[0])[0] == "/join");
    CHECK(s.Channel() == "Polanie");

    for (auto& l : log) printf("  log: %s\n", l.c_str());
}

static void TestBadInput() {
    en::SessionConfig cfg;
    en::LocalBackend backend;
    en::MemoryStore store;
    en::Session s(cfg, backend, store, [](const uint8_t*, size_t) {}, nullptr);
    uint8_t huge[4] = {0xff, 0xff, 0xff, 0x7f};
    CHECK(!s.OnData(huge, 4));
    CHECK(!s.OnData(huge, 4)); // stays closed
}

// Online ranking over a fake leaderboard service.
struct FakeRanking : en::RankingService {
    std::map<std::string, std::vector<en::BoardEntry>> boards;
    std::vector<std::string> calls;
    bool Top(const std::string& board, bool create, int count, std::vector<en::BoardEntry>& out) override {
        calls.push_back("top " + board + (create ? " create" : ""));
        auto it = boards.find(board);
        if (it == boards.end()) return !create ? true : (boards[board], true);
        out.assign(it->second.begin(), it->second.begin() + std::min((size_t)count, it->second.size()));
        return true;
    }
    void Join(const std::string& board, int score, const std::vector<int32_t>& details,
              std::function<bool(std::vector<int32_t>&)> refresh) override {
        calls.push_back("join " + board);
        for (auto& old : boards[board])
            if (old.steamName == "Wojtek") { // one entry per Steam account
                if (refresh(old.details)) calls.push_back("refresh");
                return;
            }
        en::BoardEntry e;
        e.steamName = "Wojtek";
        e.score = score;
        e.details = details;
        boards[board].push_back(e);
    }
    void Update(const std::string& board, std::function<void(int&, std::vector<int32_t>&)> apply) override {
        calls.push_back("update " + board);
        for (auto& old : boards[board])
            if (old.steamName == "Wojtek") {
                apply(old.score, old.details);
                return;
            }
        en::BoardEntry e;
        e.steamName = "Wojtek";
        apply(e.score, e.details);
        boards[board].push_back(e);
    }
};

static void TestSteamNames() {
    uint8_t g[16];
    for (int i = 0; i < 16; i++) g[i] = (uint8_t)(i + 1);
    CHECK(en::GuidString(g) == "04030201-0605-0807-090a-0b0c0d0e0f10"); // as the client writes it (/plays)
    CHECK(en::SanitizeNick("Wojtek") == "Wojtek");
    CHECK(en::SanitizeNick("  Jan \"Kowal\"  100% ") == "Jan 'Kowal' 100_");
    CHECK(en::SanitizeNick("a\tb\n\x01" "c") == "a b c");
    CHECK(en::SanitizeNick("Bardzo_dlugi_nick_gracza") == "Bardzo_dlugi_nic");
    CHECK(en::SanitizeNick("   ") == "");
    CHECK(en::SanitizeNick("\xa3\xf3" "d\xbf") == "\xa3\xf3" "d\xbf"); // Polish letters (cp1250) stay
}

static void TestRanking() {
    // board names
    CHECK(en::RankingBoard(en::LADDER_ALL, 1790000000) == "SteamNet");
    CHECK(en::RankingBoard(en::LADDER_MONTH, 1790812800) == "SteamNet 2026-10");  // 2026-10-01 00:00 UTC
    CHECK(en::RankingBoard(en::LADDER_MONTH, 1790812799) == "SteamNet 2026-09");
    CHECK(en::RankingBoard(en::LADDER_WEEK, 1790812800) == "SteamNet 2026-W40");  // Thursday 2026-10-01
    CHECK(en::RankingBoard(en::LADDER_WEEK, 1767225600) == "SteamNet 2026-W01");  // Thursday 2026-01-01
    CHECK(en::RankingBoard(en::LADDER_WEEK, 1767139200) == "SteamNet 2026-W01");  // Wednesday 2025-12-31
    CHECK(en::RankingBoard(en::LADDER_WEEK, 1798675200) == "SteamNet 2026-W53");  // Thursday 2026-12-31
    CHECK(en::RankingBoard(en::LADDER_WEEK, 1609459200) == "SteamNet 2020-W53");  // Friday 2021-01-01

    // details round trip; a 16+ character nick is cut to 16
    en::BoardEntry e;
    e.steamName = "Steam name";
    e.score = 150;
    e.details = en::RankingDetails("Test", 12, 9, 3, 1790812800);
    CHECK(e.details.size() == 9 && e.details[0] == 1);
    en::LadderRow r = en::RankingRow(e);
    CHECK(r.nick == "Test" && r.points == 150 && r.wins == 12 && r.losses == 9 && r.disconnects == 3);
    CHECK(r.lastPlayed > 46295.99 && r.lastPlayed < 46296.01); // 2026-10-01 as an OLE date
    e.details = en::RankingDetails("Bardzo_dlugi_nick_gracza", 0, 0, 0, 0);
    CHECK(en::RankingRow(e).nick == "Bardzo_dlugi_nic" && en::RankingRow(e).lastPlayed == 0);
    e.details = {7, 1, 2}; // unknown layout: Steam name and points only
    r = en::RankingRow(e);
    CHECK(r.nick == "Steam name" && r.points == 150 && r.wins == 0);

    // backend: login joins the all-time board, /ladder reads it, month/week boards are not created
    FakeClient c;
    en::SessionConfig cfg;
    cfg.accountNick = "Wojtek"; // Steam account: the ranking shows it whatever the game typed
    FakeRanking ranking;
    en::RankedBackend backend(ranking, [] { return (int64_t)1790812800; });
    en::MemoryStore store;
    en::BoardEntry old; // this account played before under the name "Test"
    old.steamName = "Wojtek";
    old.score = 150;
    old.details = en::RankingDetails("Test", 12, 9, 3, 1790812800);
    ranking.boards["SteamNet"].push_back(old);
    en::Session s(cfg, backend, store, [&](const uint8_t* p, size_t n) { c.in.insert(c.in.end(), p, p + n); }, nullptr);
    en::Writer info;
    info.raw("x", 1);
    auto pkt = GamePacket(info);
    s.OnData(pkt.data(), pkt.size());
    en::Writer login;
    login.str("Test");
    login.str("");
    login.u32(0);
    login.u32(0);
    login.u32(0);
    uint8_t zero[16] = {0};
    login.raw(zero, 16);
    pkt = GamePacket(login);
    CHECK(s.OnData(pkt.data(), pkt.size()) && s.LoggedIn());
    CHECK(s.Nick() == "Test" && s.PublicName() == "Wojtek");
    CHECK(ranking.calls.size() == 2 && ranking.calls[0] == "join SteamNet" && ranking.calls[1] == "refresh");
    std::string l = "/ladder";
    l.push_back('\0');
    l += "/ladderw";
    l.push_back('\0');
    CHECK(s.OnData((const uint8_t*)l.data(), l.size()));
    std::vector<uint8_t> body;
    c.TakePacket(body);
    c.TakeLines();
    CHECK(ranking.calls.size() == 4 && ranking.calls[2] == "top SteamNet create" && ranking.calls[3] == "top SteamNet 2026-W40");
    auto ladder = std::find_if(c.lines.begin(), c.lines.end(), [](const std::string& x) { return x.rfind("/ladder ", 0) == 0; });
    // the name is updated, points and results stay
    CHECK(ladder != c.lines.end() && *ladder == "/ladder 0 \"Wojtek\" \"46296.000000\" 12 9 3 150 0 0 0");
    std::vector<int32_t> d = old.details;
    CHECK(!en::RankingSetNick(d, "Test") && en::RankingSetNick(d, "Wojtek") && !en::RankingSetNick(d, "Wojtek"));
    std::vector<int32_t> other = {2, 1, 1, 1, 1, 1, 1, 1, 1};
    CHECK(!en::RankingSetNick(other, "Wojtek") && other[5] == 1); // unknown layout: left alone
    CHECK(!c.lines.empty() && c.lines.back() == "/ladderw 0");
}

// Steam account login: saved heroes follow the account, older ones move over from a nick.
static void TestSteamAccount() {
    FakeClient c;
    en::SessionConfig cfg;
    cfg.accountNick = "Wojtek";
    cfg.accountKey = "steam_76561198000000001";
    cfg.previousLogin = "Test";
    int hellos = 0;
    cfg.beforeHello = [&] { hellos++; CHECK(c.in.empty()); }; // before the hello goes out
    en::LocalBackend backend;
    en::MemoryStore store;
    std::vector<uint8_t> hero = {1, 2, 3, 4};
    store.Save("Test", "KS_RPG_ChData.1.0", hero); // saved by an older version under the login "Test"
    en::Session s(cfg, backend, store, [&](const uint8_t* p, size_t n) { c.in.insert(c.in.end(), p, p + n); }, nullptr);
    en::Writer info;
    info.raw("x", 1);
    auto pkt = GamePacket(info);
    CHECK(s.OnData(pkt.data(), pkt.size()) && hellos == 1);
    std::vector<uint8_t> body;
    CHECK(c.TakePacket(body));
    en::Writer login;
    login.str("Wojtek");
    login.str("");
    login.u32(0);
    login.u32(0);
    login.u32(0);
    uint8_t zero[16] = {0};
    login.raw(zero, 16);
    pkt = GamePacket(login);
    CHECK(s.OnData(pkt.data(), pkt.size()) && s.LoggedIn());
    c.TakePacket(body);
    c.TakeLines();
    c.lines.clear();

    std::string l = "/getplayerdata \"Wojtek\" \"KS_RPG_ChData.1.0\"";
    l.push_back('\0');
    CHECK(s.OnData((const uint8_t*)l.data(), l.size()));
    CHECK(c.in.size() > 4 && std::equal(hero.begin(), hero.end(), c.in.end() - 4)); // the old hero
    std::vector<uint8_t> moved;
    CHECK(store.Load("steam_76561198000000001", "KS_RPG_ChData.1.0", moved) && moved == hero);
    c.in.clear();

    std::string save = "/setplayerdata \"Wojtek\" \"KS_RPG_ChData.1.0\" \"2\"";
    save.push_back('\0');
    save += "\x09\x08";
    CHECK(s.OnData((const uint8_t*)save.data(), save.size()));
    CHECK(store.Load("steam_76561198000000001", "KS_RPG_ChData.1.0", moved) && moved == std::vector<uint8_t>({9, 8}));
    CHECK(!store.Load("Wojtek", "KS_RPG_ChData.1.0", moved)); // not under the Steam name

    // chat shows the account; the player list entry is the client's own (no $user)
    std::string j = "/join \"KnightShift\" \"\"";
    j.push_back('\0');
    j += "/msg \"#KnightShift\" \"hej\"";
    j.push_back('\0');
    CHECK(s.OnData((const uint8_t*)j.data(), j.size()));
    c.TakeLines();
    CHECK(std::none_of(c.lines.begin(), c.lines.end(), [](const std::string& x) { return x.rfind("$user ", 0) == 0; }));
    CHECK(std::find(c.lines.begin(), c.lines.end(), "/send \"Wojtek\" \"hej\"") != c.lines.end());
}

// Online channels over a fake lobby service (Steam lobbies in the game).
struct FakeLobbies : en::LobbyService {
    std::shared_ptr<en::LobbyEvents> events;
    std::vector<std::string> calls;
    void Start(std::shared_ptr<en::LobbyEvents> e) override { events = e; calls.push_back("start"); }
    void Enter(const std::string& channel) override { calls.push_back("enter " + channel); }
    void Say(const std::string& text) override { calls.push_back("say " + text); }
    void Whisper(uint64_t to, const std::string& text) override { calls.push_back("whisper " + std::to_string(to) + " " + text); }
    void RefreshChannels() override { calls.push_back("channels"); }
    void PublishGame(const std::string& name, const std::string& guid) override { calls.push_back("publish " + name + " " + guid); }
    void UnpublishGame() override { calls.push_back("unpublish"); }
    void Stop() override { calls.push_back("stop"); }
    void Push(en::LobbyEvent::Kind k, uint64_t id = 0, const std::string& name = "", const std::string& text = "") {
        en::LobbyEvent e;
        e.kind = k;
        e.member.id = id;
        e.member.name = name;
        e.text = text;
        events->Push(e);
    }
};

static void TestOnline() {
    FakeClient c;
    en::SessionConfig cfg;
    cfg.accountNick = "Wojtek";
    FakeLobbies lobbies;
    en::NoRanking ranking;
    int64_t now = 1000;
    auto backend = std::make_unique<en::OnlineBackend>(lobbies, ranking, [&] { return now; });
    en::MemoryStore store;
    en::Session s(cfg, *backend, store, [&](const uint8_t* p, size_t n) { c.in.insert(c.in.end(), p, p + n); }, nullptr);
    en::Writer info;
    info.raw("x", 1);
    auto pkt = GamePacket(info);
    s.OnData(pkt.data(), pkt.size());
    en::Writer login;
    login.str("Wojtek");
    login.str("");
    login.u32(0);
    login.u32(0);
    login.u32(0);
    uint8_t zero[16] = {0};
    login.raw(zero, 16);
    pkt = GamePacket(login);
    CHECK(s.OnData(pkt.data(), pkt.size()) && s.LoggedIn());
    std::vector<uint8_t> body;
    c.TakePacket(body);
    c.TakePacket(body);
    c.TakeLines();
    CHECK(lobbies.calls.size() == 3 && lobbies.calls[0] == "start" && lobbies.calls[1] == "enter KnightShift" &&
          lobbies.calls[2] == "channels");
    c.lines.clear();
    auto has = [&](const std::string& l) { return std::find(c.lines.begin(), c.lines.end(), l) != c.lines.end(); };

    // typed before the lobby answered: sent when it does
    std::string early = "/msg \"#KnightShift\" \"pierwsza\"";
    early.push_back('\0');
    CHECK(s.OnData((const uint8_t*)early.data(), early.size()));
    CHECK(std::find(lobbies.calls.begin(), lobbies.calls.end(), "say pierwsza") == lobbies.calls.end());
    c.TakeLines();
    c.lines.clear();

    // the lobby answers: two others in the channel, one with the same Steam name as ours... not a
    // problem for the game; two others with the same name get told apart
    en::LobbyEvent entered;
    entered.kind = en::LobbyEvent::ENTERED;
    entered.channel = "KnightShift";
    entered.members = {{11, "Ania"}, {12, "Bolek"}, {13, "Bolek"}};
    lobbies.events->Push(entered);
    s.Poll();
    c.TakeLines();
    CHECK(has("$user \"Ania\" 0 \"\" \"00000000-0000-0000-0000-000000000000\""));
    CHECK(has("$user \"Bolek\" 0 \"\" \"00000000-0000-0000-0000-000000000000\""));
    CHECK(has("$user \"Bolek#2\" 0 \"\" \"00000000-0000-0000-0000-000000000000\""));
    CHECK(has("/syncstats 4 4 1 0 0 0 0"));
    CHECK(std::find(lobbies.calls.begin(), lobbies.calls.end(), "say pierwsza") != lobbies.calls.end());
    c.lines.clear();

    // chat both ways, whispers by name
    lobbies.Push(en::LobbyEvent::SAY, 11, "Ania", "czesc");
    lobbies.Push(en::LobbyEvent::WHISPER, 13, "Bolek", "psst");
    s.Poll();
    c.TakeLines();
    CHECK(has("/send \"Ania\" \"czesc\""));
    CHECK(has("/msg 0 \"Bolek#2\" 0 \"psst\""));
    c.lines.clear();
    std::string l = "/msg \"#KnightShift\" \"hej\"";
    l.push_back('\0');
    l += "/msg \"Bolek#2\" \"tajne\"";
    l.push_back('\0');
    l += "/msg \"Nikt\" \"halo\"";
    l.push_back('\0');
    CHECK(s.OnData((const uint8_t*)l.data(), l.size()));
    c.TakeLines();
    CHECK(has("/send \"Wojtek\" \"hej\"") && has("/msgc \"Bolek#2\" \"tajne\""));
    CHECK(std::find(lobbies.calls.begin(), lobbies.calls.end(), "say hej") != lobbies.calls.end());
    CHECK(std::find(lobbies.calls.begin(), lobbies.calls.end(), "whisper 13 tajne") != lobbies.calls.end());
    CHECK(c.lines.size() == 3 && c.lines[2].find("Nikt") != std::string::npos); // not in the channel
    c.lines.clear();

    // someone leaves, someone comes
    lobbies.Push(en::LobbyEvent::LEFT, 12);
    lobbies.Push(en::LobbyEvent::JOINED, 14, "Cezary");
    s.Poll();
    c.TakeLines();
    CHECK(has("&user \"Bolek\" \"\"") && has("$user \"Cezary\" 0 \"\" \"00000000-0000-0000-0000-000000000000\""));
    c.lines.clear();

    // the channel list; back from a game room lists the others again without a new lobby
    en::LobbyEvent chans;
    chans.kind = en::LobbyEvent::CHANNELS;
    chans.channels = {"KnightShift", "Polanie"};
    lobbies.events->Push(chans);
    s.Poll();
    c.TakeLines();
    CHECK(has("$channel \"Polanie\" 0 0 \"\"") && has("/syncstats 4 4 2 0 0 0 0"));
    c.lines.clear();
    size_t before = lobbies.calls.size();
    l = "/join \"KnightShift\" \"\"";
    l.push_back('\0');
    CHECK(s.OnData((const uint8_t*)l.data(), l.size()));
    c.TakeLines();
    CHECK(lobbies.calls.size() == before && has("$user \"Cezary\" 0 \"\" \"00000000-0000-0000-0000-000000000000\""));
    c.lines.clear();

    // games: another player's game is listed with its address, joining answers with it
    en::LobbyEvent g;
    g.kind = en::LobbyEvent::GAME_ADDED;
    g.member.id = 14;
    g.text = "RTS : mapa";
    g.guid = "04030201-0605-0807-090a-0b0c0d0e0f10";
    g.ipv4 = 0x0101530a; // 10.83.1.1
    lobbies.events->Push(g);
    g.member.id = 11; // same name from someone else: told apart
    g.guid = "00000000-0000-0000-0000-000000000011";
    g.ipv4 = 0x0201530a;
    lobbies.events->Push(g);
    s.Poll();
    c.TakeLines();
    CHECK(has("$play \"RTS : mapa\" 0 0 16864010 \"04030201-0605-0807-090a-0b0c0d0e0f10\""));
    CHECK(has("$play \"RTS : mapa #2\" 0 0 33641226 \"00000000-0000-0000-0000-000000000011\""));
    CHECK(has("/syncstats 4 4 2 0 2 0 0"));
    c.lines.clear();
    l = "/playc \"04030201-0605-0807-090a-0b0c0d0e0f10\" \"RTS : mapa\" \"\"";
    l.push_back('\0');
    l += "/playc \"00000000-0000-0000-0000-000000000099\" \"Nie ma\" \"\"";
    l.push_back('\0');
    CHECK(s.OnData((const uint8_t*)l.data(), l.size()));
    c.TakeLines();
    CHECK(has("/playc \"04030201-0605-0807-090a-0b0c0d0e0f10\" \"RTS : mapa\" 1 16864010"));
    CHECK(has("/playc \"00000000-0000-0000-0000-000000000099\" \"Nie ma\" 0") && has("&play \"Nie ma\""));
    c.lines.clear();
    lobbies.Push(en::LobbyEvent::GAME_REMOVED, 14);
    lobbies.Push(en::LobbyEvent::LEFT, 11); // Ania leaves: her game goes too
    s.Poll();
    c.TakeLines();
    CHECK(has("&play \"RTS : mapa\"") && has("&play \"RTS : mapa #2\"") && has("&user \"Ania\" \"\""));
    c.lines.clear();

    // hosting: the channel sees the game until the player is back in the channel
    l = "/plays \"RPG : moja\" \"\" \"0a0b0c0d-0000-0000-0000-000000000001\"";
    l.push_back('\0');
    CHECK(s.OnData((const uint8_t*)l.data(), l.size()));
    CHECK(lobbies.calls.back() == "publish RPG : moja 0a0b0c0d-0000-0000-0000-000000000001");
    l = "/join \"KnightShift\" \"\"";
    l.push_back('\0');
    CHECK(s.OnData((const uint8_t*)l.data(), l.size()));
    CHECK(std::find(lobbies.calls.begin(), lobbies.calls.end(), "unpublish") != lobbies.calls.end());
    c.TakeLines();
    c.lines.clear();
    lobbies.Push(en::LobbyEvent::JOINED, 11, "Ania");
    s.Poll();
    c.TakeLines();
    c.lines.clear();

    // another channel: a new lobby, the old players go
    l = "/join \"Polanie\" \"\"";
    l.push_back('\0');
    CHECK(s.OnData((const uint8_t*)l.data(), l.size()));
    CHECK(lobbies.calls.back() == "enter Polanie");
    lobbies.Push(en::LobbyEvent::SAY, 11, "Ania", "stary kanal"); // Ania is not in Polanie
    entered.channel = "KnightShift";                              // a late answer for the old channel
    lobbies.events->Push(entered);
    s.Poll();
    c.TakeLines();
    CHECK(!has("$user \"Ania\" 0 \"\" \"00000000-0000-0000-0000-000000000000\""));
    c.lines.clear();

    // a quiet minute refreshes the channel list; logging out leaves the lobby
    now += 61;
    s.Poll();
    CHECK(lobbies.calls.back() == "channels");
    s.OnClosed();
    CHECK(lobbies.calls.back() == "stop");
    backend.reset();
    CHECK(std::count(lobbies.calls.begin(), lobbies.calls.end(), "stop") == 1);
}

// Match results: a started match counts as a disconnect until its result comes.
static void TestResults() {
    std::vector<int32_t> d;
    int score = 7;
    en::ResultChange win;
    win.wins = 1;
    win.points = 3;
    en::ApplyResult(score, d, win, "Wojtek", 1790812800); // no entry yet: starts from nothing
    CHECK(score == 3 && d.size() == 9 && d[1] == 1 && d[2] == 0 && d[4] == 1790812800);
    CHECK(en::RankingRow(en::BoardEntry{"x", score, d}).nick == "Wojtek");

    FakeClient c;
    en::SessionConfig cfg;
    cfg.accountNick = "Wojtek";
    bool match = false;
    cfg.matchRunning = [&] { return match; };
    FakeRanking ranking;
    int64_t now = 1790812800; // Thursday 2026-10-01
    en::RankedBackend backend(ranking, [&] { return now; });
    en::MemoryStore store;
    en::Session s(cfg, backend, store, [&](const uint8_t* p, size_t n) { c.in.insert(c.in.end(), p, p + n); }, nullptr);
    en::Writer info;
    info.raw("x", 1);
    auto pkt = GamePacket(info);
    s.OnData(pkt.data(), pkt.size());
    en::Writer login;
    login.str("Wojtek");
    login.str("");
    login.u32(0);
    login.u32(0);
    login.u32(0);
    uint8_t zero[16] = {0};
    login.raw(zero, 16);
    pkt = GamePacket(login);
    CHECK(s.OnData(pkt.data(), pkt.size()) && s.LoggedIn());
    auto entry = [&](const std::string& board) {
        for (auto& e : ranking.boards[board])
            if (e.steamName == "Wojtek") return en::RankingRow(e);
        return en::LadderRow();
    };
    auto send = [&](const std::string& line) {
        std::string l = line;
        l.push_back('\0');
        CHECK(s.OnData((const uint8_t*)l.data(), l.size()));
    };

    // match 1: starts (a disconnect for now), then won
    s.Poll();
    CHECK(entry("SteamNet").disconnects == 0);
    match = true;
    s.Poll();
    s.Poll(); // still the same match: counted once
    en::LadderRow r = entry("SteamNet");
    CHECK(r.disconnects == 1 && r.points == -1);
    CHECK(entry("SteamNet 2026-10").disconnects == 1 && entry("SteamNet 2026-W40").disconnects == 1);
    send("/playv \"g\" \"RTS : mapa\" \"\" \"g\"");
    match = false;
    s.Poll();
    r = entry("SteamNet");
    CHECK(r.wins == 1 && r.losses == 0 && r.disconnects == 0 && r.points == 3);
    CHECK(entry("SteamNet 2026-W40").points == 3);

    // match 2: lost
    match = true;
    s.Poll();
    send("/playd \"g\" \"RTS : mapa\" \"\" \"g\"");
    match = false;
    s.Poll();
    r = entry("SteamNet");
    CHECK(r.wins == 1 && r.losses == 1 && r.disconnects == 0 && r.points == 3);

    // match 3: not rated
    match = true;
    s.Poll();
    send("/play0 \"g\" \"RTS : mapa\" \"\" \"g\"");
    match = false;
    s.Poll();
    r = entry("SteamNet");
    CHECK(r.wins == 1 && r.losses == 1 && r.disconnects == 0 && r.points == 3);

    // match 4: the game is closed mid-match - the disconnect stays
    match = true;
    s.Poll();
    s.OnClosed();
    r = entry("SteamNet");
    CHECK(r.disconnects == 1 && r.points == 2);
    // two uploads for a match with a result, one for an unfinished one
    CHECK(std::count(ranking.calls.begin(), ranking.calls.end(), "update SteamNet") == 7);
}

int main() {
    TestZlib();
    TestSignature();
    TestSession();
    TestBadInput();
    TestSteamNames();
    TestRanking();
    TestSteamAccount();
    TestOnline();
    TestResults();
    printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
