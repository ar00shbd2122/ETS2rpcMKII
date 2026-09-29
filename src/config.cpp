/*
 * config.cpp - ets2rpcmkii.ini parsing, default-file creation, hot reload,
 * handcrafted field templates, country mappings. Zero dependencies.
 *
 * Hardened: BOM skipped, oversized files rejected, section/key
 * case-insensitive, malformed lines ignored, every lookup defaulted.
 */
#include "config.h"
#include "plugin_version.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace cfg {

/* ── state ──────────────────────────────────────────────────── */
struct Value {
    std::string raw;
    bool        boolean = false;
    long        number  = 0;
};

struct Store {
    std::map<std::string, Value> kv;
    std::vector<std::pair<std::string, std::string>> brands;
    std::vector<std::pair<std::string, std::string>> countries;
    std::map<std::string, std::string> wrap;               /* v2 [templates] */
    FieldTemplates         gtpl;                           /* [template]     */
    std::map<std::string, FieldTemplates> stpl;            /* [template.x]   */
    std::string path;
    long long   mtime = 0;
    long long   last_stat = 0;                             /* reload throttle */
};
static Store g;

static void (*g_log_cb)(const char*) = nullptr;
static void clog(const char* msg) { if (g_log_cb) g_log_cb(msg); }

static const FieldTemplates kEmptyFt;

/* ── default ini (written on first launch) ──────────────────── */
static const char* DEFAULT_INI =
"; ============================================================\n"
";  ETS2rpcMKII configuration  (v" ETS2RPCMKII_VERSION ")\n"
";  Hot-reloaded: save this file and the presence updates within\n"
";  a couple of seconds. No game restart needed.\n"
";  Delete this file to regenerate the defaults.\n"
"; ============================================================\n"
"\n"
"[discord]\n"
"; Application ID from https://discord.com/developers/applications\n"
"application_id = 1553660903986045029\n"
"\n"
"[presence]\n"
"; Toggle individual data on/off\n"
"show_speed       = 1\n"
"show_fuel        = 0\n"
"show_time        = 1\n"
"show_truck_badge = 1\n"
"\n"
"[behaviour]\n"
"; metric or imperial (mi, mph, short tons)\n"
"units = metric\n"
"; Seconds between Discord updates (Discord throttles hard below ~5s)\n"
"update_interval = 5\n"
"; Seconds timed events (fine, tollgate, delivered, ferry, train) stay visible\n"
"event_hold = 30\n"
"; 24h clock (1) or 12h AM/PM (0) for the {time} token\n"
"use_24h = 1\n"
"; Speeding state: trigger at this % above the current speed limit\n"
"speeding_threshold_pct = 10\n"
"; Damage states: trigger at these wear/damage percentages\n"
"cargo_damage_threshold  = 25\n"
"chassis_wear_threshold  = 50\n"
"\n""[countries]\n"
"; City-substring -> country tag for the {country} text tokens,\n"
"; and, ON YOUR OWN APPLICATION (see [discord] above), the flag\n"
"; art key for the badge when the value is a valid asset key.\n"
"; First match wins. The built-in table of ~90 cities runs after\n"
"; your entries, so mod-map cities work here too.\n"
";dresden      = Germany          ; text only on the shared app\n"
";dresden      = de               ; text + flag art on own app\n"
"\n"
"[brands]\n"
"; Custom truck-brand art mappings. WORKS ONLY ON YOUR OWN\n"
"; APPLICATION (application_id different from the repo id):\n"
"; upload a logo in the Developer Portal, then map a case-\n"
"; insensitive substring of the truck brand to the asset key.\n"
"; On the shared repo application these lines are ignored.\n"
";my_mod_truck = my_mod_logo\n"
";renault      = renault_trucks\n"
"\n"
"; CORE ART KEYS (checked against the portal before anything is\n"
"; sent): own app: generic (unknown trucks), generic_c (unknown\n"
"; countries) and ets2 (big image). Missing core keys are simply\n"
"; not sent; nothing ghosts as a text line.\n"
"\n"
"; ============================================================\n"
";  HANDCRAFTED TEMPLATES\n"
";  Design every field of the presence yourself. Each field is\n"
";  filled with tokens ({cargo}, {dest}, {speed}...) first, then\n"
";  clamped to Discord's limits. See DOCS.md for the full manual.\n"
";\n"
";  Fields:\n"
";    state / details        the two text lines (details = upper,\n"
";                           state = lower)\n"
";    button1_label/button1_url   first button\n"
";    button2_label/button2_url   second button\n"
";\n"
";  IMAGES ARE FIXED: big image = official game icon, small badge =\n"
";  truck brand logo (generic art for mods). large_image,\n"
";  large_text, small_image and small_text lines in the ini are\n"
";  ignored on purpose - see the note above [countries].\n"
";\n"
";  Rules:\n"
";    unset (no line)  -> use the smart built-in default\n"
";    n/a              -> clear/skip that field entirely\n"
";    any other text   -> used verbatim (after token fill)\n"
";\n"
";  Per-state sections [template.<key>] override single fields for\n"
";  that state only; unset fields fall back to [template].\n"
";  Keys: main_menu, free_roam, delivery_active, delivery_complete,\n"
";        paused, on_ferry, on_train, resting, got_fine, tollgate,\n"
";        cargo_damaged, truck_damaged, speeding\n"
";\n"
";  v5: typo'd keys and unknown template states are reported to\n"
";  game.log.txt as 'ini: ...' lines once per config load.\n"
";\n"
";  NOTE on image keys: an image field is an ASSET KEY uploaded to\n"
";  your Discord application (Developer Portal -> Rich Presence ->\n"
";  Art Assets). A key with no art behind it does not render an\n"
";  image: Discord then shows the tooltip text as a plain line.\n"
";  That is the 'third description' effect - it always means the\n"
";  art for that key is missing. Run tools/download_assets.sh and\n"
";  upload everything to make every badge render:\n"
";    ets2          official round game icon (the big image)\n"
";    scania, volvo, daf, ...   brand logos for the corner badge\n"
";    flag_de, ...  circular country flags\n"
";\n"
";  Art for the fixed images comes from tools/download_assets.sh:\n"
";  upload ets2 plus the brand logos (scania, volvo, ...) in the\n"
";  Developer Portal, Rich Presence, Art Assets.\n"
";  simply does not render (text still works).\n"
"; ============================================================\n"
"\n"
"[template]\n"
"; Base fields for every state; per-state sections below override.\n"
"; --- Get on Steam button, like on official game profiles ---\n"
"; (others see the buttons on your profile; you cannot see your own -\n"
";  that is a Discord limitation)\n"
"button1_label = Get on Steam\n"
"button1_url   = https://store.steampowered.com/app/227300/Euro_Truck_Simulator_2/\n"
";button2_label = Join my Convoy\n"
";button2_url   = https://discord.gg/truckersmp\n"
"\n"
"[template.delivery_active]\n"
"state        = {src} → {dest}{progress_tag}\n"
"details      = {cargo} ({mass}) ・ {distance} {distance_unit} ・ ETA {eta_clock}\n"

