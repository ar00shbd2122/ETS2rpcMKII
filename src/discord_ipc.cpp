/*
 * discord_ipc.cpp - Discord Rich Presence over the local IPC protocol.
 *
 * Protocol (see discord's archived discord-rpc repo):
 *   pipe name : \\.\pipe\discord-ipc-0 .. -9
 *   frames    : 4-byte LE op | 4-byte LE length | payload (JSON)
 *   ops       : 0=HANDSHAKE, 1=FRAME, 2=CLOSE, 3=PING, 4=PONG
 *   handshake : { "v":1, "client_id":"<appid>" } → reply evt:"READY"
 *
 * Hardened: every Win32 call checked, dead-pipe detection, malformed
 * frame length caps, non-blocking (peek-based) inbound drain, bounded
 * reconnect throttle. Any failure just drops the pipe; pump() retries.
 */
#include "discord_ipc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>

namespace discord_ipc {

/* ── JSON helpers ────────────────────────────────────────────────── */
static std::string jesc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if ((unsigned char)c < 0x20) {
                char b[8]; snprintf(b, sizeof(b), "\\u%04X", (unsigned char)c); o += b;
            } else o += c;
        }
    }
    return o;
}

static std::string jstr(const std::string& v) { return "\"" + jesc(v) + "\""; }

/* append "key":"value" if value non-empty; sep must point at a mutable ",x" buffer */
static void jfield(std::string& out, const char* key, const std::string& v) {
    if (v.empty()) return;
    if (out.size() && out.back() != '{') out += ',';
    out += jstr(key); out += ':'; out += jstr(v);
}

/* - connection state ---------------------------------------------- */
static HANDLE   g_pipe     = INVALID_HANDLE_VALUE;
static int64_t  g_last_try = 0;
static int      g_nonce    = 0;
static void (*g_log_cb)(const char*) = nullptr;
static bool     g_broken_logged = false;
static bool     g_error_logged  = false;   /* log one rejection per connect,
                                              not one per push (5 s spam) */

static void dlog(const char* msg) { if (g_log_cb) g_log_cb(msg); }

static void note_broken_pipe() {
    if (!g_broken_logged) {
        g_broken_logged = true;
        dlog("Discord pipe lost; Discord may have closed it. Retrying every 15 s.");
    }
}

/* Decode the frames Discord sends back so rejections are visible. */
static void log_discord_frame(const std::string& payload) {
    if (payload.find("\"evt\":\"READY\"") != std::string::npos) {
        g_broken_logged = false;
        g_error_logged  = false;
        dlog("Discord handshake accepted (READY). Presence updates are live.");
        return;
    }
    if (payload.find("\"evt\":\"ERROR\"") != std::string::npos) {
        if (g_error_logged) return;          /* same reason every 5 s: once is enough */
        g_error_logged = true;
        std::string msg;
        size_t p = payload.find("\"message\":\"");
        if (p != std::string::npos) {
            p += 11;
            for (; p < payload.size(); ++p) {
                char c = payload[p];
                if (c == '\\' && p + 1 < payload.size()) { msg += payload[p + 1]; ++p; continue; }
                if (c == '"') break;
                msg += c;
            }
        }
        std::string out = "Discord rejected the presence: ";
        out += msg.empty() ? payload.substr(0, 160) : msg;
        dlog(out.c_str());
    }
}

static const int64_t RETRY_INTERVAL_S = 15;

static int64_t now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static void close_pipe() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
}

/* Write a full frame: op(4, LE) + len(4, LE) + payload.
 * Bounded: payload capped at 16 KB (protocol limit is 64 KB). */
static bool write_frame(int op, const std::string& payload) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    if (payload.size() > (1u << 14))    return false;

    unsigned char hdr[8];
    uint32_t o = (uint32_t)op, n = (uint32_t)payload.size();
    memcpy(hdr + 0, &o, 4);
    memcpy(hdr + 4, &n, 4);

    DWORD w = 0;
    if (!WriteFile(g_pipe, hdr, 8, &w, nullptr) || w != 8) {
        close_pipe(); note_broken_pipe(); return false;
    }
    if (!payload.empty()) {
        if (!WriteFile(g_pipe, payload.data(), (DWORD)payload.size(), &w, nullptr)
            || w != payload.size()) {
            close_pipe(); note_broken_pipe(); return false;
        }
    }
    return true;
}

/* Drain inbound frames without blocking. PING → PONG. Anything else ignored. */
static void drain_inbound() {
    if (g_pipe == INVALID_HANDLE_VALUE) return;

    DWORD avail = 0;
    int guard = 0;
    while (guard++ < 16 &&
           PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail >= 8) {

        unsigned char hdr[8];
        DWORD r = 0;
        if (!ReadFile(g_pipe, hdr, 8, &r, nullptr) || r != 8) { close_pipe(); return; }

        uint32_t op, len;
        memcpy(&op,  hdr,     4);
        memcpy(&len, hdr + 4, 4);
        if (len > (1u << 20)) { close_pipe(); return; }   /* insane length: bail */

        std::string payload(len, '\0');
        if (len > 0) {
            if (!ReadFile(g_pipe, payload.data(), len, &r, nullptr) || r != len) {
                close_pipe(); return;
            }
        }

        if (op == 1) log_discord_frame(payload);
        if (op == 3) write_frame(4, payload);             /* PING → PONG */
        /* READY / PONG / dispatch events: nothing to do (fire-and-forget) */
    }
}

