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
    CHECK(c.lines.size() == 2);
    if (c.lines.size() == 2) {
        auto t = GameTokens(c.lines[0]);
        CHECK(t.size() == 5 && t[0] == "$channel" && t[1] == "KnightShift");
        t = GameTokens(c.lines[1]);
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

    // 8. channel change
    std::string j = "/join \"Polanie\" \"\"";
    j.push_back('\0');
    CHECK(s.OnData((const uint8_t*)j.data(), j.size()));
    c.TakeLines();
    CHECK(c.lines.size() == 3);
    if (c.lines.size() == 3) {
        CHECK(GameTokens(c.lines[1])[0] == "/join" && GameTokens(c.lines[1])[1] == "Polanie");
        CHECK(GameTokens(c.lines[2])[0] == "$user" && GameTokens(c.lines[2])[1] == "Joe");
    }
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

int main() {
    TestZlib();
    TestSignature();
    TestSession();
    TestBadInput();
    printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
