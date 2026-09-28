/*
 * ETS2rpcMKII - Discord Rich Presence - plugin.cpp (v4.1)
 * Single self-contained DLL plugin for Euro Truck Simulator 2
 *
 *   Built against the real official SCS Telemetry SDK headers
 *   (SCS_TELEMETRY_VERSION 1.00, game_version 1.18).
 *
 *   Crash-proof: every entry point runs under SEH guards; any internal
 *   fault is swallowed and logged, the game never goes down with us.
 *   Handcrafted templates: design every presence field yourself:
 *   state, details, large/small images + tooltips, two buttons.
 *   "n/a" skips a field; unset fields use professional defaults.
 *   Country mode: small badge becomes a circular flag of the country
 *   you are currently driving in (from your uploaded flag assets).
 *   ets2rpcmkii.ini hot-reload, metric/imperial, 13 presence states
 *   incl. speeding, tollgate, ETA, job income.
 *   No Discord SDK, no external DLLs. Raw IPC (src/discord_ipc.cpp).
 *
 * Build:  CMake (see CMakeLists.txt), MSVC x64 or MinGW-w64,
 *         or just run build_msvc.bat
 * Drop:   ETS2/bin/win_x64/plugins/ets2rpcmkii.dll
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <string>
#include <map>
#include <algorithm>
#include <cmath>

/* Real official SCS SDK */
#include "scssdk_telemetry.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"

/* ETS2rpcMKII */
#include "plugin_version.h"
#include "discord_ipc.h"
#include "config.h"

static const uint64_t FALLBACK_APP_ID = 1553660903986045029ULL;

/* ======================================================
   GAME STATES - all reachable
   ====================================================== */
enum class State {
    MAIN_MENU, FREE_ROAM, DELIVERY_ACTIVE, DELIVERY_COMPLETE, PAUSED,
    ON_FERRY, ON_TRAIN, RESTING, GOT_FINE, TOLLGATE,
    CARGO_DAMAGED, TRUCK_DAMAGED, SPEEDING,
};

static const char* state_key(State s) {
    switch (s) {
    case State::MAIN_MENU:         return "main_menu";
    case State::FREE_ROAM:         return "free_roam";
    case State::DELIVERY_ACTIVE:   return "delivery_active";
    case State::DELIVERY_COMPLETE: return "delivery_complete";
    case State::PAUSED:            return "paused";
    case State::ON_FERRY:          return "on_ferry";
    case State::ON_TRAIN:          return "on_train";
    case State::RESTING:           return "resting";
    case State::GOT_FINE:          return "got_fine";
    case State::TOLLGATE:          return "tollgate";
    case State::CARGO_DAMAGED:     return "cargo_damaged";
    case State::TRUCK_DAMAGED:     return "truck_damaged";
    case State::SPEEDING:          return "speeding";
    }
    return "free_roam";
}

/* ======================================================
   TELEMETRY SNAPSHOT
   ====================================================== */
struct Data {
    /* job: filled from the configuration event (real SDK has no
     * job.city/mass channels; that data arrives as config attributes) */
    char    cargo_name[64]          = {};
    float   cargo_mass_kg           = 0.f;
    char    dest_city[64]           = {};
    char    dest_company[64]        = {};
    char    src_city[64]            = {};
    char    src_company[64]         = {};
    float   planned_distance_km     = 0.f;
    int64_t income                  = 0;

    /* truck config */
    char    truck_brand[48]         = {};
    char    truck_model[64]         = {};
    float   fuel_capacity_l         = 0.f;

    /* per-frame channels */
    float   nav_distance_m          = 0.f;
    float   nav_time_min            = 0.f;
    float   speed_kmh               = 0.f;
    float   speed_limit_kmh         = 0.f;   /* 0 = unknown */
    float   fuel_l                  = 0.f;
    float   wear_chassis            = 0.f;
    float   cargo_damage            = 0.f;

    bool    engine_on               = false;
    bool    job_active              = false;
    bool    on_ferry                = false;
    bool    on_train                = false;
    float   fine_amount             = 0.f;
    int64_t session_start           = 0;
} g;

static int64_t g_fine_until      = 0;
static int64_t g_tollgate_until  = 0;
static int64_t g_delivered_until = 0;
static int64_t g_delivered_income = 0;

static State     g_state     = State::MAIN_MENU;
static bool      g_paused    = false;
static scs_log_t g_log       = nullptr;
static int64_t   g_last_push = 0;
static int       g_seh_faults = 0;
static bool      g_logged_cfg = false;
static bool      g_logged_truck = false;

/* ======================================================
   BASICS
   ====================================================== */
static void log_msg(const char* msg) {
    if (g_log) g_log(SCS_LOG_TYPE_message, msg);
}

/* Bridge discord_ipc diagnostics (READY/ERROR frames, pipe loss) into
 * the game log so handshake problems become visible in game.log.txt. */
static void discord_log(const char* msg) {
    std::string line("[ETS2rpcMKII] ");
    line += msg ? msg : "";
    log_msg(line.c_str());
}

/* Discord field caps: text 128 bytes (UTF-8 safe), asset keys 32, urls 512 */
static void clamp_utf8(std::string& s, size_t cap) {
    if (s.size() <= cap) return;
    s.resize(cap);
    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
    if (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0xC0) s.pop_back();
}
static std::string clamped(std::string s, size_t cap) { clamp_utf8(s, cap); return s; }

