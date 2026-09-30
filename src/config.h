/*
 * config.h - ets2rpcmkii.ini configuration + handcrafted template engine
 *            for ETS2rpcMKII.
 *
 * The ini lives next to the DLL. If missing, a commented default is
 * written on first launch. Hot-reloaded on mtime change.
 *
 * v3 additions:
 *   • field-level handcrafted templates: every presence field can be
 *     designed individually: state, details, large image + tooltip,
 *     small image + tooltip, and two buttons. "n/a" skips the field.
 *   • per-state variants: [template.delivery_active] etc.
 *   • [countries] section: city-substring → country-key mappings.
 */
#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <utility>

namespace cfg {

/* The shared application id shipped with the repo. Presence sent under
 * this id uses ONLY the built-in art set: custom [brands] and [countries]
 * image mappings are ignored, because the art lives on the owner's
 * application and nobody else can add to it. Users who create their OWN
 * application and put its id below unlock custom art mappings. */
constexpr uint64_t REPO_APP_ID = 1553660903986045029ULL;

/* True when the configured application id is present and is NOT the
 * repo id, i.e. the user runs their own Discord application and custom
 * art mappings may be sent. */
bool custom_art_allowed();

/* A valid Discord art asset key: 1 to 32 chars, only a-z, 0-9,
 * underscore and hyphen. Anything else is rejected by the portal and
 * therefore rejected here. */
bool valid_asset_key(const std::string& key);

/* Load (or create) ets2rpcmkii.ini in the given directory. */
void init(const std::string& directory);

/* The directory passed to init(): where the ini lives and where
 * plugin-side state (ets2rpcmkii.job) belongs. Empty before init. */
const std::string& state_directory();

/* Optional log hook for failsafe notices (ini missing, defaults
 * written, file vanished mid-session). Pass the game-log writer,
 * or nullptr to disable. */
void set_log_callback(void (*fn)(const char* msg));

/* Re-read the ini if it changed on disk. Stats the file at most
 * once per second (the hot-reload path runs per game frame). */
void maybe_reload();

/* ── ini schema report (v5.0) ───────────────────────────────
 * Catches the classic mistakes the parser silently forgave before:
 * unknown keys (typos like 'evnt_hold = 30'), unknown template state
 * names, and values that get silently clamped. Each finding is
 * logged once per config load as
 *   [ETS2rpcMKII] ini: <what> '<key>' (...)
 * A warning never blocks: the rest of the ini stays live. */
void validate_schema();

/* ── [discord] ───────────────────────────────────────────── */
uint64_t app_id();

/* ── [presence] toggles ──────────────────────────────────── */
bool show_speed();        /* speed in free-roam/delivery text     */
bool show_fuel();         /* fuel % in delivery text              */
bool show_time();         /* elapsed-time timer                   */
bool show_truck_badge();  /* small brand-logo badge on/off        */

/* ── [behaviour] ─────────────────────────────────────────── */
int  update_interval();   /* seconds between pushes (min 3)       */
int  event_hold();        /* timed event visibility (seconds)     */
bool use_24h();           /* {time} token format                  */
bool imperial();          /* units = imperial                     */
int  speed_limit_pct();   /* speeding threshold, % over limit     */
int  cargo_damage_pct();  /* cargo-damaged state threshold        */
int  chassis_wear_pct();  /* truck-damaged state threshold        */

/* ── [countries] city hints +, on own apps, flag art mappings ─ */
const std::vector<std::pair<std::string, std::string>>& countries();

/* ── [brands] custom art mappings, own applications only ─── */
const std::vector<std::pair<std::string, std::string>>& custom_brands();

/* ── handcrafted templates ───────────────────────────────── */
struct FieldTemplates {
    std::string state;        /* "" = default; "n/a" = clear field */
    std::string details;
    std::string large_image, large_text;
    std::string small_image, small_text;
    std::string btn1_label, btn1_url;
    std::string btn2_label, btn2_url;
};

/* Global [template], applied to all states. */
const FieldTemplates& global_template();

/* Per-state [template.<state>]: fields here beat the global ones.
 * Falls back to global for unset fields; "n/a" clears the field. */
const FieldTemplates& state_template(const std::string& state_key);

/* True if any handcrafted field is configured at all. */
bool any_template_set();

/* Legacy [templates] wrapper support (v2 compat): wraps raw text. */
std::string tpl_state(const std::string& state, const std::string& details);
std::string tpl_details(const std::string& details);

} /* namespace cfg */
