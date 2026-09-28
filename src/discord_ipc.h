/*
 * discord_ipc.h - minimal Discord Rich Presence client over the
 * documented local IPC protocol (named pipes on Windows).
 *
 * No Discord SDK, no external DLLs, no OAuth. Talks straight to the
 * running Discord desktop client via \\.\pipe\discord-ipc-N.
 *
 * All functions are non-blocking and safe to call from a game thread.
 * Every function tolerates: no Discord running, Discord quitting
 * mid-write, oversized/garbled payloads. Worst case the call returns
 * false and pump() reconnects later.
 */
#pragma once
#include <string>
#include <cstdint>

namespace discord_ipc {

/* Everything a presence can carry. Empty string = field omitted. */
struct Presence {
    std::string state, details;
    std::string large_image, large_text;
    std::string small_image, small_text;
    std::string btn1_label, btn1_url;    /* buttons need label AND url,
                                            url must be http(s) */
    std::string btn2_label, btn2_url;
    int64_t     start_unix = 0;          /* elapsed timer; 0 = none   */
    bool        clear_activity = false;  /* true -> presence removed  */
};

/* Try to connect + handshake with the Discord desktop client.
 * Returns true when connected. Safe to call repeatedly. */
bool connect(uint64_t application_id);

/* True while the pipe is open. */
bool connected();

/* Drop the connection. */
void disconnect();

/* Send the presence described by p. Returns true if written. */
bool set(const Presence& p);

/* Convenience: clear the current presence. */
bool clear();

/* Pump inbound frames (READY / PING→PONG) and auto-reconnect every
 * ~15 s when disconnected. Call once per frame from the game loop. */
void pump(uint64_t application_id);

/* Optional logging hook so handshake results and Discord error frames
 * become visible. Pass the game-log writer, or nullptr to disable. */
void set_log_callback(void (*fn)(const char* msg));

} /* namespace discord_ipc */