/* - brands -------------------------------------------------
 * The 7 officially licensed ETS2 brands only. ATS brands (ford,
 * mack, kenworth, peterbilt) and everything else, including mods,
 * resolve to the generic art. */
static const struct { const char* id; const char* asset; } BUILTINS[] = {
    { "scania",        "scania"         },
    { "volvo",         "volvo"          },
    { "daf",           "daf"            },
    { "man",           "man"            },
    { "mercedes",      "mercedes"       },
    { "actros",        "mercedes"       },
    { "renault",       "renault"        },
    { "iveco",         "iveco"          },
};

/* Brand badge resolution, strictest first:
 *   1. own application only: [brands] mappings whose value is a VALID
 *      asset key (invalid ones are skipped, never sent)
 *   2. the built-in 7 licensed ETS2 brands
 *   3. fallback: "generic"
 * On the shared repo application step 1 is skipped entirely: the art
 * lives on the owner's application and cannot be extended by users. */
static std::string brand_asset(const char* brand) {
    std::string b(brand);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    if (cfg::custom_art_allowed()) {
        for (const auto& kv : cfg::custom_brands()) {
            if (kv.first.empty() || !cfg::valid_asset_key(kv.second)) continue;
            if (b.find(kv.first) != std::string::npos) return kv.second;
        }
    }
    for (const auto& e : BUILTINS)
        if (b.find(e.id) != std::string::npos) return e.asset;
    return "generic";
}

/* - countries (built-in city hints for the {country} tokens) - */
struct CityCc { const char* city; const char* cc; };
static const CityCc CITIES[] = {
    /* germany */ { "berlin","de" },{ "hamburg","de" },{ "munchen","de" },{ "munich","de" },
    { "koln","de" },{ "cologne","de" },{ "frankfurt","de" },{ "dresden","de" },{ "leipzig","de" },
    { "dortmund","de" },{ "dusseldorf","de" },{ "nurnberg","de" },
    { "stuttgart","de" },{ "hannover","de" },{ "bremen","de" },{ "mannheim","de" },
    { "karlsruhe","de" },{ "kiel","de" },{ "erfurt","de" },{ "rostock","de" },{ "augsburg","de" },
    { "magdeburg","de" },{ "kassel","de" },{ "saarbrucken","de" },{ "duisburg","de" },
    { "essen","de" },{ "wiesbaden","de" },{ "mainz","de" },{ "ulm","de" },
    /* france */ { "paris","fr" },{ "calais","fr" },{ "lyon","fr" },{ "marseille","fr" },
    { "bordeaux","fr" },{ "toulouse","fr" },{ "lille","fr" },{ "nantes","fr" },{ "strasbourg","fr" },
    { "rennes","fr" },{ "dijon","fr" },{ "metz","fr" },{ "reims","fr" },
    /* uk */ { "london","gb" },{ "dover","gb" },{ "manchester","gb" },{ "birmingham","gb" },
    { "leeds","gb" },{ "sheffield","gb" },{ "liverpool","gb" },{ "bristol","gb" },
    { "newcastle","gb" },{ "hull","gb" },{ "edinburgh","gb" },{ "glasgow","gb" },{ "cardiff","gb" },
    /* benelux */ { "amsterdam","nl" },{ "rotterdam","nl" },{ "utrecht","nl" },{ "groningen","nl" },
    { "zwolle","nl" },{ "brussels","be" },{ "bruxelles","be" },{ "antwerp","be" },{ "antwerpen","be" },
    { "gent","be" },{ "liege","be" },{ "brugge","be" },{ "luxembourg","lu" },
    /* alps & iberia */ { "bern","ch" },{ "zurich","ch" },{ "geneva","ch" },{ "basel","ch" },
    { "innsbruck","at" },{ "wien","at" },{ "vienna","at" },{ "graz","at" },{ "linz","at" },
    { "salzburg","at" },{ "klagenfurt","at" },{ "madrid","es" },{ "barcelona","es" },
    { "valencia","es" },{ "sevilla","es" },{ "bilbao","es" },{ "zaragoza","es" },
    { "lisboa","pt" },{ "lisbon","pt" },{ "porto","pt" },
    /* italy */ { "roma","it" },{ "rome","it" },{ "milano","it" },{ "milan","it" },
    { "torino","it" },{ "genova","it" },{ "venezia","it" },{ "bologna","it" },{ "napoli","it" },
    { "palermo","it" },{ "messina","it" },{ "catanzaro","it" },{ "ancona","it" },{ "bari","it" },
    /* north & east */ { "kobenhavn","dk" },{ "copenhagen","dk" },{ "odense","dk" },{ "aalborg","dk" },
    { "stockholm","se" },{ "goteborg","se" },{ "jonkoping","se" },{ "linkoping","se" },
    { "oslo","no" },{ "kristiansand","no" },{ "stavanger","no" },{ "bergen","no" },
    { "helsinki","fi" },{ "tampere","fi" },{ "turku","fi" },{ "oulu","fi" },
    { "kajaani","fi" },{ "joensuu","fi" },{ "kuopio","fi" },{ "vaasa","fi" },
    { "rovaniemi","fi" },{ "jyvaskyla","fi" },{ "mikkeli","fi" },{ "kotka","fi" },
    { "pori","fi" },{ "lahti","fi" },{ "lappeeranta","fi" },
    { "praha","cz" },{ "prague","cz" },{ "brno","cz" },{ "plzen","cz" },{ "ostrava","cz" },
    { "warszawa","pl" },{ "warsaw","pl" },{ "krakow","pl" },{ "lodz","pl" },{ "poznan","pl" },
    { "szczecin","pl" },{ "gdansk","pl" },{ "katowice","pl" },{ "lublin","pl" },{ "bialystok","pl" },
    { "bratislava","sk" },{ "kosice","sk" },{ "budapest","hu" },{ "pecs","hu" },{ "szeged","hu" },
    { "debrecen","hu" },{ "ljubljana","si" },{ "maribor","si" },{ "koper","si" },
};