/* ── public ──────────────────────────────────────────────────────── */

bool connect(uint64_t application_id) {
    close_pipe();
    if (application_id == 0) return false;                /* refuse nonsense */

    char name[64];
    for (int i = 0; i < 10; ++i) {
        snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE p = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (p != INVALID_HANDLE_VALUE) { g_pipe = p; break; }
    }
    if (g_pipe == INVALID_HANDLE_VALUE) return false;

    /* If the pipe can't be peeked it's already dead, bail early. */
    if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, nullptr, nullptr)) {
        close_pipe();
        return false;
    }

    char cid[32];
    snprintf(cid, sizeof(cid), "%llu", (unsigned long long)application_id);
    std::string hs = "{\"v\":1,\"client_id\":" + jstr(cid) + "}";
    if (!write_frame(0, hs)) return false;

    g_last_try = now_s();
    g_broken_logged = false;
    g_error_logged  = false;
    return true;
}

bool connected() { return g_pipe != INVALID_HANDLE_VALUE; }

void disconnect() { close_pipe(); }

bool set(const Presence& p) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;

    char nonce[32];
    snprintf(nonce, sizeof(nonce), "%d", ++g_nonce);

    /* Clearing requires "activity":null. An empty object does NOT
       remove the presence on Discord's side. */
    std::string act = p.clear_activity ? std::string("null") : std::string("{");

    if (!p.clear_activity) {
        jfield(act, "state",    p.state);
        jfield(act, "details",  p.details);

        if (p.start_unix > 0)
            act += ",\"timestamps\":{\"start\":" + std::to_string(p.start_unix) + "}";

        /* Images MUST live under the "assets" object, exactly like
         * smallImageKey / smallImageText in the official discord-rpc
         * struct. Flat large_image / small_image at activity level are
         * silently dropped by Discord as unknown keys - which is why
         * the badges never rendered before, with no error logged. */
        std::string assets;
        jfield(assets, "large_image", p.large_image);
        jfield(assets, "large_text",  p.large_text);
        jfield(assets, "small_image", p.small_image);
        jfield(assets, "small_text",  p.small_text);
        if (!assets.empty()) act += ",\"assets\":{" + assets + "}";

        /* buttons: Discord requires label + http(s) url pairs, max 2 */
        auto url_ok = [](const std::string& u) {
            return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0;
        };
        int buttons = 0;
        if (!p.btn1_label.empty() && url_ok(p.btn1_url)) ++buttons;
        if (!p.btn2_label.empty() && url_ok(p.btn2_url)) ++buttons;

        if (buttons > 0) {
            act += ",\"buttons\":[";
            bool first = true;
            auto add = [&](const std::string& l, const std::string& u) {
                if (l.empty() || !url_ok(u)) return;
                if (!first) act += ',';
                first = false;
                act += "{\"label\":" + jstr(l) + ",\"url\":" + jstr(u) + "}";
            };
            add(p.btn1_label, p.btn1_url);
            add(p.btn2_label, p.btn2_url);
            act += ']';
        }

        /* "instance" must be a JSON boolean in Discord's current schema;
         * the legacy 1/0 integer form is now rejected with a 4005-style
         * validation error and the presence never renders. */
        act += ",\"instance\":true";
    }

    act += "}";

    std::string frame = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
        std::to_string((unsigned long long)GetCurrentProcessId()) +
        ",\"activity\":" + act + "},\"nonce\":" + jstr(nonce) + "}";

    return write_frame(1, frame);
}

bool clear() {
    Presence p;
    p.clear_activity = true;
    return set(p);
}

void pump(uint64_t application_id) {
    int64_t now = now_s();

    if (g_pipe == INVALID_HANDLE_VALUE) {
        if (now - g_last_try >= RETRY_INTERVAL_S) {
            g_last_try = now;                     /* throttle regardless of outcome */
            connect(application_id);
        }
        return;
    }
    /* dead-pipe detection: any failure here drops and retries later */
    DWORD flags = 0;
    if (!GetNamedPipeInfo(g_pipe, &flags, nullptr, nullptr, nullptr) ||
        !PeekNamedPipe(g_pipe, nullptr, 0, nullptr, nullptr, nullptr)) {
        close_pipe(); note_broken_pipe();
        return;
    }

    drain_inbound();
}

void set_log_callback(void (*fn)(const char* msg)) { g_log_cb = fn; }

} /* namespace discord_ipc */
