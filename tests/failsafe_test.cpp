/*
 * failsafe_test.cpp - standalone verification of the ini failsafe.
 *
 * Builds as a normal console exe (no SCS SDK involved) and exercises
 * the exact cfg:: code paths the DLL uses at runtime:
 *
 *   1. no ini on disk          -> baked-in defaults come up, file regenerated
 *   2. regenerated file        -> contains the documented default set
 *   3. valid ini on disk       -> its values win, no fallback logged
 *   4. garbage ini on disk     -> parser survives, every lookup stays safe
 *   5. ini deleted at runtime  -> maybe_reload() falls back, file restored
 *
 * Exit code 0 = all tests passed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "config.h"

static int g_failed = 0;
static void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
    if (!ok) ++g_failed;
}

static bool exists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void write_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

/* capture the plugin-style log lines so we can assert on them */
static std::string g_log;
static void capture_log(const char* msg) { g_log += msg; g_log += "\n"; }

static std::string temp_dir() {
    char base[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, base);
    static int n = 0;
    std::string d = std::string(base) + "ets2rpcmkii_test_"
                  + std::to_string(GetCurrentProcessId())
                  + "_" + std::to_string(++n);
    CreateDirectoryA(d.c_str(), nullptr);
    return d;
}

/* every scenario gets a fresh store: cfg::init re-points the path */
static void fresh(const std::string& dir) {
    g_log.clear();
    cfg::set_log_callback(capture_log);
    cfg::init(dir);
}