struct CcName { const char* cc; const char* name; };
static const CcName CC_NAMES[] = {
    { "de","Germany" },{ "fr","France" },{ "gb","United Kingdom" },{ "nl","Netherlands" },
    { "be","Belgium" },{ "lu","Luxembourg" },{ "ch","Switzerland" },{ "at","Austria" },
    { "es","Spain" },{ "pt","Portugal" },{ "it","Italy" },{ "dk","Denmark" },
    { "se","Sweden" },{ "no","Norway" },{ "fi","Finland" },{ "cz","Czechia" },
    { "pl","Poland" },{ "sk","Slovakia" },{ "hu","Hungary" },{ "si","Slovenia" },
};

static std::string norm_city(const char* city) {
    std::string s(city);
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

/* City matching helper: does either known city contain the fragment? */
static bool city_has(const std::string& fragment) {
    if (fragment.empty()) return false;
    std::string dest = norm_city(g.dest_city);
    std::string src  = norm_city(g.src_city);
    return (!dest.empty() && dest.find(fragment) != std::string::npos) ||
           (!src.empty()  && src.find(fragment)  != std::string::npos);
}

/* Country for the TEXT tokens ({country}, {country_code}, emoji): a
 * [countries] value that is exactly two letters acts as the country
 * code, otherwise the built-in city table decides. */
static std::string country_text_cc() {
    for (const auto& kv : cfg::countries()) {
        const std::string& v = kv.second;
        if (v.size() == 2 &&
            ((v[0] >= 'a' && v[0] <= 'z') || (v[0] >= '0' && v[0] <= '9')) &&
            (v[1] >= 'a' && v[1] <= 'z') && city_has(kv.first))
            return v;
    }
    for (const auto& e : CITIES)
        if (city_has(e.city)) return e.cc;
    return "";
}

/* Flag ART for the badge, own applications only: the first
 * [countries] entry whose city matches AND whose value is a valid
 * asset key. Invalid values are never sent; the caller falls back
 * to the generic_c country art. */
static std::string country_flag_asset() {
    if (!cfg::custom_art_allowed()) return "";
    for (const auto& kv : cfg::countries()) {
        if (city_has(kv.first) && cfg::valid_asset_key(kv.second))
            return kv.second;
    }
    return "";
}

static std::string country_name(const std::string& cc) {
    for (const auto& e : CC_NAMES)
        if (cc == e.cc) return e.name;
    return cc;
}

/* regional indicator emoji for the {country_emoji} token, e.g. "de" -> flag emoji */
static std::string country_emoji(const std::string& cc) {
    if (cc.size() != 2) return "";
    std::string out;
    for (char c : cc) {
        unsigned char u = (unsigned char)std::toupper(c);
        if (u < 'A' || u > 'Z') return "";
        out += (char)0xF0; out += (char)0x9F; out += (char)0x87;
        out += (char)(0x80 + (u - 'A'));   /* U+1F1E6 + index */
    }
    return out;
}

/* - token engine ------------------------------------------- */
using Tokens = std::map<std::string, std::string>;

static std::string eval(const std::string& t, const Tokens& tok) {
    std::string out; out.reserve(t.size() + 32);
    size_t i = 0;
    while (i < t.size()) {
        if (t[i] == '{') {
            size_t e = t.find('}', i);
            if (e != std::string::npos) {
                auto it = tok.find(t.substr(i + 1, e - i - 1));
                if (it != tok.end()) out += it->second;   /* unknown tokens vanish */
                i = e + 1;
                continue;
            }
        }
        out += t[i++];
    }
    return out;
}

static bool is_na(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    return v == "n/a" || v == "na" || v == "none" || v == "-";
}

/* - formatters ---------------------------------------------- */
static std::string fmt_mass(float kg, bool imperial) {
    char buf[40];
    if (imperial)          snprintf(buf, sizeof(buf), "%.1f t", kg / 907.185f);
    else if (kg >= 1000.f) snprintf(buf, sizeof(buf), "%.0f t", kg / 1000.f);
    else                   snprintf(buf, sizeof(buf), "%.0f kg", kg);
    return buf;
}
static std::string fmt_distance(float metres, bool imperial, const char** unit) {
    char buf[40];
    if (imperial) { *unit = "mi"; float v = metres / 1609.344f;
                    snprintf(buf, sizeof(buf), v >= 10.f ? "%.0f" : "%.1f", v); }
    else          { *unit = "km"; float v = metres / 1000.f;
                    snprintf(buf, sizeof(buf), v >= 10.f ? "%.0f" : "%.1f", v); }
    return buf;
}
static std::string fmt_speed(float kmh, bool imperial, const char** unit) {
    char buf[32];
    if (imperial) { *unit = "mph";  snprintf(buf, sizeof(buf), "%.0f", kmh / 1.609344f); }
    else          { *unit = "km/h"; snprintf(buf, sizeof(buf), "%.0f", kmh); }
    return buf;
}
static std::string fmt_clock(bool h24) {
    time_t now = time(nullptr);
    struct tm lt; localtime_s(&lt, &now);
    char buf[16];
    if (h24) snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    else { int h = lt.tm_hour % 12; if (!h) h = 12;
           snprintf(buf, sizeof(buf), "%d:%02d %s", h, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM"); }
    return buf;
}
static std::string fmt_money(int64_t v) {
    char buf[48]; snprintf(buf, sizeof(buf), "%lld", (long long)v);
    std::string s = buf, out; int n = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i) {
        out += s[i];
        if (++n % 3 == 0 && i > 0) out += ',';
    }
    std::reverse(out.begin(), out.end());
    return out;
}

/* ======================================================
   STATE RESOLVER
   ====================================================== */
static State resolve() {
    const int64_t now = (int64_t)time(nullptr);

    if (now < g_delivered_until)                 return State::DELIVERY_COMPLETE;
    if (now < g_tollgate_until && !g_paused)     return State::TOLLGATE;
    if (now < g_fine_until)                      return State::GOT_FINE;

    if (g_paused)                                return State::PAUSED;
    if (g.on_ferry)                              return State::ON_FERRY;
    if (g.on_train)                              return State::ON_TRAIN;

    if (g.engine_on && g.speed_limit_kmh > 0.f) {
        float limit = g.speed_limit_kmh * (1.f + cfg::speed_limit_pct() / 100.f);
        if (g.speed_kmh > limit)                 return State::SPEEDING;
    }

    if (g.cargo_damage * 100.f >= cfg::cargo_damage_pct() && g.job_active)
                                                 return State::CARGO_DAMAGED;
    if (g.wear_chassis * 100.f >= cfg::chassis_wear_pct() && g.engine_on)
                                                 return State::TRUCK_DAMAGED;

    if (g.job_active)  return g.engine_on ? State::DELIVERY_ACTIVE : State::RESTING;
    if (!g.engine_on)  return State::MAIN_MENU;
    return State::FREE_ROAM;
}

/* ======================================================
   PRESENCE ASSEMBLY
   ====================================================== */
struct RawPresence {
    std::string state, details;
    std::string large_image, large_text;
    std::string small_image, small_text;
    std::string btn1_label, btn1_url, btn2_label, btn2_url;
};

/* merge one handcrafted layer over the current fields; "n/a" clears */
static void apply_layer(RawPresence& r, const cfg::FieldTemplates& t) {
    auto pick = [](const std::string& tmpl, const std::string& cur) -> std::string {
        if (tmpl.empty())    return cur;      /* unset -> keep */
        if (is_na(tmpl))     return "";       /* n/a   -> clear */
        return tmpl;                          /* handcrafted */
    };
    r.state       = pick(t.state,       r.state);
    r.details     = pick(t.details,     r.details);
    r.large_image = pick(t.large_image, r.large_image);
    r.large_text  = pick(t.large_text,  r.large_text);
    r.small_image = pick(t.small_image, r.small_image);
    r.small_text  = pick(t.small_text,  r.small_text);
    r.btn1_label  = pick(t.btn1_label,  r.btn1_label);
    r.btn1_url    = pick(t.btn1_url,    r.btn1_url);
    r.btn2_label  = pick(t.btn2_label,  r.btn2_label);
    r.btn2_url    = pick(t.btn2_url,    r.btn2_url);
}

static void build_presence() {
    const bool imp = cfg::imperial();
    g_state = resolve();

    /* - tokens ----------------------------------------- */
    Tokens tok;
    const char* dist_unit = "km";
    const char* spd_unit  = "km/h";
    float dist_m = g.nav_distance_m > 0.f ? g.nav_distance_m
                 : g.planned_distance_km * 1000.f;

    /* country tokens always resolve so {city}, {country} and
     * {country_emoji} work in every template. The badge slot itself is
     * brand-only: flags would need per-country art we cannot assume. */
    std::string cc = country_text_cc();

    tok["cargo"]          = g.cargo_name;
    tok["mass"]           = fmt_mass(g.cargo_mass_kg, imp);
    tok["dest"]           = g.dest_city;
    tok["src"]            = g.src_city;
    tok["company"]        = g.dest_company[0] ? g.dest_company : g.src_company;
    tok["distance"]       = fmt_distance(dist_m, imp, &dist_unit);
    tok["distance_remaining"] = tok["distance"];
    tok["distance_unit"]  = dist_unit;
    tok["speed"]          = fmt_speed(g.speed_kmh, imp, &spd_unit);
    tok["speed_unit"]     = spd_unit;
    tok["speed_limit"]    = g.speed_limit_kmh > 0.f
                          ? fmt_speed(g.speed_limit_kmh, imp, &spd_unit) + std::string(" ") + spd_unit : "";
    tok["over_limit"]     = (g.speed_limit_kmh > 0.f && g.speed_kmh > g.speed_limit_kmh)
                          ? std::to_string((int)std::lround((g.speed_kmh / g.speed_limit_kmh - 1.f) * 100.f)) : "";
    tok["fuel"]           = std::to_string((int)std::lround(
                            (g.fuel_capacity_l > 0.f ? g.fuel_l / g.fuel_capacity_l : 0.f) * 100.f));
    tok["damage"]         = std::to_string((int)std::lround(g.cargo_damage * 100.f));
    tok["fine"]           = g.fine_amount > 0.f ? fmt_money((int64_t)g.fine_amount) : "";
    tok["brand"]          = g.truck_brand;
    tok["model"]          = g.truck_model;
    tok["truck"]          = std::string(g.truck_brand) + " " + g.truck_model;
    tok["brand_asset"]    = brand_asset(g.truck_brand);
    tok["time"]           = fmt_clock(cfg::use_24h());
    tok["eta"]            = g.nav_time_min > 0.f
                          ? std::to_string((int)std::lround(g.nav_time_min)) : "";
    tok["income"]         = (g.income > 0 ? g.income : g_delivered_income) > 0
                          ? fmt_money(g.income > 0 ? g.income : g_delivered_income) : "";
    tok["country"]        = cc.empty() ? "" : country_name(cc);
    std::string ccu = cc;   /* uppercase for display, e.g. (DE), (RU) */
    for (auto& c : ccu) c = (char)std::toupper((unsigned char)c);
    tok["country_code"]   = cc.empty() ? "" : ccu;
    /* ready-to-append tag: " (FI)" when known, empty (never bare parens)
     * when the city could not be matched */
    tok["country_tag"]    = cc.empty() ? "" : " (" + ccu + ")";
    tok["country_flag"]   = cc.empty() ? "" : "flag_" + cc;
    tok["country_emoji"]  = cc.empty() ? "" : country_emoji(cc);
    tok["city"]           = g.dest_city[0] ? g.dest_city : g.src_city;
    tok["game_version"]   = "";
    tok["newline"]        = "\n";

    /* - smart defaults per state ----------------------- */
    RawPresence r;
    float dist = dist_m;
    switch (g_state) {
    case State::MAIN_MENU:
        r.state = "Planning the next haul";
        r.details = "In main menu";
        break;

    case State::FREE_ROAM:
        r.state = "Exploring Europe";
        r.details = cfg::show_speed()
            ? "Driving a {truck} - {speed} {speed_unit}"
            : "Driving a {truck}";
        break;

    case State::DELIVERY_ACTIVE:
        r.details = "Delivering {cargo} ({mass})";
        if (cfg::show_fuel()) r.details += " - {fuel}% fuel";
        r.state = dist > 1000.f
            ? "En route to {dest} - {distance} {distance_unit} left, ETA {eta} min"
            : "Arriving at {dest}";
        break;

    case State::DELIVERY_COMPLETE:
        r.state = "Delivered to {dest}";
        r.details = g_delivered_income > 0 ? "Job complete - earned {income}" : "Job complete";
        break;

    case State::PAUSED:
        r.state = g.job_active ? "Mid-delivery to {dest}" : "Game paused";
        r.details = "Game paused";
        break;

    case State::ON_FERRY:
        r.state = "Crossing to {dest}";
        r.details = "On a ferry";
        break;

    case State::ON_TRAIN:
        r.state = "Rail freight to {dest}";
        r.details = "On a train";
        break;

    case State::RESTING:
        r.state = "Taking a break near {dest}";
        r.details = "Resting - engine off";
        break;

    case State::GOT_FINE:
        r.state = g.fine_amount > 0.f
            ? "Fined {fine} - watch the road"
            : "Fined - watch the road";
        r.details = "Traffic fine received";
        break;

    case State::TOLLGATE:
        r.state = "Paying the toll";
        r.details = "Passing a tollgate";
        break;

    case State::CARGO_DAMAGED:
        r.state = "En route to {dest}";
        r.details = "Cargo damaged - {damage}%";
        break;

    case State::TRUCK_DAMAGED:
        r.state = "Find a garage";
        r.details = "Truck needs repairs";
        break;

    case State::SPEEDING:
        r.state = "Speeding - {speed} {speed_unit} in a {speed_limit} zone";
        r.details = "Running {over_limit}% over the limit";
        break;
    }

    /* - handcrafted layers: [template] then [template.<state>] -
     * Text and buttons only; image fields are forced further down. */
    apply_layer(r, cfg::global_template());
    apply_layer(r, cfg::state_template(state_key(g_state)));

    /* - image assembly with the full failsafe chain ----
     * Every key that leaves the DLL is verified against the portal
     * asset list; an unknown or invalid key is never sent, so a bad
     * ini can never ghost a mystery text line onto the profile.
     *
     *   big image    : ets2            (required core art)
     *   truck badge  : brand art, or generic for unknown/modded
     *   country badge: user flag art (own app only), or generic_c
     *   nothing left : fields stay empty and are simply not sent
     *
     * ets2 / generic / generic_c are the CORE keys: if they are
     * missing from the portal, those images are dropped (text and
     * buttons keep working). On the shared repo application only the
     * owner's art exists, so custom mappings are not consulted. */
    const bool own_app = cfg::custom_art_allowed();
    (void)own_app;   /* read via cfg:: helpers inside brand_asset etc. */

    r.large_image = "ets2";
    r.large_text  = "Euro Truck Simulator 2";

    if (cfg::show_truck_badge()) {
        r.small_image = tok["brand_asset"];
        r.small_text  = "{truck} ・ {speed} {speed_unit}";
    }

    /* country badge on own apps: user flag art wins over the brand
     * logo whenever the location is known and mapped */
    const std::string flag_asset = country_flag_asset();
    if (!flag_asset.empty() && !cc.empty()) {
        r.small_image = flag_asset;
        r.small_text  = "{country}";
    } else if (!cc.empty() && own_app && cfg::show_truck_badge()) {
        /* own app, location known, no flag art mapped: generic_c */
        r.small_image = "generic_c";
        r.small_text  = "{country}";
    }

    /* Discord quirk: when an asset key has no art uploaded, Discord
     * renders the small_image tooltip as a plain text line - the
     * ghost "third description". A tooltip identical to the raw
     * asset key is always that leak, never intentional, so drop it. */
    if (!r.small_image.empty() && !r.small_text.empty() &&
        r.small_text == r.small_image)
        r.small_text.clear();

    /* - fill tokens, clamp to Discord limits ----------- */
    auto fill = [&](std::string s) { return clamped(eval(s, tok), 128); };
    discord_ipc::Presence p;
    p.state       = fill(r.state);
    p.details     = fill(r.details);
    p.large_image = clamped(eval(r.large_image, tok), 32);
    p.large_text  = fill(r.large_text);
    p.small_image = clamped(eval(r.small_image, tok), 32);
    p.small_text  = fill(r.small_text);
    p.btn1_label  = fill(r.btn1_label);
    p.btn1_url    = clamped(eval(r.btn1_url, tok), 512);
    p.btn2_label  = fill(r.btn2_label);
    p.btn2_url    = clamped(eval(r.btn2_url, tok), 512);
    p.start_unix  = cfg::show_time() ? g.session_start : 0;

    discord_ipc::set(p);
}

static void push() {
    if (!discord_ipc::connected()) return;
    int64_t now = (int64_t)time(nullptr);
    if (now - g_last_push < cfg::update_interval()) return;
    g_last_push = now;
    build_presence();
}

/* ======================================================
   SCS CALLBACKS (SEH-guarded wrappers below)
   ====================================================== */
static void rdstr(const scs_value_t* v, char* dst, size_t n) {
    if (v && v->type == SCS_VALUE_TYPE_string && v->value_string.value)
        strncpy(dst, v->value_string.value, n - 1);
}
static void rdflt(const scs_value_t* v, float& dst) {
    if (v && v->type == SCS_VALUE_TYPE_float) dst = v->value_float.value;
}
static void rdblm(const scs_value_t* v, bool& dst) {
    if (v && v->type == SCS_VALUE_TYPE_bool) dst = (v->value_bool.value != 0);
}
static void rds64(const scs_value_t* v, int64_t& dst) {
    if (v && v->type == SCS_VALUE_TYPE_s64) dst = v->value_s64.value;
}

#define CB(fn) static SCSAPI_VOID fn(const scs_string_t, const scs_u32_t, \
                                      const scs_value_t* v, const scs_context_t)

CB(cb_speed)     { if (v && v->type==SCS_VALUE_TYPE_float)
                       g.speed_kmh = v->value_float.value * 3.6f;              }
CB(cb_speedlim)  { if (v && v->type==SCS_VALUE_TYPE_float)
                       g.speed_limit_kmh = v->value_float.value * 3.6f;        }
CB(cb_fuel)      { rdflt(v, g.fuel_l);                                        }
CB(cb_w_chassis) { rdflt(v, g.wear_chassis);                                  }
CB(cb_engine)    { rdblm(v, g.engine_on);                                     }
CB(cb_navdist)   { rdflt(v, g.nav_distance_m);                                }
CB(cb_navtime)   { rdflt(v, g.nav_time_min);                                  }
CB(cb_cargodmg)  { rdflt(v, g.cargo_damage);                                  }
#undef CB

/* configuration event: carries job + truck identity in the real SDK */
static void config_impl(const scs_telemetry_configuration_t* info) {
    if (!info || !info->attributes) return;

    bool saw_job_dest = false;
    for (const scs_named_value_t* a = info->attributes; a->name; ++a) {
        const char* n = a->name;
        const scs_value_t* v = &a->value;

        if      (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_cargo))             rdstr(v, g.cargo_name,   sizeof(g.cargo_name));
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_cargo_mass))        rdflt(v, g.cargo_mass_kg);
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city))  { rdstr(v, g.dest_city,  sizeof(g.dest_city)); saw_job_dest = true; }
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_company)) rdstr(v, g.dest_company, sizeof(g.dest_company));
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_city))       rdstr(v, g.src_city,     sizeof(g.src_city));
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_company))    rdstr(v, g.src_company,  sizeof(g.src_company));
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_planned_distance_km)) rdflt(v, g.planned_distance_km);
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_income))            rds64(v, g.income);
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand_id))          rdstr(v, g.truck_brand,  sizeof(g.truck_brand));
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_name))              rdstr(v, g.truck_model,  sizeof(g.truck_model));
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_fuel_capacity))     rdflt(v, g.fuel_capacity_l);
    }

    if (saw_job_dest && !g.job_active) {
        g.job_active = true;   /* a job config appeared: delivery is on */
        if (!g_logged_cfg) { g_logged_cfg = true; log_msg("[ETS2rpcMKII] Job data received from configuration."); }
    }

    /* one-time diagnostic: exactly which badge asset the DLL resolves
     * for the current truck, so a missing corner logo can be checked
     * against the portal asset list */
    if (!g_logged_truck && g.truck_brand[0]) {
        g_logged_truck = true;
        std::string line = "[ETS2rpcMKII] Truck: ";
        line += g.truck_brand;
        line += " ";
        line += g.truck_model;
        line += ", corner badge asset: ";
        line += brand_asset(g.truck_brand);
        log_msg(line.c_str());
    }
    else if (!saw_job_dest && g.dest_city[0] == '\0') {
        g.job_active = false;
    }
}

