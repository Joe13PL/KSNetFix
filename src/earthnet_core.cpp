// SteamNet - EarthNet server emulator, protocol core. See earthnet_core.h and
// docs/STEAMNET_PROTOCOL.md (client addresses are for KnightShift.ex1).
#define _CRT_SECURE_NO_WARNINGS
#include "earthnet_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

namespace en {

// ---------------------------------------------------------------------------
// Login signature (0x8069E0): table lookup keyed by the identity, then the MSVC rand()
// stream seeded from it. The client compares all 64 bytes.
// ---------------------------------------------------------------------------
void Sign(uint64_t id, uint8_t out[64]) {
    static const uint8_t T[64] = {
        0xd2, 0x12, 0x13, 0xd3, 0x11, 0xd1, 0xd0, 0x10, 0xf0, 0x30, 0x31, 0xf1, 0x33, 0xf3, 0xf2, 0x32,
        0x36, 0xf6, 0xf7, 0x37, 0xf5, 0x35, 0x34, 0xf4, 0x3c, 0xfc, 0xfd, 0x3d, 0xff, 0x3f, 0x3e, 0xfe,
        0xfa, 0x3a, 0x3b, 0xfb, 0x39, 0xf9, 0xf8, 0x38, 0x28, 0xe8, 0xe9, 0x29, 0xeb, 0x2b, 0x2a, 0xea,
        0xee, 0x2e, 0x2f, 0xef, 0x2d, 0xed, 0xec, 0x2c, 0xe4, 0x24, 0x25, 0xe5, 0x27, 0xe7, 0xe6, 0x26};
    auto byteAt = [&](int i) { return (uint8_t)(id >> (8 * i)); };
    int32_t s = 0, a = 0;
    uint8_t x = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t b = byteAt(i);
        s += x + b;
        x ^= b;
        a += x;
    }
    for (int i = 0; i < 64; i++) out[i] = T[(i + s) % 64] ^ byteAt((i + a) % 8);
    uint32_t seed = (uint32_t)(s + a);
    auto rnd = [&]() {
        seed = seed * 214013u + 2531011u;
        return (int)((seed >> 16) & 0x7fff);
    };
    for (int i = 0; i < 64; i++) out[i] ^= (uint8_t)rnd();
    for (int i = 0; i < 32; i++) {
        int p = rnd() % 64;
        int q = rnd() % 64;
        uint8_t t = out[q];
        out[q] = out[p];
        out[p] = t;
    }
}

std::string Quote(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '%' || c < 0x20) {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            r += hex;
        } else {
            r += (char)c;
        }
    }
    return r + "\"";
}

std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> w;
    size_t i = 0, n = line.size();
    while (i < n) {
        while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\n' || line[i] == '\r')) i++;
        if (i >= n) break;
        if (line[i] == '"') {
            size_t e = line.find('"', i + 1);
            if (e == std::string::npos) e = n;
            w.push_back(line.substr(i + 1, e - i - 1));
            i = e + 1;
        } else {
            size_t e = i;
            while (e < n && line[e] != ' ' && line[e] != '\t' && line[e] != '\n' && line[e] != '\r') e++;
            w.push_back(line.substr(i, e - i));
            i = e;
        }
    }
    return w;
}

// ---------------------------------------------------------------------------
// zlib (RFC 1950/1951). The game links zlib 1.1.3; its packets are zlib streams.
// ---------------------------------------------------------------------------
static uint32_t Adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return b << 16 | a;
}

std::vector<uint8_t> ZlibStore(const uint8_t* p, size_t n) {
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t off = 0;
    do {
        size_t k = n - off > 65535 ? 65535 : n - off;
        z.push_back(off + k == n ? 1 : 0); // BFINAL, BTYPE 00 (stored)
        z.push_back((uint8_t)k);
        z.push_back((uint8_t)(k >> 8));
        z.push_back((uint8_t)~k);
        z.push_back((uint8_t)(~k >> 8));
        z.insert(z.end(), p + off, p + off + k);
        off += k;
    } while (off < n);
    uint32_t a = Adler32(p, n);
    for (int i = 3; i >= 0; i--) z.push_back((uint8_t)(a >> (8 * i)));
    return z;
}