int main() {
    std::printf("ETS2rpcMKII ini failsafe test\n");
    std::printf("-----------------------------\n");

    /* 1. missing ini: the baked-in defaults must come up immediately */
    {
        std::string dir = temp_dir();
        const std::string ini = dir + "\\ets2rpcmkii.ini";
        fresh(dir);                       /* no ini written beforehand */

        check(exists(ini),                     "1a. missing ini: default file regenerated on disk");
        check(cfg::app_id() == 1553660903986045029ULL,
                                               "1b. missing ini: baked-in application_id is live");
        check(!cfg::custom_art_allowed(),      "1i. repo app id: custom art mappings locked");
        check(cfg::valid_asset_key("generic") && cfg::valid_asset_key("generic_c") &&
              cfg::valid_asset_key("flag_de") && cfg::valid_asset_key("my-mod_2"),
                                               "1j. valid asset keys accepted");
        check(!cfg::valid_asset_key("") && !cfg::valid_asset_key("Flag DE") &&
              !cfg::valid_asset_key("flag/de") &&
              !cfg::valid_asset_key("012345678901234567890123456789012"),
                                               "1k. invalid asset keys rejected (empty, case, space, slash, 33 chars)");
        check(cfg::update_interval() == 5,     "1c. missing ini: update_interval default active");
        check(cfg::event_hold() == 30,         "1d. missing ini: event_hold default active");
        check(cfg::show_speed(),               "1e. missing ini: presence toggles at defaults");
        check(cfg::any_template_set(),         "1f. missing ini: handcrafted templates parsed");
        check(cfg::state_template("delivery_active").details.find("{mass}") != std::string::npos,
                                               "1g. missing ini: per-state default template loaded");
        check(cfg::state_template("delivery_active").state.find("{src}") != std::string::npos &&
              cfg::state_template("delivery_active").state.find("Close to") == std::string::npos,
                                               "1m. v5.0.3: baked-in delivery line is the honest {src} -> {dest} route");
        check(cfg::state_template("paused").details == "Paused",
                                               "1n. v5.0.3: baked-in paused line carries no ({mass}) that renders as (0 kg)");
        check(g_log.find("not found") != std::string::npos,
                                               "1h. missing ini: fallback logged");
    }

    /* 2. the regenerated file must be the documented default set */
    {
        std::string dir = temp_dir();
        const std::string ini = dir + "\\ets2rpcmkii.ini";
        fresh(dir);

        std::ifstream f(ini, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(text.find("[template.delivery_active]") != std::string::npos,
                                               "2a. regenerated file: per-state sections present");
        check(text.find("Get on Steam") != std::string::npos,
                                               "2b. regenerated file: Get on Steam button kept");
        check(text.find("application_id") != std::string::npos,
                                               "2c. regenerated file: [discord] application_id kept");
        check(text.find("[countries]") != std::string::npos,
                                               "2d. regenerated file: [countries] section kept");
        check(text.find("Close to") == std::string::npos &&
              text.find("{country_tag}") != std::string::npos,
                                               "2e. v5.0.2: no fake 'Close to' line; {country_tag} still used where it is safe");
        check(text.find("{ferry_to}") != std::string::npos &&
              text.find("{ferry_from}") != std::string::npos,
                                               "2f. regenerated file: ferry/train templates use real crossing names");
        check(text.find("({country_code})") == std::string::npos,
                                               "2g. regenerated file: no raw ( country_code ) parens left anywhere");
        check(text.find("{job_progress}") == std::string::npos &&
              text.find("{progress_tag}") == std::string::npos &&
              text.find("{driven_tag}") != std::string::npos,
                                               "2h. v5.0.6: no % done suffix; the session odometer {driven_tag} replaced it");
        check(text.find("{city}") == std::string::npos ||
              text.find("{src}") != std::string::npos,
                                               "2i. v5.0.2: default lines use the honest {src} -> {dest} route, not a fake city");
        check(text.find("[template.paused]\nstate        = {src}{src_tag} \xe2\x86\x92 {dest}{dest_tag}\ndetails      = Paused") != std::string::npos,
                                               "2j. v5.0.2: paused default is route + Paused, no ({mass}) that rendered as (0 kg)");
        check(text.find("{src}{src_tag}") != std::string::npos &&
              text.find("{dest}{dest_tag}") != std::string::npos &&
              text.find("{ferry_to}{ferry_tag}") != std::string::npos,
                                               "2k. v5.0.4: route and rail lines carry per-city ISO tags");
    }

    /* 3. valid ini on disk: its values must win over the defaults */
    {
        std::string dir = temp_dir();
        write_file(dir + "\\ets2rpcmkii.ini",
                   "[discord]\napplication_id = 12345\n"
                   "[behaviour]\nevent_hold = 77\n"
                   "[template]\nstate = HELLO {truck}\n");
        fresh(dir);

        check(cfg::app_id() == 12345,          "3a. existing ini: application_id read from disk");
        check(cfg::custom_art_allowed(),       "3e. own app id: custom art mappings unlocked");
        check(cfg::event_hold() == 77,         "3b. existing ini: event_hold read from disk");
        check(cfg::global_template().state == "HELLO {truck}",
                                               "3c. existing ini: [template] read from disk");
        check(g_log.find("not found") == std::string::npos,
                                               "3d. existing ini: no fallback logged");
    }

    /* 4. garbage ini: the parser must not break, lookups stay safe */
    {
        std::string dir = temp_dir();
        write_file(dir + "\\ets2rpcmkii.ini",
                   "\xEF\xBB\xBF[discord\nthis line has no equals sign\n"
                   "=\n[behaviour\nevent hold = 99\n\xFF\xFE binary \x01\x02\n");
        fresh(dir);

        check(cfg::app_id() == 0,              "4a. garbage ini: no application_id (DLL uses its built-in fallback)");
        check(cfg::event_hold() == 20,         "4b. garbage ini: absent keys fall back to built-in defaults");
        check(cfg::show_speed(),               "4c. garbage ini: presence toggles stay at defaults");
        check(!cfg::any_template_set(),        "4d. garbage ini: no half-parsed template state");
    }

    /* 5. ini deleted while the game runs: fallback must kick in */
    {
        std::string dir = temp_dir();
        const std::string ini = dir + "\\ets2rpcmkii.ini";
        write_file(ini, "[behaviour]\nevent_hold = 99\n");
        fresh(dir);

        check(cfg::event_hold() == 99,         "5a. runtime: custom value loaded before deletion");

        DeleteFileA(ini.c_str());
        cfg::maybe_reload();                   /* file gone: fallback must trigger */

        check(cfg::event_hold() == 30,         "5b. runtime: deleted ini -> baked-in defaults live again");
        check(exists(ini),                     "5c. runtime: default file regenerated after deletion");
        check(g_log.find("disappeared") != std::string::npos,
                                               "5d. runtime: deletion fallback logged");
    }

    /* 6. v5.0 schema report: typos and out-of-range values get named
     *    in the log instead of being silently ignored or clamped */
    {
        std::string dir = temp_dir();
        write_file(dir + "\\ets2rpcmkii.ini",
                   "[behaviour]\nevent_hold = 99\n"
                   "evnt_hold = 40\n"
                   "update_interval = 9999\n"
                   "[template.not_a_state]\nstate = x\n");
        fresh(dir);

        check(cfg::event_hold() == 99,         "6a. schema: typo'd key does not clobber the real one");
        check(g_log.find("unknown key 'evnt_hold'") != std::string::npos,
                                               "6b. schema: unknown key named in the log");
        check(g_log.find("update_interval") != std::string::npos &&
              g_log.find("out of range") != std::string::npos,
                                               "6c. schema: out-of-range value reported");
        check(g_log.find("unknown template state") != std::string::npos,
                                               "6d. schema: unknown template state reported");
        check(cfg::update_interval() == 3600,  "6e. schema: clamped value still applied safely");

        cfg::maybe_reload();
        check(g_log.find("unknown key 'evnt_hold'") != std::string::npos,
                                               "6f. schema: report also runs after a hot reload");
    }

    std::printf("-----------------------------\n%s\n",
                g_failed ? "RESULT: FAIL" : "RESULT: ALL TESTS PASSED");
    return g_failed ? 1 : 0;
}