/* gameplay event: deliveries, fines, tolls, ferry/train */
static void gameplay_impl(const scs_telemetry_gameplay_event_t* gev) {
    if (!gev || !gev->id) return;

    const char* id = gev->id;
    const int64_t now  = (int64_t)time(nullptr);
    const int64_t hold = cfg::event_hold();

    if (!strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_job_delivered)) {
        g.job_active = false;
        g.income = 0;
        g_delivered_income = 0;
        g.dest_city[0] = '\0';
        g.src_city[0] = '\0';
        g.cargo_name[0] = '\0';
        g.planned_distance_km = 0.f;
        /* the delivery pay arrives as the "revenue" attribute */
        if (gev->attributes) {
            for (const scs_named_value_t* a = gev->attributes; a->name; ++a) {
                if (!strcmp(a->name, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_revenue) &&
                    a->value.type == SCS_VALUE_TYPE_s64) {
                    g_delivered_income = a->value.value_s64.value;
                    break;
                }
            }
        }
        g_delivered_until = now + hold;
    }
    else if (!strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_job_cancelled)) {
        g.job_active = false;
        g.income = 0;
        g.dest_city[0] = '\0';
        g.src_city[0] = '\0';
        g.cargo_name[0] = '\0';
        g.planned_distance_km = 0.f;
    }
    else if (!strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_fined)) {
        g_fine_until = now + hold;
        g.fine_amount = 0.f;
        if (gev->attributes) {
            for (const scs_named_value_t* a = gev->attributes; a->name; ++a) {
                if (!strcmp(a->name, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_fine_amount)) {
                    if (a->value.type == SCS_VALUE_TYPE_s64)
                        g.fine_amount = (float)a->value.value_s64.value;
                    else if (a->value.type == SCS_VALUE_TYPE_s32)
                        g.fine_amount = (float)a->value.value_s32.value;
                    break;
                }
            }
        }
    }
    else if (!strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_tollgate_paid)) {
        g_tollgate_until = now + 10;
    }
    else if (!strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_use_ferry)) {
        g.on_ferry = true;  g.on_train = false;
    }
    else if (!strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_use_train)) {
        g.on_train = true;  g.on_ferry = false;
    }
}

