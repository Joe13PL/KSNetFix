// SteamNet - EarthNet server emulator, protocol core. See earthnet_core.h and
// docs/STEAMNET_PROTOCOL.md (client addresses are for KnightShift.ex1).
#define _CRT_SECURE_NO_WARNINGS
#include "earthnet_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

std::vector<uint8_t> Writer::Packet() const {
    Writer p;
    p.u32((uint32_t)b.size() + 4);
    p.raw(b.data(), b.size());
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
            std::vector<uint8_t> body(in_.begin() + 4, in_.begin() + len);
            in_.erase(in_.begin(), in_.begin() + len);
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
    if (!r.ok || li.login.empty()) {
        Log("malformed login packet (%u bytes)", (unsigned)n);
        Writer w;
        w.u32(1);
        w.str("SteamNet: invalid login");
        Packet(w);
        return false;
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
    for (int i = 0; i < 3; i++) w.u8(0xFF); // three empty {index, string} lists
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
    } else if (cmd == "/join") {
        backend_.OnJoin(*this, arg(1), arg(2));
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

// ---------------------------------------------------------------------------
// Offline backend
// ---------------------------------------------------------------------------
void LocalBackend::OnLogin(Session& s) {
    s.ChannelAdded(s.Channel(), "SteamNet");
    s.ChannelMessage("SteamNet", "Tryb offline: kanaly i gry przez Steam jeszcze w budowie.");
}

void LocalBackend::OnJoin(Session& s, const std::string& channel, const std::string& password) {
    (void)password;
    if (channel.empty()) return;
    s.ChannelAdded(channel, "");
    s.EnteredChannel(channel, "");
    s.UserEntered(s.Nick());
}

void LocalBackend::OnSay(Session& s, const std::string& text) { s.ChannelMessage(s.Nick(), text); }

void LocalBackend::OnWhisper(Session& s, const std::string& to, const std::string& text) {
    s.WhisperTo(to, text);
    if (to == s.Nick()) s.WhisperFrom(s.Nick(), text);
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