"\n"
"[template.free_roam]\n"
"state        = Cruising through {country}{country_tag}\n"
"details      = {truck} ・ {speed} {speed_unit}\n"

"\n"
"[template.speeding]\n"
"state        = {speed_limit} limit ・ {over_limit}% over\n"
"details      = {truck} ・ {speed} {speed_unit}\n"

"\n"
"[template.delivery_complete]\n"
"state        = Earned {income}\n"
"details      = Delivered {cargo} to {dest}\n"

"\n"
"[template.got_fine]\n"
"state        = Fined €{fine}\n"
"details      = Watch the road\n"

"\n"
"[template.tollgate]\n"
"state        = → {dest}\n"
"details      = Toll booth passed\n"

"\n"
"[template.cargo_damaged]\n"
"state        = {cargo} ・ {damage}% damaged\n"
"details      = Drive gently to {dest}\n"

"\n"
"[template.truck_damaged]\n"
"state        = {truck} ・ {wear}% wear\n"
"details      = Find a garage soon\n"

"\n"
"[template.on_ferry]\n"
"state        = On a ferry → {ferry_to}\n"
"details      = Crossing from {ferry_from}\n"

"\n"
"[template.on_train]\n"
"state        = On a train → {ferry_to}\n"
"details      = Rail freight from {ferry_from}\n"

"\n"
"[template.resting]\n"
"state        = Resting before {dest}\n"
"details      = Engine off ・ {time}\n"

"\n"
"[template.paused]\n"
"state        = {src} → {dest}\n"
"details      = Paused\n"

"\n"
"[template.main_menu]\n"
"state        = Main Menu\n"
"details      = Planning the next haul\n";