static void event_impl(const scs_event_t ev, const void* info, const scs_context_t) {
    switch (ev) {
    case SCS_TELEMETRY_EVENT_paused:  g_paused = true;  break;
    case SCS_TELEMETRY_EVENT_started: g_paused = false; break;

    case SCS_TELEMETRY_EVENT_configuration:
        if (info) config_impl(static_cast<const scs_telemetry_configuration_t*>(info));
        break;

    case SCS_TELEMETRY_EVENT_gameplay:
        if (info) gameplay_impl(static_cast<const scs_telemetry_gameplay_event_t*>(info));
        break;

    default: break;
    }
}

static void frame_impl() {
    cfg::maybe_reload();

    const bool was = discord_ipc::connected();
    discord_ipc::pump(cfg::app_id() ? cfg::app_id() : FALLBACK_APP_ID);
    if (!was && discord_ipc::connected())
        log_msg("[ETS2rpcMKII] Discord pipe (re)connected.");

    push();
}

/* SEH wrappers: swallow anything so the game thread survives.
 * MSVC only, MinGW builds call the impls directly. */
static void seh_fault(const char* where) {
    ++g_seh_faults;
    if (g_seh_faults <= 3) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[ETS2rpcMKII] recovered from internal fault in %s", where);
        log_msg(buf);
    }
}