namespace {
struct Bits {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool err = false;
    int Get(int k) {
        while (cnt < k) {
            if (pos >= n) {
                err = true;
                return 0;
            }
            buf |= (uint32_t)p[pos++] << cnt;
            cnt += 8;
        }
        int v = (int)(buf & ((1u << k) - 1));
        buf >>= k;
        cnt -= k;
        return v;
    }
};

// Canonical Huffman code: count of codes per length, symbols ordered by code.
struct Huff {
    uint16_t count[16];
    uint16_t symbol[320];
    bool Build(const uint8_t* len, int n) {
        uint16_t offs[16];
        memset(count, 0, sizeof(count));
        for (int i = 0; i < n; i++) count[len[i]]++;
        count[0] = 0;
        int left = 1;
        for (int l = 1; l < 16; l++) {
            left <<= 1;
            left -= count[l];
            if (left < 0) return false; // over-subscribed
        }
        offs[1] = 0;
        for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + count[l];
        for (int i = 0; i < n; i++)
            if (len[i]) symbol[offs[len[i]]++] = (uint16_t)i;
        return true;
    }
    int Decode(Bits& b) const {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16; l++) {
            code |= b.Get(1);
            if (b.err) return -1;
            int c = count[l];
            if (code - first < c) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                               67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(Bits& b, const Huff& lit, const Huff& dist, std::vector<uint8_t>& out, size_t maxOut) {
    for (;;) {
        int sym = lit.Decode(b);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= maxOut) return false;
            out.push_back((uint8_t)sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = kLenBase[sym] + b.Get(kLenExtra[sym]);
            int d = dist.Decode(b);
            if (d < 0 || d >= 30) return false;
            size_t back = kDistBase[d] + b.Get(kDistExtra[d]);
            if (b.err || back > out.size() || out.size() + len > maxOut) return false;
            for (size_t i = 0; i < len; i++) out.push_back(out[out.size() - back]);
        }
    }
}
} // namespace

bool Inflate(const uint8_t* p, size_t n, std::vector<uint8_t>& out, size_t maxOut) {
    out.clear();
    if (n < 6 || (p[0] & 0x0F) != 8 || ((p[0] << 8) | p[1]) % 31 != 0 || (p[1] & 0x20)) return false;
    Bits b;
    b.p = p + 2;
    b.n = n - 2;
    int last;
    do {
        last = b.Get(1);
        int type = b.Get(2);
        if (b.err) return false;
        if (type == 0) {
            b.buf = 0;
            b.cnt = 0;
            if (b.pos + 4 > b.n) return false;
            size_t len = b.p[b.pos] | b.p[b.pos + 1] << 8;
            size_t nlen = b.p[b.pos + 2] | b.p[b.pos + 3] << 8;
            b.pos += 4;
            if (len != (~nlen & 0xFFFF) || b.pos + len > b.n || out.size() + len > maxOut) return false;
            out.insert(out.end(), b.p + b.pos, b.p + b.pos + len);
            b.pos += len;
        } else if (type == 1) {
            static Huff lit, dist;
            static bool built = false;
            if (!built) {
                uint8_t l[288];
                for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                lit.Build(l, 288);
                for (int i = 0; i < 30; i++) l[i] = 5;
                dist.Build(l, 30);
                built = true;
            }
            if (!Codes(b, lit, dist, out, maxOut)) return false;
        } else if (type == 2) {
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            int nlit = b.Get(5) + 257, ndist = b.Get(5) + 1, ncode = b.Get(4) + 4;
            if (b.err || nlit > 286 || ndist > 30) return false;
            uint8_t len[320] = {0};
            for (int i = 0; i < ncode; i++) len[order[i]] = (uint8_t)b.Get(3);
            Huff lencode, lit, dist;
            if (!lencode.Build(len, 19)) return false;
            memset(len, 0, sizeof(len));
            int i = 0;
            while (i < nlit + ndist) {
                int sym = lencode.Decode(b);
                if (sym < 0) return false;
                if (sym < 16) {
                    len[i++] = (uint8_t)sym;
                    continue;
                }
                int rep, val = 0;
                if (sym == 16) {
                    if (i == 0) return false;
                    val = len[i - 1];
                    rep = 3 + b.Get(2);
                } else if (sym == 17) {
                    rep = 3 + b.Get(3);
                } else {
                    rep = 11 + b.Get(7);
                }
                if (b.err || i + rep > nlit + ndist) return false;
                while (rep--) len[i++] = (uint8_t)val;
            }
            if (len[256] == 0) return false;
            if (!lit.Build(len, nlit) || !dist.Build(len + nlit, ndist)) return false;
            if (!Codes(b, lit, dist, out, maxOut)) return false;
        } else {
            return false;
        }
    } while (!last);
    // Adler-32 of the data follows the last block (byte aligned).
    size_t at = b.pos - (size_t)(b.cnt / 8);
    if (at + 4 > b.n) return false;
    uint32_t want = (uint32_t)b.p[at] << 24 | b.p[at + 1] << 16 | b.p[at + 2] << 8 | b.p[at + 3];
    return want == Adler32(out.data(), out.size());
}

std::vector<uint8_t> Writer::Packet() const {
    auto z = ZlibStore(b.data(), b.size());
    Writer p;
    p.u32((uint32_t)z.size() + 4);
    p.raw(z.data(), z.size());
    return p.b;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
static const char kNullGuid[] = "00000000-0000-0000-0000-000000000000";
static const size_t kMaxPacket = 64 * 1024;
static const size_t kMaxLine = 16 * 1024;
static const size_t kMaxBinary = 1024 * 1024;

Session::Session(const SessionConfig& cfg, Backend& backend, PlayerStore& store, SendFn send, LogFn log)
    : cfg_(cfg), backend_(backend), store_(store), send_(send), log_(log) {}

void Session::Log(const char* fmt, ...) {
    if (!log_) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    log_(buf);
}

void Session::Packet(const Writer& w) {
    auto p = w.Packet();
    send_(p.data(), p.size());
}

void Session::Line(const std::string& line) {
    std::string l = line;
    l.push_back('\0');
    send_((const uint8_t*)l.data(), l.size());
}

bool Session::OnData(const uint8_t* p, size_t n) {
    if (state_ == CLOSED) return false;
    in_.insert(in_.end(), p, p + n);
    for (;;) {
        if (state_ == CLIENT_INFO || state_ == LOGIN) {
            if (in_.size() < 4) return true;
            uint32_t len = in_[0] | in_[1] << 8 | in_[2] << 16 | (uint32_t)in_[3] << 24;
            if (len < 4 || len > kMaxPacket) {
                Log("bad packet length %u - closing", len);
                state_ = CLOSED;
                return false;
            }
            if (in_.size() < len) return true;
            std::vector<uint8_t> body;
            bool ok = Inflate(in_.data() + 4, len - 4, body, kMaxPacket);
            in_.erase(in_.begin(), in_.begin() + len);
            if (!ok) {
                Log("packet is not a valid zlib stream (%u bytes) - closing", len);
                state_ = CLOSED;
                return false;
            }
            if (!OnPacket(body.data(), body.size())) {
                state_ = CLOSED;
                return false;
            }
        } else if (state_ == BINARY) {
            if (in_.size() < binNeed_) return true;
            std::vector<uint8_t> data(in_.begin(), in_.begin() + binNeed_);
            in_.erase(in_.begin(), in_.begin() + binNeed_);
            binNeed_ = 0;
            state_ = LINES;
            OnBinary(data);
        } else if (state_ == LINES) {
            size_t z = 0;
            while (z < in_.size() && in_[z] != 0) z++;
            if (z == in_.size()) {
                if (in_.size() > kMaxLine) {
                    Log("line too long - closing");
                    state_ = CLOSED;
                    return false;
                }
                return true;
            }
            std::string line(in_.begin(), in_.begin() + z);
            in_.erase(in_.begin(), in_.begin() + z + 1);
            OnClientLine(line);
        } else {
            return false;
        }
    }
}

void Session::OnClosed() {
    if (LoggedIn()) backend_.OnLogout(*this);
    state_ = CLOSED;
}

bool Session::OnPacket(const uint8_t* p, size_t n) {
    if (state_ == CLIENT_INFO) {
        Log("client information: %u bytes", (unsigned)n);
        Writer w;
        w.u32(0); // accepted
        w.str(cfg_.welcome);
        Packet(w);
        state_ = LOGIN;
        return true;
    }
    // Login packet (0x803110): login, password, u32 0, GUID list, reconnect flag, ...
    Reader r(p, n);
    LoginInfo li;
    li.login = r.str(256);
    li.password = r.str(256);
    r.u32();
    uint32_t guids = r.u32();
    if (guids <= 64) r.skip(16 * guids);
    li.reconnect = r.u32() != 0;
    if (!r.ok) {
        Log("malformed login packet (%u bytes)", (unsigned)n);
        Writer w;
        w.u32(1);
        w.str("\"SteamNet: invalid login\"");
        Packet(w);
        return false;
    }
    if (li.login.empty()) {
        // First connect from the server list: no name yet. An error makes the client open its
        // login window (0x81F9C0); it then sends another login packet on this same connection
        // (0x819570) and waits for the reply, so the connection must stay open. The text is
        // "message" ["name for the name field"]; the message is used as a format string.
        Log("login without a name - asking for one");
        Writer w;
        w.u32(1);
        w.str("\"" + cfg_.askName + "\"");
        Packet(w);
        return true;
    }
    nick_ = li.login;
    channel_ = cfg_.channel;
    Log("login \"%s\"%s", nick_.c_str(), li.reconnect ? " (reconnect)" : "");

    // Login reply (0x803820), field order as the client reads it.
    uint8_t zero16[16] = {0};
    uint8_t sig[64];
    Sign(cfg_.identity, sig);
    Writer w;
    w.u32(0);              // result: ok
    w.u32(0);              // +0x4EC8
    w.str(cfg_.welcome);   // +0x4EB4
    w.str(cfg_.welcome);   // +0x4EB4 (message of the day, replaces the first)
    w.raw(zero16, 8);      // +0x4EF0 (double)
    for (int i = 0; i < 9; i++) w.u32(0);
    w.u8(0xFF);            // {index, string} lists 1 and 2: empty
    w.u8(0xFF);
    w.u8(0);               // list 3 (+0x4F24): ranking categories; /ladder rows need a valid one
    w.str("KnightShift");
    w.u8(0xFF);
    w.u8(0);               // +0x4EEC
    w.str(channel_);       // +0x4EB8 first channel
    for (int i = 0; i < 2; i++) { // two server images: none
        w.u32(0);
        w.raw(zero16, 16);
    }
    w.raw(sig, 64);
    Packet(w);
    state_ = LINES;
    backend_.OnLogin(*this);
    return true;
}

void Session::OnClientLine(const std::string& line) {
    auto w = Tokenize(line);
    if (w.empty()) return;
    const std::string& cmd = w[0];
    auto arg = [&](size_t i) { return i < w.size() ? w[i] : std::string(); };
    if (cmd == "/send") {
        // The client does not escape quotes inside the text: take everything between the
        // first and the last quote.
        size_t q0 = line.find('"'), q1 = line.rfind('"');
        std::string text = q0 != std::string::npos && q1 > q0 ? line.substr(q0 + 1, q1 - q0 - 1) : arg(1);
        // Chat commands the client passes through: /msg, /w, /whisper <nick> <text>
        if (!text.empty() && text[0] == '/') {
            auto t = Tokenize(text);
            if (t.size() >= 3 && (t[0] == "/msg" || t[0] == "/w" || t[0] == "/whisper")) {
                size_t at = text.find(t[1]) + t[1].size();
                while (at < text.size() && text[at] == ' ') at++;
                backend_.OnWhisper(*this, t[1], text.substr(at));
                return;
            }
        }
        backend_.OnSay(*this, text);
    } else if (cmd == "/msg") {
        // Chat input (0x8268C0): /msg "#channel" "text" for the channel, /msg "nick" "text" to whisper.
        const std::string to = arg(1);
        size_t q = line.find('"', line.find('"', line.find('"') + 1) + 1); // opening quote of the text
        size_t e = line.rfind('"');
        std::string text = q != std::string::npos && e > q ? line.substr(q + 1, e - q - 1) : arg(2);
        if (!to.empty() && to[0] == '#')
            backend_.OnSay(*this, text);
        else
            backend_.OnWhisper(*this, to, text);
    } else if (cmd == "/join") {
        backend_.OnJoin(*this, arg(1), arg(2));
    } else if (cmd == "/plays") {
        // /plays "<zero guid>" "RTS : name" "password" asks to host (0x80B580); after our
        // /plays "name" "password" the client hosts (+0xE4) and answers /plays "name" "password" "guid".
        bool request = arg(1).size() == 36 && arg(1)[8] == '-' && arg(1)[13] == '-';
        if (request) {
            Log("host request \"%s\"", arg(2).c_str());
            backend_.OnHostRequest(*this, arg(2), arg(3));
        } else {
            Log("game hosted \"%s\" %s", arg(1).c_str(), arg(3).c_str());
            backend_.OnGameHosted(*this, arg(1), arg(3));
        }
    } else if (cmd == "/ladder" || cmd == "/ladderm" || cmd == "/ladderw") {
        LadderPeriod p = cmd == "/ladder" ? LADDER_ALL : cmd == "/ladderm" ? LADDER_MONTH : LADDER_WEEK;
        Ladder(p, backend_.OnLadder(*this, p));
    } else if (cmd == "/update") {
        // profile data after login (country, age, ...): nothing to keep yet
    } else if (cmd == "/getplayerdata") {
        std::vector<uint8_t> data;
        bool found = store_.Load(arg(1), arg(2), data);
        Log("getplayerdata \"%s\" \"%s\": %s (%u bytes)", arg(1).c_str(), arg(2).c_str(), found ? "found" : "none",
            (unsigned)data.size());
        char n[16];
        snprintf(n, sizeof(n), "%u", (unsigned)data.size());
        Line("/getplayerdata " + Quote(arg(1)) + " " + Quote(arg(2)) + " " + n);
        if (!data.empty()) send_(data.data(), data.size());
    } else if (cmd == "/setplayerdata") {
        long k = strtol(arg(3).c_str(), nullptr, 10);
        if (k < 0 || (size_t)k > kMaxBinary) {
            Log("setplayerdata: bad size %ld", k);
            return;
        }
        binNick_ = arg(1);
        binKey_ = arg(2);
        binNeed_ = (size_t)k;
        state_ = BINARY;
    } else {
        Log("command: %s", line.c_str());
        backend_.OnCommand(*this, w);
    }
}

void Session::OnBinary(const std::vector<uint8_t>& data) {
    Log("setplayerdata \"%s\" \"%s\": %u bytes", binNick_.c_str(), binKey_.c_str(), (unsigned)data.size());
    store_.Save(binNick_, binKey_, data);
}

void Session::UserEntered(const std::string& nick, const std::string& guid) {
    Line("$user " + Quote(nick) + " 0 \"\" " + Quote(guid.empty() ? kNullGuid : guid));
}
void Session::UserLeft(const std::string& nick, const std::string& where) {
    Line("&user " + Quote(nick) + " " + Quote(where));
}
void Session::ChannelAdded(const std::string& name, const std::string& topic) {
    Line("$channel " + Quote(name) + " 0 0 " + Quote(topic));
}
void Session::ChannelRemoved(const std::string& name) { Line("&channel " + Quote(name)); }
void Session::EnteredChannel(const std::string& name, const std::string& topic) {
    channel_ = name;
    Line("/join " + Quote(name) + " 0 0 " + Quote(topic));
}
void Session::ChannelMessage(const std::string& from, const std::string& text) {
    Line("/send " + Quote(from) + " " + Quote(text));
}
void Session::WhisperFrom(const std::string& from, const std::string& text) {
    Line("/msg 0 " + Quote(from) + " 0 " + Quote(text));
}
void Session::WhisperTo(const std::string& to, const std::string& text) {
    Line("/msgc " + Quote(to) + " " + Quote(text));
}
void Session::AdminMessage(const std::string& text) { Line("/admin " + text); }
void Session::GameAdded(const std::string& name, uint32_t ipv4, const std::string& guid) {
    char ip[16];
    snprintf(ip, sizeof(ip), "%u", ipv4);
    Line("$play " + Quote(name) + " 0 0 " + ip + " " + Quote(guid.empty() ? kNullGuid : guid));
}
void Session::GameUpdated(const std::string& name, int players, int maxPlayers, const std::string& level) {
    char buf[64];
    snprintf(buf, sizeof(buf), " %d 0 0 0 ", players);
    std::string l = "@play " + Quote(name) + buf + Quote(level);
    snprintf(buf, sizeof(buf), " %d \"\"", maxPlayers);
    Line(l + buf);
}
void Session::GameRemoved(const std::string& name) { Line("&play " + Quote(name)); }
void Session::Stats(int players, int allPlayers, int games, int allGames, int channels) {
    char buf[96];
    snprintf(buf, sizeof(buf), "/syncstats %d %d %d %d %d 0 0", players, allPlayers, games, allGames, channels);
    Line(buf);
}

void Session::Ladder(LadderPeriod period, const std::vector<LadderRow>& rows) {
    // 0x80A6F0: category index (list 3 of the login reply), then per row
    // "nick" "date" wins losses disconnects points <unused> <unused> <unused>.
    std::string l = period == LADDER_ALL ? "/ladder 0" : period == LADDER_MONTH ? "/ladderm 0" : "/ladderw 0";
    for (size_t i = 0; i < rows.size() && i < 10; i++) {
        const LadderRow& r = rows[i];
        char buf[160];
        snprintf(buf, sizeof(buf), " \"%.6f\" %d %d %d %d 0 0 0", r.lastPlayed, r.wins, r.losses, r.disconnects,
                 r.points < 0 ? 0 : r.points);
        l += " " + Quote(r.nick) + buf;
    }
    Line(l);
}

void Backend::OnHostRequest(Session& s, const std::string& name, const std::string& password) {
    s.Line("/plays " + Quote(name) + " " + Quote(password));
}

// ---------------------------------------------------------------------------
// Offline backend
// ---------------------------------------------------------------------------
void LocalBackend::SendStats(Session& s) {
    s.Stats(1, 1, (int)games_.size(), (int)games_.size(), (int)channels_.size());
}

void LocalBackend::OnLogin(Session& s) {
    channels_.push_back(s.Channel());
    s.ChannelAdded(s.Channel(), "SteamNet");
    SendStats(s);
    s.ChannelMessage("SteamNet", "Tryb offline: kanaly i gry przez Steam jeszcze w budowie.");
}

void LocalBackend::OnJoin(Session& s, const std::string& channel, const std::string& password) {
    (void)password;
    if (channel.empty()) return;
    if (std::find(channels_.begin(), channels_.end(), channel) == channels_.end()) {
        channels_.push_back(channel);
        s.ChannelAdded(channel, "");
    }
    s.EnteredChannel(channel, "");
    s.UserEntered(s.Nick());
    // The client sends /join when it leaves a game room, and says nothing else about the game:
    // the player's games are over.
    for (const std::string& g : games_) s.GameRemoved(g);
    games_.clear();
    SendStats(s);
}

void LocalBackend::OnGameHosted(Session& s, const std::string& name, const std::string& guid) {
    if (std::find(games_.begin(), games_.end(), name) == games_.end()) games_.push_back(name);
    s.GameAdded(name, 0, guid);
    SendStats(s);
}

void LocalBackend::OnSay(Session& s, const std::string& text) { s.ChannelMessage(s.Nick(), text); }

void LocalBackend::OnWhisper(Session& s, const std::string& to, const std::string& text) {
    s.WhisperTo(to, text);
    if (to == s.Nick()) s.WhisperFrom(s.Nick(), text);
}

// ---------------------------------------------------------------------------
// Online ranking

static void CivilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) { // days since 1970-01-01
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = (int)(yoe + era * 400) + (m <= 2);
}

static int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

std::string RankingBoard(LadderPeriod period, int64_t t) {
    int64_t days = (t >= 0 ? t : t - 86399) / 86400;
    int y;
    unsigned m, d;
    char buf[32];
    if (period == LADDER_ALL) return "SteamNet";
    if (period == LADDER_MONTH) {
        CivilFromDays(days, y, m, d);
        snprintf(buf, sizeof(buf), "SteamNet %04d-%02u", y, m);
        return buf;
    }
    // ISO week: the week (Monday..Sunday) belongs to the year of its Thursday
    int wd = (int)(((days % 7) + 7 + 3) % 7); // 0 = Monday (1970-01-01 was a Thursday)
    int64_t thursday = days - wd + 3;
    CivilFromDays(thursday, y, m, d);
    snprintf(buf, sizeof(buf), "SteamNet %04d-W%02d", y, (int)((thursday - DaysFromCivil(y, 1, 1)) / 7 + 1));
    return buf;
}

std::vector<int32_t> RankingDetails(const std::string& nick, int wins, int losses, int disconnects, int64_t lastGame) {
    std::vector<int32_t> d = {1, wins, losses, disconnects, (int32_t)(uint32_t)lastGame, 0, 0, 0, 0};
    for (size_t i = 0; i < nick.size() && i < 16; i++) d[5 + i / 4] |= (int32_t)((uint32_t)(uint8_t)nick[i] << (8 * (i % 4)));
    return d;
}

LadderRow RankingRow(const BoardEntry& e) {
    LadderRow r;
    r.points = e.score;
    r.nick = e.steamName;
    const std::vector<int32_t>& d = e.details;
    if (d.size() >= 9 && d[0] == 1) {
        r.wins = d[1], r.losses = d[2], r.disconnects = d[3];
        if (d[4]) r.lastPlayed = 25569.0 + (double)(uint32_t)d[4] / 86400.0; // unix -> OLE date
        std::string nick;
        for (size_t i = 0; i < 16; i++) {
            char c = (char)((uint32_t)d[5 + i / 4] >> (8 * (i % 4)));
            if (!c) break;
            nick += c;
        }
        if (!nick.empty()) r.nick = nick;
    }
    return r;
}

void RankedBackend::OnLogin(Session& s) {
    LocalBackend::OnLogin(s);
    // A new player shows up on the all-time board with 0 points; "last game" = first login.
    ranking_.Join(RankingBoard(LADDER_ALL, now_()), 0, RankingDetails(s.Nick(), 0, 0, 0, now_()));
}

std::vector<LadderRow> RankedBackend::OnLadder(Session& s, LadderPeriod period) {
    (void)s;
    std::vector<BoardEntry> top;
    std::vector<LadderRow> rows;
    // month / week boards appear with the first result of that period
    if (ranking_.Top(RankingBoard(period, now_()), period == LADDER_ALL, 10, top))
        for (const BoardEntry& e : top) rows.push_back(RankingRow(e));
    return rows;
}

bool MemoryStore::Load(const std::string& nick, const std::string& key, std::vector<uint8_t>& data) {
    auto it = items.find(nick + "\n" + key);
    if (it == items.end()) return false;
    data = it->second;
    return true;
}

void MemoryStore::Save(const std::string& nick, const std::string& key, const std::vector<uint8_t>& data) {
    items[nick + "\n" + key] = data;
}

} // namespace en