/* ── small helpers ──────────────────────────────────────────── */
static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool parse_bool(const std::string& v) {
    std::string s = lower(trim(v));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

static long long file_mtime(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((long long)fad.ftLastWriteTime.dwHighDateTime << 32)
         | fad.ftLastWriteTime.dwLowDateTime;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return "";
    std::streamoff sz = f.tellg();
    if (sz <= 0 || sz > (4 << 20)) return "";        /* missing or > 4 MB: refuse */
    std::string out; out.resize((size_t)sz);
    f.seekg(0);
    f.read(&out[0], sz);
    out.resize((size_t)f.gcount());
    return out;
}

/* strip a UTF-8/UTF-16 BOM if present */
static void strip_bom(std::string& s) {
    if (s.size() >= 3 &&
        (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
}

/* write a field into a FieldTemplates struct by name */
static void ft_set(FieldTemplates& ft, const std::string& key, const std::string& val) {
    if      (key == "state")         ft.state        = val;
    else if (key == "details")       ft.details      = val;
    else if (key == "large_image")   ft.large_image  = val;
    else if (key == "large_text")    ft.large_text   = val;
    else if (key == "small_image")   ft.small_image  = val;
    else if (key == "small_text")    ft.small_text   = val;
    else if (key == "button1_label" || key == "btn1_label") ft.btn1_label = val;
    else if (key == "button1_url"   || key == "btn1_url")   ft.btn1_url   = val;
    else if (key == "button2_label" || key == "btn2_label") ft.btn2_label = val;
    else if (key == "button2_url"   || key == "btn2_url")   ft.btn2_url   = val;
}

/* ── parsing ────────────────────────────────────────────────── */
static void parse(std::string text) {
    Store fresh;
    strip_bom(text);

    std::string section;      /* raw, may be "template.delivery_active" */
    std::istringstream in(text);
    std::string line;

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            section = lower(line.substr(1, line.size() - 2));
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = lower(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if (section == "brands") {
            fresh.brands.push_back({ key, val });
        }
        else if (section == "countries") {
            /* city hints for the {country} text tokens; on own
             * applications a value that looks like an asset key doubles
             * as the flag art for the badge */
            fresh.countries.push_back({ key, val });
        }
        else if (section == "templates") {              /* v2 compat */
            if      (key == "state")   fresh.wrap["state"]   = val;
            else if (key == "details") fresh.wrap["details"] = val;
        }
        else if (section == "template") {
            ft_set(fresh.gtpl, key, val);
        }
        else if (section.rfind("template.", 0) == 0) {
            ft_set(fresh.stpl[section.substr(9)], key, val);
        }
        else if (!section.empty()) {
            Value v; v.raw = val;
            v.boolean = parse_bool(val);
            v.number  = strtol(val.c_str(), nullptr, 10);
            fresh.kv[section + "." + key] = v;
        }
    }
    fresh.path  = g.path;
    fresh.mtime = g.mtime;
    g = std::move(fresh);
}

/* FAILSAFE: if the ini is missing, empty or unreadable the baked-in
 * DEFAULT_INI is used immediately, so the presence always comes up.
 * The defaults are also written to disk so the user can edit them. */
static void load_or_create() {
    std::string text = read_file(g.path);
    if (text.empty()) {
        clog("ets2rpcmkii.ini not found or unreadable; using the defaults baked into the DLL.");
        HANDLE h = CreateFileA(g.path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, DEFAULT_INI, (DWORD)strlen(DEFAULT_INI), &w, nullptr);
            CloseHandle(h);
            clog("Default ets2rpcmkii.ini written next to the DLL; edit it to taste (hot-reloaded).");
        } else {
            clog("Could not write ets2rpcmkii.ini (read-only folder?); baked-in defaults stay active.");
        }
        text = DEFAULT_INI;
    }
    g.mtime = file_mtime(g.path);
    parse(std::move(text));
    validate_schema();
    g.last_stat = 0;                     /* allow an immediate re-stat */
}

/* ── lifecycle ──────────────────────────────────────────────── */
void set_log_callback(void (*fn)(const char* msg)) { g_log_cb = fn; }

void init(const std::string& directory) {
    g.path = directory + "\\ets2rpcmkii.ini";
    load_or_create();
}

void maybe_reload() {
    if (g.path.empty()) return;

    /* v5.0: this runs per game frame; stat the disk at most once per
     * second. Hot reload stays instant to the human eye, the file
     * system stops being hammered 60 times a second. */
    long long now = (long long)GetTickCount64();
    if (g.last_stat != 0 && now - g.last_stat < 1000) return;
    g.last_stat = now;

    long long m = file_mtime(g.path);
    if (m == 0) {
        if (g.mtime != 0) {
            /* the ini existed and is gone now: fall back to the
             * baked-in defaults and regenerate the file */
            clog("ets2rpcmkii.ini disappeared; falling back to the defaults baked into the DLL.");
            load_or_create();
        }
        return;
    }
    if (m != g.mtime) {
        std::string text = read_file(g.path);
        if (!text.empty()) {          /* ignore transient read failures */
            g.mtime = m;
            parse(std::move(text));
            validate_schema();        /* warn about typos on every reload */
        }
    }
}

/* ── settings ───────────────────────────────────────────────── */
uint64_t app_id() {
    auto it = g.kv.find("discord.application_id");
    if (it == g.kv.end() || it->second.raw.empty()) return 0;
    return strtoull(it->second.raw.c_str(), nullptr, 10);
}

bool custom_art_allowed() {
    uint64_t id = app_id();
    return id != 0 && id != REPO_APP_ID;
}

bool valid_asset_key(const std::string& key) {
    if (key.empty() || key.size() > 32) return false;
    for (char c : key) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}

static bool getb(const char* key, bool dflt) {
    auto it = g.kv.find(key);
    return it != g.kv.end() ? it->second.boolean : dflt;
}
static long getn(const char* key, long dflt, long lo, long hi) {
    auto it = g.kv.find(key);
    if (it == g.kv.end()) return dflt;
    long v = it->second.number;
    return v < lo ? lo : (v > hi ? hi : v);
}

bool show_speed()       { return getb("presence.show_speed", true); }
bool show_fuel()        { return getb("presence.show_fuel",  true); }
bool show_time()        { return getb("presence.show_time",  true); }
bool show_truck_badge() { return getb("presence.show_truck_badge", true); }

int  update_interval()  { return (int)getn("behaviour.update_interval", 5, 3, 3600); }
int  event_hold()       { return (int)getn("behaviour.event_hold", 20, 0, 600); }
bool use_24h()          { return getb("behaviour.use_24h", true); }
int  speed_limit_pct()  { return (int)getn("behaviour.speeding_threshold_pct", 15, 1, 200); }
int  cargo_damage_pct() { return (int)getn("behaviour.cargo_damage_threshold", 45, 1, 100); }
int  chassis_wear_pct() { return (int)getn("behaviour.chassis_wear_threshold", 65, 1, 100); }
bool imperial() {
    auto it = g.kv.find("behaviour.units");
    return it != g.kv.end() && lower(it->second.raw) == "imperial";
}

const std::vector<std::pair<std::string, std::string>>& countries() { return g.countries; }
const std::vector<std::pair<std::string, std::string>>& custom_brands() { return g.brands; }

/* ── templates ──────────────────────────────────────────────── */
const FieldTemplates& global_template() { return g.gtpl; }

const FieldTemplates& state_template(const std::string& key) {
    auto it = g.stpl.find(key);
    return it != g.stpl.end() ? it->second : g.gtpl;
}

bool any_template_set() {
    return !g.gtpl.state.empty()    || !g.gtpl.details.empty()
        || !g.gtpl.large_image.empty() || !g.gtpl.large_text.empty()
        || !g.gtpl.small_image.empty() || !g.gtpl.small_text.empty()
        || !g.gtpl.btn1_label.empty()  || !g.gtpl.btn1_url.empty()
        || !g.gtpl.btn2_label.empty()  || !g.gtpl.btn2_url.empty()
        || !g.stpl.empty();
}

/* v2 wrapper compat, forward-scanning and loop-safe */
static std::string replace_token(std::string t, const char* token, const std::string& val) {
    const size_t tl = strlen(token);
    size_t p = 0;
    while ((p = t.find(token, p)) != std::string::npos) {
        t.replace(p, tl, val);
        p += val.size();                    /* continue past the inserted text */
    }
    return t;
}

std::string tpl_state(const std::string& state, const std::string& details) {
    auto s = g.wrap.find("state");
    if (s == g.wrap.end()) return state;
    std::string t = replace_token(s->second, "{details}", details);
    return       replace_token(t,           "{state}",   state);
}

std::string tpl_details(const std::string& details) {
    auto s = g.wrap.find("details");
    if (s == g.wrap.end()) return details;
    return replace_token(s->second, "{details}", details);
}

/* ── ini schema report (v5.0) ────────────────────────────────
 * The parser forgives everything; this pass tells the user what it
 * forgave, once per load, in plain game-log lines. Catches: unknown
 * keys in known sections (typos), unknown template state names, and
 * numeric values outside the accepted range (they are clamped). */
static const char* const KNOWN_BOOLS[] = {
    "presence.show_speed", "presence.show_fuel", "presence.show_time",
    "presence.show_truck_badge", "behaviour.use_24h",
};
static const char* const KNOWN_NUMS[] = {
    "behaviour.update_interval", "behaviour.event_hold",
    "behaviour.speeding_threshold_pct", "behaviour.cargo_damage_threshold",
    "behaviour.chassis_wear_threshold",
};
static const char* const KNOWN_STATES[] = {
    "main_menu", "free_roam", "delivery_active", "delivery_complete",
    "paused", "on_ferry", "on_train", "resting", "got_fine", "tollgate",
    "cargo_damaged", "truck_damaged", "speeding",
};
static const char* const TEMPLATE_FIELDS[] = {
    "state", "details", "large_image", "large_text", "small_image",
    "small_text", "button1_label", "button1_url", "btn1_label", "btn1_url",
    "button2_label", "button2_url", "btn2_label", "btn2_url",
};

static bool contains(const char* const* arr, size_t n, const std::string& s) {
    for (size_t i = 0; i < n; ++i)
        if (s == arr[i]) return true;
    return false;
}

void validate_schema() {
    /* known top-level keys per section */
    static const char* const SECT_DISCORD   = "discord.";
    static const char* const SECT_PRESENCE  = "presence.";
    static const char* const SECT_BEHAVIOUR = "behaviour.";

    for (const auto& kv : g.kv) {
        const std::string& k = kv.first;
        bool known =
            (k.rfind(SECT_DISCORD, 0)   == 0 && k == "discord.application_id") ||
            (k.rfind(SECT_PRESENCE, 0)  == 0 &&
             contains(KNOWN_BOOLS, sizeof(KNOWN_BOOLS)  / sizeof(*KNOWN_BOOLS),  k)) ||
            (k.rfind(SECT_BEHAVIOUR, 0) == 0 &&
             contains(KNOWN_NUMS,  sizeof(KNOWN_NUMS)   / sizeof(*KNOWN_NUMS),   k)) ||
            k == "behaviour.units";
        if (known) continue;

        std::string sec = k.substr(0, k.find('.'));
        std::string key = k.substr(k.find('.') + 1);
        std::string msg = "ini: unknown key '" + key + "' in [" + sec +
                          "] (ignored by the plugin; check for a typo)";
        clog(msg.c_str());
    }

    for (const auto& p : g.stpl) {
        if (!contains(KNOWN_STATES, sizeof(KNOWN_STATES) / sizeof(*KNOWN_STATES), p.first)) {
            std::string msg = "ini: unknown template state '[template." + p.first +
                              "]' (valid: main_menu, free_roam, delivery_active, ...)";
            clog(msg.c_str());
        }
    }

    /* numeric keys outside their accepted range: the value is clamped,
     * say so instead of letting the user wonder why 9999 does nothing */
    struct Range { const char* key; long lo, hi; };
    static const Range RANGES[] = {
        { "behaviour.update_interval",          3,   3600 },
        { "behaviour.event_hold",               0,    600 },
        { "behaviour.speeding_threshold_pct",   1,    200 },
        { "behaviour.cargo_damage_threshold",   1,    100 },
        { "behaviour.chassis_wear_threshold",   1,    100 },
    };
    for (const auto& r : RANGES) {
        auto it = g.kv.find(r.key);
        if (it == g.kv.end()) continue;
        long v = it->second.number;
        if (v < r.lo || v > r.hi) {
            std::string key = r.key;
            std::string msg = "ini: " + key.substr(key.find('.') + 1) + " = " +
                it->second.raw + " is out of range (" +
                std::to_string(r.lo) + " to " + std::to_string(r.hi) +
                "), the plugin uses the nearest allowed value";
            clog(msg.c_str());
        }
    }

}

} /* namespace cfg */