static SCSAPI_VOID cb_event(const scs_event_t ev, const void* info, const scs_context_t ctx) {
#ifdef _MSC_VER
    __try { event_impl(ev, info, ctx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { seh_fault("event"); }
#else
    event_impl(ev, info, ctx);
#endif
}

static SCSAPI_VOID cb_frame(const scs_event_t ev, const void* info, const scs_context_t ctx) {
#ifdef _MSC_VER
    __try { frame_impl(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { seh_fault("frame"); }
#else
    frame_impl();
#endif
}

/* (channel callbacks are trivial field copies, no SEH needed there) */

/* ======================================================
   ENTRY POINTS
   ====================================================== */
static std::string dll_directory() {
    char path[MAX_PATH] = {};
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&dll_directory, &mod))
        return ".";
    DWORD n = GetModuleFileNameA(mod, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string dir(path, n);
    size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? "." : dir.substr(0, slash);
}

static int g_reg_fails = 0;

static void reg_ch(const scs_telemetry_init_params_v100_t* p,
                   const char* ch, scs_value_type_t type,
                   scs_telemetry_channel_callback_t cb) {
    if (!p) return;
    scs_result_t r = p->register_for_channel(ch, SCS_U32_NIL, type,
                                             SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
                                             cb, nullptr);
    if (r != SCS_RESULT_ok) {
        ++g_reg_fails;
        if (g_reg_fails <= 3) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "[ETS2rpcMKII] channel %s not registered (result %u), "
                     "some data may be missing.", ch, (unsigned)r);
            log_msg(buf);
        }
    }
}

static SCSAPI_RESULT init_impl(const scs_u32_t version,
                               const scs_telemetry_init_params_t* params) {
    /* log BEFORE anything else so a rejected load is visible in game.log.txt */
    const auto* p = static_cast<const scs_telemetry_init_params_v100_t*>(params);
    g_log = p ? p->common.log : nullptr;
    discord_ipc::set_log_callback(discord_log);

    char banner[128];
    snprintf(banner, sizeof(banner), "[ETS2rpcMKII] v%s initialising (telemetry API %u.%u).",
             ETS2RPCMKII_VERSION, SCS_GET_MAJOR_VERSION(version), SCS_GET_MINOR_VERSION(version));
    log_msg(banner);

    if (SCS_GET_MAJOR_VERSION(version) != 1) {
        snprintf(banner, sizeof(banner),
                 "[ETS2rpcMKII] unsupported telemetry major version %u, plugin disabled.",
                 SCS_GET_MAJOR_VERSION(version));
        log_msg(banner);
        return SCS_RESULT_unsupported;
    }

    g.session_start = (int64_t)time(nullptr);
    cfg::init(dll_directory());

    const uint64_t app = cfg::app_id() ? cfg::app_id() : FALLBACK_APP_ID;
    if (!cfg::app_id())
        log_msg("[ETS2rpcMKII] No application_id in ets2rpcmkii.ini, using built-in fallback.");

    if (discord_ipc::connect(app)) {
        log_msg("[ETS2rpcMKII] Discord pipe opened, handshake sent.");
        log_msg("[ETS2rpcMKII] Tip: others see your buttons - Discord hides them from you.");
    }
    else
        log_msg("[ETS2rpcMKII] Discord not found yet, retrying every 15 s (is the Discord desktop client running?)");

    /* per-frame channels (real SDK names) */
    reg_ch(p, SCS_TELEMETRY_TRUCK_CHANNEL_speed,                  SCS_VALUE_TYPE_float, cb_speed);
    reg_ch(p, SCS_TELEMETRY_TRUCK_CHANNEL_navigation_speed_limit, SCS_VALUE_TYPE_float, cb_speedlim);
    reg_ch(p, SCS_TELEMETRY_TRUCK_CHANNEL_fuel,                   SCS_VALUE_TYPE_float, cb_fuel);
    reg_ch(p, SCS_TELEMETRY_TRUCK_CHANNEL_wear_chassis,           SCS_VALUE_TYPE_float, cb_w_chassis);
    reg_ch(p, SCS_TELEMETRY_TRUCK_CHANNEL_engine_enabled,         SCS_VALUE_TYPE_bool,  cb_engine);
    reg_ch(p, SCS_TELEMETRY_TRUCK_CHANNEL_navigation_distance,    SCS_VALUE_TYPE_float, cb_navdist);
    reg_ch(p, SCS_TELEMETRY_TRUCK_CHANNEL_navigation_time,        SCS_VALUE_TYPE_float, cb_navtime);
    reg_ch(p, SCS_TELEMETRY_JOB_CHANNEL_cargo_damage,             SCS_VALUE_TYPE_float, cb_cargodmg);

    /* events: config carries job + truck identity, gameplay carries
     * deliveries/fines/tolls, frame_start drives the push loop */
    if (p) {
        p->register_for_event(SCS_TELEMETRY_EVENT_configuration, cb_event, nullptr);
        p->register_for_event(SCS_TELEMETRY_EVENT_gameplay,      cb_event, nullptr);
        p->register_for_event(SCS_TELEMETRY_EVENT_paused,        cb_event, nullptr);
        p->register_for_event(SCS_TELEMETRY_EVENT_started,       cb_event, nullptr);
        p->register_for_event(SCS_TELEMETRY_EVENT_frame_start,   cb_frame,  nullptr);
    }

    log_msg("[ETS2rpcMKII] Initialisation complete.");
    return SCS_RESULT_ok;
}

extern "C" SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version,
                                            const scs_telemetry_init_params_t* params) {
#ifdef _MSC_VER
    __try { return init_impl(version, params); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return SCS_RESULT_unsupported; }
#else
    return init_impl(version, params);
#endif
}

static void shutdown_impl() {
    discord_ipc::clear();
    discord_ipc::disconnect();
    log_msg("[ETS2rpcMKII] Shutdown.");
}

extern "C" SCSAPI_VOID scs_telemetry_shutdown() {
#ifdef _MSC_VER
    __try { shutdown_impl(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
#else
    shutdown_impl();
#endif
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
