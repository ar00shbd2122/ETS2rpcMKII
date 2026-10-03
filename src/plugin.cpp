/*
 * ETS2rpcMKII - Discord Rich Presence - plugin.cpp
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
#include "config.h"
#include "discord_ipc.h"

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
    int64_t income                  = 0;

    /* truck config */
    char    truck_brand[48]         = {};
    char    truck_model[64]         = {};
    float   fuel_capacity_l         = 0.f;

    /* transport events: ferry/train crossings, filled from the
     * gameplay event attributes (source.name / target.name) */
    char    ferry_from[64]          = {};
    char    ferry_to[64]            = {};

    /* v5.0.2: last-job snapshot. The delivered/cancelled events wipe the
     * live job buffers before the delivery_complete state renders, so
     * cargo and cities are remembered here for the completion card. */
    char    last_cargo[64]          = {};
    char    last_dest[64]           = {};
    char    last_src[64]            = {};

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
    float   fine_amount             = 0.f;
    int64_t session_start           = 0;
    /* v5.0.6: session odometer - speed x time, nothing on disk */
    float   driven_m                = 0.f;
    int64_t odom_t0                 = 0;
} g;

static int64_t g_fine_until      = 0;
static int64_t g_tollgate_until  = 0;
static int64_t g_delivered_until = 0;
static int64_t g_delivered_income = 0;
/* whole seconds since the epoch; used by the session odometer */
static int64_t now_s() { return (int64_t)time(nullptr); }

static int64_t g_ferry_until     = 0;
static int64_t g_train_until     = 0;
static uint32_t g_jobs_done      = 0;        /* deliveries this session */

static State     g_state     = State::MAIN_MENU;
static bool      g_paused    = false;
static scs_log_t g_log       = nullptr;
static int64_t   g_last_push = 0;
static int64_t   g_session_t0 = 0;          /* for the shutdown summary */
static int       g_seh_faults = 0;
static int64_t   g_last_fault_log = 0;      /* fault-log cooldown */
static uint64_t  g_push_count = 0;
static bool      g_logged_cfg = false;
static bool      g_logged_truck = false;

/* v5.0: telemetry floats are guarded everywhere they are consumed.
 * A driver crash or a mod writing NaN into a channel must never
 * produce NaN text like 'nan km/h' on the profile. */
static float sane_f(float v) {
    if (v != v)             return 0.f;                 /* NaN            */
    if (v >  3.4e38f / 4.f) return  3.4e38f / 4.f;      /* +inf, huge     */
    if (v < -3.4e38f / 4.f) return -3.4e38f / 4.f;      /* -inf, huge neg */
    return v;
}

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

/* - countries (built-in city hints for the {country} tokens) -
 * Keys are given in the folded alphabet (ASCII, accents stripped,
 * Cyrillic transliterated), so one entry matches every UI language:
 * the Russian client's Выборг folds to vyborg and hits directly. */
struct CityCc { const char* city; const char* cc; };
static const CityCc CITIES[] = {
    /* ordering-sensitive entries first: each of these contains a
     * shorter entry from another country (penza, tartu, pori, olten,
     * nis, ankara, hama, karak), and first match wins */
    { "penzance","gb" },{ "tartus","sy" },{ "zaporizhzhia","ua" },
    { "oltenita","ro" },{ "manisa","tr" },{ "lankaran","az" },
    { "hamadan","ir" },{ "karakol","kg" },
    /* russia (base + rusmap / promods) */
    { "vyborg","ru" },{ "sankt peterburg","ru" },{ "peterburg","ru" },
    { "petersburg","ru" },{ "moskva","ru" },{ "moscow","ru" },{ "kaliningrad","ru" },
    { "baltiysk","ru" },{ "pskov","ru" },{ "velikiye luki","ru" },{ "novgorod","ru" },
    { "tver","ru" },{ "torzhok","ru" },{ "dubna","ru" },{ "smolensk","ru" },
    { "voronezh","ru" },{ "kazan","ru" },{ "samara","ru" },{ "tolyatti","ru" },
    { "volgograd","ru" },{ "saratov","ru" },{ "ulyanovsk","ru" },{ "penza","ru" },
    { "tambov","ru" },{ "lipetsk","ru" },{ "kursk","ru" },{ "belgorod","ru" },
    { "oryol","ru" },{ "bryansk","ru" },{ "ivanovo","ru" },{ "yaroslavl","ru" },
    { "kostroma","ru" },{ "vologda","ru" },{ "cherepovets","ru" },{ "rostov","ru" },
    { "krasnodar","ru" },{ "sochi","ru" },{ "novorossiysk","ru" },{ "taganrog","ru" },
    { "astrakhan","ru" },
    /* belarus */ { "minsk","by" },{ "brest","by" },{ "grodno","by" },{ "gomel","by" },
    { "mogilev","by" },{ "baranovichi","by" },{ "pinsk","by" },{ "orsha","by" },
    /* baltics */ { "tallinn","ee" },{ "tartu","ee" },{ "narva","ee" },{ "parnu","ee" },
    { "riga","lv" },{ "daugavpils","lv" },{ "rezekne","lv" },{ "liepaja","lv" },
    { "ventspils","lv" },{ "kaunas","lt" },{ "vilnius","lt" },{ "klaipeda","lt" },
    { "siauliai","lt" },{ "panevezys","lt" },{ "utena","lt" },{ "daugpils","lv" },
    /* finland */ { "helsinki","fi" },{ "tampere","fi" },{ "turku","fi" },{ "oulu","fi" },
    { "kajaani","fi" },{ "joensuu","fi" },{ "kuopio","fi" },{ "vaasa","fi" },
    { "rovaniemi","fi" },{ "jyvaskyla","fi" },{ "mikkeli","fi" },{ "kotka","fi" },
    { "pori","fi" },{ "lahti","fi" },{ "lappeenranta","fi" },{ "imatra","fi" },
    { "hanko","fi" },{ "savonlinna","fi" },
    /* germany */ { "berlin","de" },{ "hamburg","de" },{ "munchen","de" },{ "munich","de" },
    { "koln","de" },{ "cologne","de" },{ "frankfurt","de" },{ "dresden","de" },{ "leipzig","de" },
    { "dortmund","de" },{ "dusseldorf","de" },{ "nurnberg","de" },{ "nuremberg","de" },
    { "stuttgart","de" },{ "hannover","de" },{ "hanover","de" },{ "bremen","de" },
    { "mannheim","de" },{ "karlsruhe","de" },{ "kiel","de" },{ "erfurt","de" },
    { "rostock","de" },{ "augsburg","de" },{ "magdeburg","de" },{ "kassel","de" },
    { "saarbrucken","de" },{ "duisburg","de" },{ "essen","de" },{ "wiesbaden","de" },
    { "mainz","de" },{ "ulm","de" },{ "aachen","de" },{ "munster","de" },
    { "osnabruck","de" },{ "paderborn","de" },{ "bielefeld","de" },{ "gutersloh","de" },
    { "koblenz","de" },{ "trier","de" },{ "kaiserslautern","de" },{ "freiburg","de" },
    { "konstanz","de" },{ "friedrichshafen","de" },{ "heilbronn","de" },{ "pforzheim","de" },
    { "offenburg","de" },{ "passau","de" },{ "landshut","de" },{ "kempten","de" },
    { "memmingen","de" },{ "ingolstadt","de" },{ "bayreuth","de" },{ "bamberg","de" },
    { "erlangen","de" },{ "wurzburg","de" },{ "regensburg","de" },{ "fulda","de" },
    { "braunschweig","de" },{ "brunswick","de" },{ "chemnitz","de" },{ "cottbus","de" },
    { "wolfsburg","de" },{ "salzgitter","de" },{ "lubeck","de" },{ "schwerin","de" },
    { "wismar","de" },{ "emden","de" },{ "oldenburg","de" },{ "bremerhaven","de" },
    { "stralsund","de" },{ "greifswald","de" },{ "neubrandenburg","de" },
    /* france (base + promods) */ { "paris","fr" },{ "calais","fr" },{ "lyon","fr" },
    { "marseille","fr" },{ "bordeaux","fr" },{ "toulouse","fr" },{ "lille","fr" },
    { "nantes","fr" },{ "strasbourg","fr" },{ "rennes","fr" },{ "dijon","fr" },
    { "metz","fr" },{ "reims","fr" },{ "montpellier","fr" },{ "rouen","fr" },
    { "toulon","fr" },{ "grenoble","fr" },{ "angers","fr" },{ "clermont ferrand","fr" },
    { "le havre","fr" },{ "le mans","fr" },{ "tours","fr" },{ "orleans","fr" },
    { "nimes","fr" },{ "avignon","fr" },{ "cannes","fr" },{ "perpignan","fr" },
    { "biarritz","fr" },{ "bayonne","fr" },{ "caen","fr" },{ "cherbourg","fr" },
    { "mulhouse","fr" },{ "nancy","fr" },{ "troyes","fr" },{ "poitiers","fr" },
    { "limoges","fr" },{ "besancon","fr" },{ "valence","fr" },{ "saint etienne","fr" },
    { "quimper","fr" },{ "la rochelle","fr" },{ "dunkerque","fr" },{ "amiens","fr" },
    { "beauvais","fr" },{ "bourges","fr" },{ "aix en provence","fr" },
    /* uk (base + promods) */ { "london","gb" },{ "dover","gb" },{ "manchester","gb" },
    { "birmingham","gb" },{ "leeds","gb" },{ "sheffield","gb" },{ "liverpool","gb" },
    { "bristol","gb" },{ "newcastle","gb" },{ "hull","gb" },{ "edinburgh","gb" },
    { "glasgow","gb" },{ "cardiff","gb" },{ "aberdeen","gb" },{ "dundee","gb" },
    { "inverness","gb" },{ "perth","gb" },{ "carlisle","gb" },{ "sunderland","gb" },
    { "whitby","gb" },{ "cambridge","gb" },{ "oxford","gb" },{ "norwich","gb" },
    { "ipswich","gb" },{ "felixstowe","gb" },{ "harwich","gb" },{ "colchester","gb" },
    { "chelmsford","gb" },{ "lowestoft","gb" },{ "great yarmouth","gb" },
    { "kings lynn","gb" },{ "peterborough","gb" },{ "milton keynes","gb" },
    { "reading","gb" },{ "guildford","gb" },{ "crawley","gb" },{ "basingstoke","gb" },
    { "southampton","gb" },{ "portsmouth","gb" },{ "brighton","gb" },{ "bournemouth","gb" },
    { "canterbury","gb" },{ "maidstone","gb" },{ "margate","gb" },{ "plymouth","gb" },
    { "exeter","gb" },{ "taunton","gb" },{ "truro","gb" },
    { "swansea","gb" },{ "belfast","gb" },{ "londonderry","gb" },{ "derry","gb" },
    { "larne","gb" },{ "stranraer","gb" },{ "wrexham","gb" },{ "chester","gb" },
    { "lancaster","gb" },{ "grimsby","gb" },{ "doncaster","gb" },
    /* benelux */ { "amsterdam","nl" },{ "rotterdam","nl" },{ "utrecht","nl" },{ "groningen","nl" },
    { "zwolle","nl" },{ "eindhoven","nl" },{ "arnhem","nl" },{ "breda","nl" },
    { "tilburg","nl" },{ "venlo","nl" },{ "enschede","nl" },{ "apeldoorn","nl" },
    { "emmen","nl" },{ "assen","nl" },{ "leeuwarden","nl" },{ "middelburg","nl" },
    { "brussels","be" },{ "bruxelles","be" },{ "antwerp","be" },{ "antwerpen","be" },
    { "gent","be" },{ "ghent","be" },{ "liege","be" },{ "brugge","be" },{ "bruges","be" },
    { "namur","be" },{ "leuven","be" },{ "charleroi","be" },{ "mons","be" },
    { "kortrijk","be" },{ "hasselt","be" },{ "luxembourg","lu" },
    /* alps */ { "bern","ch" },{ "zurich","ch" },{ "geneva","ch" },{ "basel","ch" },
    { "lausanne","ch" },{ "lugano","ch" },{ "locarno","ch" },{ "bellinzona","ch" },
    { "chur","ch" },{ "sion","ch" },{ "fribourg","ch" },{ "winterthur","ch" },
    { "neuchatel","ch" },{ "gallen","ch" },
    { "innsbruck","at" },{ "wien","at" },{ "vienna","at" },{ "graz","at" },{ "linz","at" },
    { "salzburg","at" },{ "klagenfurt","at" },{ "villach","at" },{ "leoben","at" },
    { "wiener neustadt","at" },{ "polten","at" },{ "amstetten","at" },{ "wels","at" },
    { "steyr","at" },{ "wolfsberg","at" },
    /* iberia (base + iberia dlc + promods) */ { "madrid","es" },{ "barcelona","es" },
    { "valencia","es" },{ "sevilla","es" },{ "seville","es" },{ "bilbao","es" },
    { "zaragoza","es" },{ "malaga","es" },{ "murcia","es" },{ "coruna","es" },
    { "valladolid","es" },{ "alicante","es" },{ "pamplona","es" },{ "logrono","es" },
    { "santander","es" },{ "oviedo","es" },{ "gijon","es" },{ "cadiz","es" },
    { "huelva","es" },{ "albacete","es" },{ "badajoz","es" },{ "ciudad real","es" },
    { "cuenca","es" },{ "guadalajara","es" },{ "segovia","es" },{ "soria","es" },
    { "teruel","es" },{ "toledo","es" },{ "leon","es" },{ "burgos","es" },
    { "lleida","es" },{ "tarragona","es" },{ "girona","es" },{ "castellon","es" },
    { "almeria","es" },{ "granada","es" },{ "jaen","es" },{ "cordoba","es" },
    { "vigo","es" },{ "pontevedra","es" },{ "santiago","es" },{ "ourense","es" },
    { "avila","es" },{ "salamanca","es" },{ "zamora","es" },{ "palencia","es" },
    { "lisboa","pt" },{ "lisbon","pt" },{ "porto","pt" },{ "coimbra","pt" },
    { "braga","pt" },{ "setubal","pt" },{ "faro","pt" },{ "aveiro","pt" },
    { "leiria","pt" },{ "evora","pt" },{ "funchal","pt" },{ "ponta delgada","pt" },
    /* italy (base + italia dlc + promods) */ { "roma","it" },{ "rome","it" },
    { "milano","it" },{ "milan","it" },{ "torino","it" },{ "turin","it" },
    { "genova","it" },{ "genoa","it" },{ "venezia","it" },{ "venice","it" },
    { "bologna","it" },{ "napoli","it" },{ "naples","it" },{ "firenze","it" },
    { "florence","it" },{ "palermo","it" },{ "messina","it" },{ "catanzaro","it" },
    { "ancona","it" },{ "bari","it" },{ "verona","it" },{ "trieste","it" },
    { "trento","it" },{ "bolzano","it" },{ "udine","it" },{ "vicenza","it" },
    { "padova","it" },{ "padua","it" },{ "brescia","it" },{ "bergamo","it" },
    { "parma","it" },{ "modena","it" },{ "piacenza","it" },{ "cremona","it" },
    { "mantova","it" },{ "ferrara","it" },{ "ravenna","it" },{ "rimini","it" },
    { "livorno","it" },{ "pisa","it" },{ "la spezia","it" },{ "arezzo","it" },
    { "siena","it" },{ "grosseto","it" },{ "perugia","it" },{ "terni","it" },
    { "latina","it" },{ "viterbo","it" },{ "pescara","it" },{ "foggia","it" },
    { "salerno","it" },{ "potenza","it" },{ "matera","it" },{ "cosenza","it" },
    { "reggio calabria","it" },{ "crotone","it" },{ "catania","it" },
    { "siracusa","it" },{ "syracuse","it" },{ "ragusa","it" },{ "cagliari","it" },
    { "sassari","it" },{ "olbia","it" },{ "alessandria","it" },{ "asti","it" },
    { "cuneo","it" },{ "novara","it" },{ "como","it" },{ "varese","it" },
    /* nordics (base + scandinavia dlc + promods) */
    { "kobenhavn","dk" },{ "copenhagen","dk" },{ "odense","dk" },{ "aalborg","dk" },
    { "esbjerg","dk" },{ "frederikshavn","dk" },{ "herning","dk" },{ "aarhus","dk" },
    { "randers","dk" },{ "vejle","dk" },{ "kolding","dk" },{ "silkeborg","dk" },
    { "sonderborg","dk" },{ "helsingor","dk" },
    { "stockholm","se" },{ "goteborg","se" },{ "gothenburg","se" },{ "jonkoping","se" },
    { "linkoping","se" },{ "malmo","se" },{ "uppsala","se" },{ "kalmar","se" },
    { "karlskrona","se" },{ "vaxjo","se" },{ "helsingborg","se" },{ "karlstad","se" },
    { "orebro","se" },{ "gavle","se" },{ "sundsvall","se" },{ "umea","se" },
    { "lulea","se" },{ "visby","se" },{ "norrkoping","se" },
    { "oslo","no" },{ "kristiansand","no" },{ "stavanger","no" },{ "bergen","no" },
    { "trondheim","no" },{ "tromso","no" },{ "bodo","no" },{ "narvik","no" },
    { "alta","no" },{ "kirkenes","no" },{ "molde","no" },{ "alesund","no" },
    { "steinkjer","no" },{ "mosjoen","no" },{ "sandnessjoen","no" },{ "orkanger","no" },
    { "fauske","no" },{ "lakselv","no" },{ "vardo","no" },{ "hammerfest","no" },
    { "honningsvag","no" },{ "mo i rana","no" },{ "batsfjord","no" },
    /* central europe */ { "praha","cz" },{ "prague","cz" },{ "brno","cz" },
    { "plzen","cz" },{ "pilsen","cz" },{ "ostrava","cz" },{ "liberec","cz" },
    { "olomouc","cz" },{ "usti nad labem","cz" },{ "hradec kralove","cz" },
    { "pardubice","cz" },{ "zlin","cz" },{ "karlovy vary","cz" },{ "jihlava","cz" },
    { "decin","cz" },{ "chomutov","cz" },
    { "warszawa","pl" },{ "warsaw","pl" },{ "krakow","pl" },{ "lodz","pl" },{ "poznan","pl" },
    { "szczecin","pl" },{ "gdansk","pl" },{ "gdynia","pl" },{ "katowice","pl" },
    { "lublin","pl" },{ "bialystok","pl" },{ "bydgoszcz","pl" },{ "wroclaw","pl" },
    { "zielona gora","pl" },{ "gorzow","pl" },{ "torun","pl" },{ "radom","pl" },
    { "olsztyn","pl" },{ "elblag","pl" },{ "rzeszow","pl" },{ "tarnow","pl" },
    { "opole","pl" },{ "gliwice","pl" },{ "zabrze","pl" },{ "sosnowiec","pl" },
    { "czestochowa","pl" },{ "koszalin","pl" },{ "slupsk","pl" },{ "legnica","pl" },
    { "walbrzych","pl" },{ "jelenia gora","pl" },{ "kalisz","pl" },{ "plock","pl" },
    { "przemysl","pl" },{ "zamosc","pl" },
    { "bratislava","sk" },{ "kosice","sk" },{ "nitra","sk" },{ "zilina","sk" },
    { "trnava","sk" },{ "trencin","sk" },{ "presov","sk" },{ "poprad","sk" },
    { "banska bystrica","sk" },{ "martin","sk" },{ "humenne","sk" },
    { "budapest","hu" },{ "pecs","hu" },{ "szeged","hu" },{ "debrecen","hu" },
    { "miskolc","hu" },{ "gyor","hu" },{ "kaposvar","hu" },{ "nagykanizsa","hu" },
    { "sopron","hu" },{ "szekesfehervar","hu" },{ "szolnok","hu" },{ "eger","hu" },
    { "veszprem","hu" },{ "zalaegerszeg","hu" },{ "bekescsaba","hu" },{ "szombathely","hu" },
    { "ljubljana","si" },{ "maribor","si" },{ "koper","si" },{ "celje","si" },{ "kranj","si" },
    /* ukraine (promods) */ { "lviv","ua" },{ "kyiv","ua" },{ "kiev","ua" },
    { "odesa","ua" },{ "odessa","ua" },{ "kharkiv","ua" },{ "dnipro","ua" },
    { "ternopil","ua" },{ "rivne","ua" },{ "zhytomyr","ua" },
    { "lutsk","ua" },{ "ivano frankivsk","ua" },{ "chernivtsi","ua" },{ "vinnytsia","ua" },
    { "cherkasy","ua" },{ "poltava","ua" },{ "sumy","ua" },{ "mykolaiv","ua" },
    { "kherson","ua" },{ "mariupol","ua" },{ "kramatorsk","ua" },
    { "kropyvnytskyi","ua" },{ "khmelnytskyi","ua" },{ "uzhhorod","ua" },
    { "mukachevo","ua" },{ "kryvyi rih","ua" },{ "donetsk","ua" },
    { "luhansk","ua" },{ "sevastopol","ua" },{ "simferopol","ua" },
    /* black sea (romania, bulgaria, moldova) */ { "bucuresti","ro" },{ "bucharest","ro" },
    { "cluj","ro" },{ "timisoara","ro" },{ "iasi","ro" },{ "constanta","ro" },
    { "brasov","ro" },{ "sibiu","ro" },{ "oradea","ro" },{ "craiova","ro" },
    { "suceava","ro" },{ "pitesti","ro" },{ "ploiesti","ro" },{ "arad","ro" },
    { "galati","ro" },{ "bacau","ro" },{ "lugoj","ro" },{ "deva","ro" },
    { "baia mare","ro" },{ "satu mare","ro" },{ "targu mures","ro" },{ "targu jiu","ro" },
    { "targoviste","ro" },{ "focsani","ro" },{ "buzau","ro" },{ "ramnicu valcea","ro" },
    { "tulcea","ro" },{ "giurgiu","ro" },{ "alexandria","ro" },{ "resita","ro" },
    { "hunedoara","ro" },{ "sighisoara","ro" },{ "medias","ro" },{ "vaslui","ro" },
    { "barlad","ro" },{ "fetesti","ro" },{ "severin","ro" },{ "slobozia","ro" },
    { "calarasi","ro" },{ "campina","ro" },{ "arges","ro" },
    { "fagaras","ro" },{ "vatra dornei","ro" },{ "radauti","ro" },{ "pascani","ro" },
    { "sofia","bg" },{ "sofiya","bg" },{ "plovdiv","bg" },{ "varna","bg" },
    { "burgas","bg" },{ "ruse","bg" },{ "pleven","bg" },{ "veliko tarnovo","bg" },
    { "shumen","bg" },{ "sliven","bg" },{ "stara zagora","bg" },{ "vidin","bg" },
    { "haskovo","bg" },{ "blagoevgrad","bg" },{ "dobrich","bg" },{ "lovech","bg" },
    { "gabrovo","bg" },{ "vratsa","bg" },{ "yambol","bg" },{ "montana","bg" },
    { "silistra","bg" },{ "razgrad","bg" },{ "svishtov","bg" },{ "pazardzhik","bg" },
    { "dimitrovgrad","bg" },
    { "chisinau","md" },{ "balti","md" },{ "tiraspol","md" },{ "cahul","md" },
    { "ungheni","md" },{ "orhei","md" },{ "comrat","md" },{ "bender","md" },
    { "soroca","md" },{ "dubasari","md" },
    /* west balkans */ { "zagreb","hr" },{ "split","hr" },{ "osijek","hr" },{ "rijeka","hr" },
    { "zadar","hr" },{ "sibenik","hr" },{ "pula","hr" },{ "karlovac","hr" },
    { "vukovar","hr" },{ "varazdin","hr" },{ "sisak","hr" },{ "slavonski brod","hr" },
    { "dubrovnik","hr" },
    { "sarajevo","ba" },{ "mostar","ba" },{ "banja luka","ba" },{ "tuzla","ba" },
    { "zenica","ba" },{ "bihac","ba" },{ "brcko","ba" },
    { "beograd","rs" },{ "belgrade","rs" },{ "novi sad","rs" },{ "nis","rs" },
    { "kragujevac","rs" },{ "subotica","rs" },{ "novi pazar","rs" },{ "sombor","rs" },
    { "zrenjanin","rs" },{ "vrsac","rs" },{ "uzice","rs" },{ "cacak","rs" },
    { "kraljevo","rs" },{ "vranje","rs" },{ "pirot","rs" },{ "leskovac","rs" },
    { "kikinda","rs" },{ "smederevo","rs" },{ "sabac","rs" },
    { "pristina","xk" },{ "prizren","xk" },{ "peja","xk" },{ "gjakova","xk" },
    { "mitrovica","xk" },{ "ferizaj","xk" },
    { "skopje","mk" },{ "ohrid","mk" },{ "bitola","mk" },{ "tetovo","mk" },
    { "stip","mk" },{ "veles","mk" },
    { "podgorica","me" },{ "niksic","me" },{ "bijelo polje","me" },
    { "tirana","al" },{ "durres","al" },{ "vlore","al" },{ "shkoder","al" },
    { "elbasan","al" },{ "fier","al" },
    /* greece (base + promods) */ { "athina","gr" },{ "athens","gr" },
    { "thessaloniki","gr" },{ "patras","gr" },{ "larissa","gr" },{ "ioannina","gr" },
    { "volos","gr" },{ "igoumenitsa","gr" },{ "preveza","gr" },{ "kalamata","gr" },
    { "corinth","gr" },{ "korinthos","gr" },{ "katerini","gr" },{ "serres","gr" },
    { "drama","gr" },{ "alexandroupoli","gr" },{ "komotini","gr" },{ "xanthi","gr" },
    { "kozani","gr" },{ "florina","gr" },{ "edessa","gr" },{ "veria","gr" },
    { "kilkis","gr" },{ "amfissa","gr" },
    /* turkey (promods) */ { "istanbul","tr" },{ "ankara","tr" },{ "izmir","tr" },
    { "bursa","tr" },{ "adana","tr" },{ "konya","tr" },{ "gaziantep","tr" },
    { "antalya","tr" },{ "eskisehir","tr" },{ "kayseri","tr" },{ "samsun","tr" },
    { "trabzon","tr" },{ "edirne","tr" },{ "tekirdag","tr" },{ "canakkale","tr" },
    { "balikesir","tr" },{ "denizli","tr" },{ "afyon","tr" },
    { "kutahya","tr" },{ "usak","tr" },{ "bolu","tr" },{ "zonguldak","tr" },
    { "sinop","tr" },{ "sivas","tr" },{ "malatya","tr" },{ "elazig","tr" },
    { "diyarbakir","tr" },{ "sanliurfa","tr" },{ "mardin","tr" },{ "batman","tr" },
    { "erzurum","tr" },{ "kars","tr" },{ "mersin","tr" },{ "hatay","tr" },
    /* caucasus (promods) */ { "tbilisi","ge" },{ "kutaisi","ge" },{ "batumi","ge" },
    { "rustavi","ge" },{ "zugdidi","ge" },{ "poti","ge" },
    { "yerevan","am" },{ "gyumri","am" },{ "vanadzor","am" },
    { "baku","az" },{ "ganja","az" },{ "sumqayit","az" },{ "mingachevir","az" },
    { "nakhchivan","az" },
    /* middle east (promods me) */ { "damascus","sy" },{ "aleppo","sy" },
    { "homs","sy" },{ "hama","sy" },{ "latakia","sy" },
    { "deir ez zor","sy" },{ "beirut","lb" },{ "tripoli","lb" },{ "saida","lb" },
    { "zahle","lb" },{ "jerusalem","il" },{ "tel aviv","il" },{ "haifa","il" },
    { "beersheba","il" },{ "netanya","il" },{ "ashdod","il" },{ "eilat","il" },
    { "amman","jo" },{ "zarqa","jo" },{ "irbid","jo" },{ "aqaba","jo" },
    { "madaba","jo" },{ "karak","jo" },{ "baghdad","iq" },{ "basra","iq" },
    { "mosul","iq" },{ "erbil","iq" },{ "arbil","iq" },{ "kirkuk","iq" },
    { "sulaymaniyah","iq" },{ "najaf","iq" },{ "karbala","iq" },{ "duhok","iq" },
    { "riyadh","sa" },{ "jeddah","sa" },{ "dammam","sa" },{ "tabuk","sa" },
    { "buraydah","sa" },{ "abha","sa" },{ "jubail","sa" },{ "yanbu","sa" },
    { "mecca","sa" },{ "medina","sa" },
    /* iran & turkmenistan (road to asia) */ { "tehran","ir" },{ "tabriz","ir" },
    { "mashhad","ir" },{ "isfahan","ir" },{ "shiraz","ir" },{ "ahvaz","ir" },
    { "kermanshah","ir" },{ "kerman","ir" },{ "rasht","ir" },
    { "yazd","ir" },{ "urmia","ir" },{ "sanandaj","ir" },{ "zahedan","ir" },
    { "bandar abbas","ir" },{ "ardabil","ir" },{ "ashgabat","tm" },
    { "turkmenbasy","tm" },{ "dashoguz","tm" },{ "balkanabat","tm" },
    /* central asia (road to asia) */ { "almaty","kz" },{ "astana","kz" },
    { "shymkent","kz" },{ "karaganda","kz" },{ "aktobe","kz" },{ "taraz","kz" },
    { "pavlodar","kz" },{ "semey","kz" },{ "atyrau","kz" },{ "kostanay","kz" },
    { "uralsk","kz" },{ "aktau","kz" },{ "zhezkazgan","kz" },{ "turkistan","kz" },
    { "tashkent","uz" },{ "samarkand","uz" },{ "bukhara","uz" },{ "andijan","uz" },
    { "namangan","uz" },{ "nukus","uz" },{ "urgench","uz" },{ "fergana","uz" },
    { "kokand","uz" },{ "jizzakh","uz" },{ "navoi","uz" },{ "qarshi","uz" },
    { "termez","uz" },    { "bishkek","kg" },{ "osh","kg" },
    { "naryn","kg" },{ "jalal abad","kg" },{ "talas","kg" },{ "batken","kg" },
    { "tokmok","kg" },{ "dushanbe","tj" },{ "khujand","tj" },{ "kulob","tj" },
    { "bokhtar","tj" },{ "panjakent","tj" },{ "khorugh","tj" },
    /* iceland (promods) */ { "reykjavik","is" },{ "akureyri","is" },
    { "keflavik","is" },{ "selfoss","is" },{ "isafjordur","is" },
    { "egilsstadir","is" },{ "hofn","is" },{ "akranes","is" },{ "borgarnes","is" },
    { "seydisfjordur","is" },{ "breiddalsvik","is" },{ "dalvik","is" },
    { "husavik","is" },{ "blonduos","is" },{ "stykkisholmur","is" },
    /* ireland (promods) */ { "dublin","ie" },{ "cork","ie" },{ "limerick","ie" },
    { "galway","ie" },{ "waterford","ie" },{ "wexford","ie" },{ "kilkenny","ie" },
    { "sligo","ie" },{ "drogheda","ie" },{ "dundalk","ie" },{ "letterkenny","ie" },
    { "athlone","ie" },{ "tralee","ie" },{ "carlow","ie" },{ "naas","ie" },
    { "mullingar","ie" },
    /* microstates */ { "andorra","ad" },{ "valletta","mt" },
    /* ordering-sensitive: "nice" is a substring of "venice" and
     * "olten" of "polten", so both stay after those (first match wins) */
    { "nice","fr" },{ "olten","ch" },
};

struct CcName { const char* cc; const char* name; };
static const CcName CC_NAMES[] = {
    { "de","Germany" },{ "fr","France" },{ "gb","United Kingdom" },{ "nl","Netherlands" },
    { "be","Belgium" },{ "lu","Luxembourg" },{ "ch","Switzerland" },{ "at","Austria" },
    { "es","Spain" },{ "pt","Portugal" },{ "it","Italy" },{ "dk","Denmark" },
    { "se","Sweden" },{ "no","Norway" },{ "fi","Finland" },{ "cz","Czechia" },
    { "pl","Poland" },{ "sk","Slovakia" },{ "hu","Hungary" },{ "si","Slovenia" },
    { "ru","Russia" },{ "by","Belarus" },{ "ee","Estonia" },{ "lv","Latvia" },
    { "lt","Lithuania" },{ "ro","Romania" },{ "bg","Bulgaria" },{ "hr","Croatia" },
    { "ba","Bosnia and Herzegovina" },{ "rs","Serbia" },{ "me","Montenegro" },
    { "al","Albania" },{ "gr","Greece" },
    { "ie","Ireland" },{ "is","Iceland" },{ "ua","Ukraine" },{ "md","Moldova" },
    { "tr","Turkey" },{ "ge","Georgia" },{ "am","Armenia" },{ "az","Azerbaijan" },
    { "sy","Syria" },{ "lb","Lebanon" },{ "il","Israel" },{ "jo","Jordan" },
    { "iq","Iraq" },{ "sa","Saudi Arabia" },{ "ir","Iran" },{ "tm","Turkmenistan" },
    { "kz","Kazakhstan" },{ "uz","Uzbekistan" },{ "kg","Kyrgyzstan" },{ "tj","Tajikistan" },
    { "ad","Andorra" },{ "mt","Malta" },{ "mk","North Macedonia" },{ "xk","Kosovo" },
};

/* ── city-name folding ─────────────────────────────────
 * The game reports city names in the user's UI language: a Russian
 * client sends Cyrillic ("Выборг"), German keeps umlauts ("Köln").
 * Folding maps every spelling into one normalized alphabet: ASCII
 * lowercase, accents stripped, Cyrillic transliterated. Table keys
 * and game names both go through it, so a city matches in every
 * locale without the user mapping anything by hand. */
static const char* const LATIN1_FOLD[64] = {
    "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
    "d","n","o","o","o","o","o","", "o","u","u","u","u","y","th","ss",
    "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
    "d","n","o","o","o","o","o","", "o","u","u","u","u","y","th","y"
};

/* Cyrillic а..я (U+0430..U+044F); ъ and ь fold to nothing */
static const char* const CYRILLIC_FOLD[32] = {
    "a","b","v","g","d","e","zh","z","i","i","k","l","m","n","o","p",
    "r","s","t","u","f","h","c","ch","sh","sch","","y","","e","yu","ya"
};

static void fold_utf8(const std::string& in, std::string& out) {
    out.clear(); out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out += (char)std::tolower(c); ++i; continue; }

        unsigned int cp = 0; size_t len = 0;
        if      ((c & 0xE0) == 0xC0 && i + 1 < in.size()) {
            cp = ((unsigned)(c & 0x1F) << 6)  | ((unsigned)(unsigned char)in[i+1] & 0x3F); len = 2;
        }
        else if ((c & 0xF0) == 0xE0 && i + 2 < in.size()) {
            cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)(unsigned char)in[i+1] & 0x3F) << 6
               |  ((unsigned)(unsigned char)in[i+2] & 0x3F); len = 3;
        }
        else { ++i; continue; }             /* 4-byte or broken: drop */
        i += len;

        if (cp >= 0xC0 && cp <= 0xFF) {     /* Latin-1 supplement */
            out += LATIN1_FOLD[cp - 0xC0]; continue;
        }
        if (cp >= 0x100 && cp <= 0x17F) {   /* Latin Extended-A */
            unsigned int d = cp - 0x100;
            if      (d <= 0x05) out += 'a';
            else if (d <= 0x0D) out += 'c';
            else if (d <= 0x11) out += 'd';
            else if (d <= 0x1B) out += 'e';
            else if (d <= 0x23) out += 'g';
            else if (d <= 0x27) out += 'h';
            else if (d <= 0x31) out += 'i';
            else if (d <= 0x35 && d >= 0x34) out += 'j';
            else if (d <= 0x38) out += 'k';
            else if (d <= 0x42) out += 'l';
            else if (d <= 0x4B) out += 'n';
            else if (d <= 0x51) out += 'o';
            else if (d <= 0x53) out += "oe";
            else if (d <= 0x59) out += 'r';
            else if (d <= 0x61) out += 's';
            else if (d <= 0x67) out += 't';
            else if (d <= 0x73) out += 'u';
            else if (d <= 0x75) out += 'w';
            else if (d <= 0x78) out += 'y';
            else if (d <= 0x7E) out += 'z';
            else                out += 's';
            continue;
        }
        if (cp >= 0x400 && cp <= 0x4FF) {   /* Cyrillic */
            unsigned int d = cp - 0x400;
            if      (d == 0x01 || d == 0x51) { out += 'e'; continue; }  /* Ё ё */
            else if (d == 0x04 || d == 0x54) { out += 'e'; continue; }  /* Є є */
            else if (d == 0x06 || d == 0x56) { out += 'i'; continue; }  /* І і */
            else if (d == 0x07 || d == 0x57) { out += 'i'; continue; }  /* Ї ї */
            else if (d == 0x0E || d == 0x5E) { out += 'u'; continue; }  /* Ў ў */
            else if (d == 0x90 || d == 0x91) { out += 'g'; continue; }  /* Ґ ґ */
            else if (d >= 0x10 && d <= 0x4F) {
                unsigned int idx = (d >= 0x30) ? (d - 0x30)             /* а..я lowercase */
                                               : (d - 0x10);            /* А..Я uppercase */
                out += CYRILLIC_FOLD[idx];
            }
            continue;
        }
        /* anything else: dropped, keeps matching deterministic */
    }
}

static std::string norm_city(const char* city) {
    std::string out;
    fold_utf8(city ? city : "", out);
    return out;
}

/* City matching helper: does either known city contain the fragment? */
static bool city_has(const std::string& fragment) {
    if (fragment.empty()) return false;
    std::string f; fold_utf8(fragment, f);
    if (f.empty()) return false;
    std::string dest = norm_city(g.dest_city);
    std::string src  = norm_city(g.src_city);
    return (!dest.empty() && dest.find(f) != std::string::npos) ||
           (!src.empty()  && src.find(f)  != std::string::npos);
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

/* v5.0.4: 2-letter code -> 3-letter uppercase display code
 * (ISO 3166-1 alpha-3) for the per-city tags, e.g. fi -> FIN. */
static std::string cc_to_iso3(const char* cc2) {
    struct Cc3 { const char* cc2; const char* cc3; };
    static const Cc3 MAP[] = {
        { "de","DEU" },{ "fr","FRA" },{ "gb","GBR" },{ "nl","NLD" },
        { "be","BEL" },{ "lu","LUX" },{ "ch","CHE" },{ "at","AUT" },
        { "es","ESP" },{ "pt","PRT" },{ "it","ITA" },{ "dk","DNK" },
        { "se","SWE" },{ "no","NOR" },{ "fi","FIN" },{ "cz","CZE" },
        { "pl","POL" },{ "sk","SVK" },{ "hu","HUN" },{ "si","SVN" },
        { "ru","RUS" },{ "by","BLR" },{ "ee","EST" },{ "lv","LVA" },
        { "lt","LTU" },{ "ro","ROU" },{ "bg","BGR" },{ "hr","HRV" },
        { "ba","BIH" },{ "rs","SRB" },{ "me","MNE" },{ "al","ALB" },
        { "gr","GRC" },
        { "ie","IRL" },{ "is","ISL" },{ "ua","UKR" },{ "md","MDA" },
        { "tr","TUR" },{ "ge","GEO" },{ "am","ARM" },{ "az","AZE" },
        { "sy","SYR" },{ "lb","LBN" },{ "il","ISR" },{ "jo","JOR" },
        { "iq","IRQ" },{ "sa","SAU" },{ "ir","IRN" },{ "tm","TKM" },
        { "kz","KAZ" },{ "uz","UZB" },{ "kg","KGZ" },{ "tj","TJK" },
        { "ad","AND" },{ "mt","MLT" },{ "mk","MKD" },{ "xk","XKX" },
    };
    if (!cc2) return "";
    for (const auto& e : MAP)
        if (strcmp(cc2, e.cc2) == 0) return e.cc3;
    return "";
}

/* v5.0.4: resolve ONE city name to its 2-letter country code.
 * [countries] mappings run before the built-in table, so a user
 * entry wins for that city too. Empty when the city is unknown. */
static std::string cc_for_city(const char* city) {
    std::string c = norm_city(city);
    if (c.empty()) return "";
    for (const auto& kv : cfg::countries()) {
        std::string k; fold_utf8(kv.first, k);
        const std::string& v = kv.second;
        if (!k.empty() && c.find(k) != std::string::npos &&
            v.size() == 2 &&
            ((v[0] >= 'a' && v[0] <= 'z') || (v[0] >= '0' && v[0] <= '9')) &&
            (v[1] >= 'a' && v[1] <= 'z'))
            return v;
    }
    for (const auto& e : CITIES)
        if (c.find(e.city) != std::string::npos) return e.cc;
    return "";
}

/* v5.0.4: ready-to-append ISO tag for one city, e.g. " (FIN)".
 * Empty when the city is unknown or off the map, so a line can
 * never end in bare parentheses. */
static std::string city_cc_tag(const char* city) {
    std::string iso3 = cc_to_iso3(cc_for_city(city).c_str());
    return iso3.empty() ? "" : " (" + iso3 + ")";
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

/* ── output hygiene ─────────────────────────────────────
 * Tokens vanish when their data is missing, which can leave junk
 * behind: empty "( )", dangling arrows, a hanging "to" at the end.
 * Every filled line runs through this, so no template can ever
 * render that garbage - whatever the ini says. */
static bool is_sep_char(char c) {
    return c == ' ' || c == '\t' || c == '-' || c == '|';
}

static void tidy_line(std::string& s) {
    /* collapse whitespace runs */
    std::string out; out.reserve(s.size());
    bool sp = false;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!sp && !out.empty()) out += ' ';
            sp = true;
        } else { out += c; sp = false; }
    }
    s.swap(out);

    for (;;) {
        /* remove empty parenthesis pairs left by vanished tokens */
        bool removed = false;
        for (size_t p = s.find('('); p != std::string::npos && !removed;
             p = s.find('(', p + 1)) {
            size_t e = s.find(')', p);
            if (e == std::string::npos) break;
            size_t q = p + 1;
            while (q < e && (s[q] == ' ' || s[q] == '\t')) ++q;
            if (q == e) { s.erase(p, e - p + 1); removed = true; }
            break;                      /* non-empty parens stay */
        }

        /* trim separators and separator glyphs from both ends, UTF-8
         * aware so multibyte arrows and middle dots survive intact */
        auto trim_front = [&]() {
            for (;;) {
                if (s.empty()) break;
                if (is_sep_char(s.front())) { s.erase(0, 1); continue; }
                if (s.compare(0, 3, "\xE2\x86\x92") == 0) {   /* arrow */
                    s.erase(0, 3); continue;
                }
                if (s.compare(0, 2, "\xC2\xB7") == 0 ||       /* middle dot */
                    s.compare(0, 3, "\xE3\x83\xBB") == 0) {   /* katakana dot */
                    s.erase(0, s[1] == (char)0xB7 ? 2 : 3); continue;
                }
                break;
            }
        };
        auto trim_back = [&]() {
            for (;;) {
                if (s.empty()) break;
                char c0 = s.back();
                size_t n = s.size();
                if (is_sep_char(c0)) { s.pop_back(); continue; }
                if (c0 == ',' || c0 == ':') { s.pop_back(); continue; }
                if (n >= 3 && (unsigned char)c0 == 0x92 &&
                    (unsigned char)s[n-2] == 0x86 &&
                    (unsigned char)s[n-3] == 0xE2) {            /* arrow */
                    s.resize(n - 3); continue;
                }
                if (n >= 2 && (unsigned char)c0 == 0xB7 &&
                    (unsigned char)s[n-2] == 0xC2) {            /* middle dot */
                    s.resize(n - 2); continue;
                }
                if (n >= 3 && (unsigned char)c0 == 0xBB &&
                    (unsigned char)s[n-2] == 0x83 &&
                    (unsigned char)s[n-3] == 0xE3) {            /* katakana dot */
                    s.resize(n - 3); continue;
                }
                break;
            }
        };

        size_t before = s.size();
        trim_front();
        trim_back();

        /* no line may end on a hanging preposition */
        static const char* const HANGERS[] =
            { " to", " near", " in", " at", " of", " from", " through", " on" };
        for (const char* h : HANGERS) {
            size_t hl = strlen(h);
            if (s.size() > hl && s.compare(s.size() - hl, hl, h) == 0)
                s.resize(s.size() - hl);
        }

        if (s.size() != before) continue;   /* removals may expose new junk */
        break;
    }

    /* a line made only of separators is no line at all */
    bool only_sep = !s.empty();
    for (char c : s)
        if (!is_sep_char(c) && c != '(' && c != ')') { only_sep = false; break; }
    if (only_sep) s.clear();
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
    if (now < g_ferry_until)                     return State::ON_FERRY;
    if (now < g_train_until)                     return State::ON_TRAIN;

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
    float dist_m = g.nav_distance_m;

    /* country tokens always resolve so {city}, {country} and
     * {country_emoji} work in every template. The badge slot itself is
     * brand-only: flags would need per-country art we cannot assume. */
    std::string cc = country_text_cc();

    /* v5.0.2: job-bound tokens stay EMPTY when there is no job (or no
     * data): pausing in free-roam must not print "(0 kg)" or a nameless
     * cargo. A template with a static "({mass})" therefore renders
     * "()" in free-roam, which tidy_line() strips - the honest output
     * is nothing, not a fake zero. */
    tok["cargo"]          = g.job_active ? g.cargo_name : "";
    tok["mass"]           = (g.job_active && g.cargo_mass_kg > 1.f)
                          ? fmt_mass(g.cargo_mass_kg, imp) : "";

    /* v5.0.2: delivery_complete renders AFTER the delivered event wiped
     * the job buffers, so {cargo}/{dest} would be empty exactly when the
     * completion card needs them. Both are snapshotted at event time. */
    tok["cargo"]          = tok["cargo"].empty() ? g.last_cargo : tok["cargo"];
    /* v5.0.4: live buffers first, delivery snapshot as the fallback.
     * Two stray re-assignments from v5.0.3 overwrote this with the
     * raw buffers, which made the snapshot dead code - the
     * delivery_complete card lost its city names entirely. */
    tok["dest"]           = g.dest_city[0] ? g.dest_city : g.last_dest;
    tok["src"]            = g.src_city[0]  ? g.src_city  : g.last_src;
    tok["company"]        = g.dest_company[0] ? g.dest_company : g.src_company;
    /* v5.0.2: with no route (free-roam, menus) the honest value is
     * nothing, not a fabricated "0 km". */
    tok["distance_unit"]  = dist_unit;
    if (dist_m > 0.f) {
        tok["distance"]           = fmt_distance(dist_m, imp, &dist_unit);
        tok["distance_remaining"] = tok["distance"];
    } else {
        tok["distance"].clear();
        tok["distance_remaining"].clear();
    }
    tok["speed"]          = fmt_speed(g.speed_kmh, imp, &spd_unit);
    tok["speed_unit"]     = spd_unit;
    tok["speed_limit"]    = g.speed_limit_kmh > 0.f
                          ? fmt_speed(g.speed_limit_kmh, imp, &spd_unit) + std::string(" ") + spd_unit : "";
    tok["over_limit"]     = (g.speed_limit_kmh > 0.f && g.speed_kmh > g.speed_limit_kmh)
                          ? std::to_string((int)std::lround((g.speed_kmh / g.speed_limit_kmh - 1.f) * 100.f)) : "";
    tok["fuel"]           = std::to_string((int)std::lround(
                            (g.fuel_capacity_l > 0.f ? g.fuel_l / g.fuel_capacity_l : 0.f) * 100.f));
    tok["damage"]         = std::to_string((int)std::lround(g.cargo_damage * 100.f));
    /* v5.0.2: chassis wear had no token of its own, so truck_damaged
     * templates showed the CARGO damage percentage instead. {wear} is
     * the chassis/truck number; {damage} stays cargo-only. */
    tok["wear"]           = std::to_string((int)std::lround(g.wear_chassis * 100.f));
    tok["fine"]           = g.fine_amount > 0.f ? fmt_money((int64_t)g.fine_amount) : "";
    tok["brand"]          = g.truck_brand;
    tok["model"]          = g.truck_model;
    tok["truck"]          = std::string(g.truck_brand) + " " + g.truck_model;
    tok["brand_asset"]    = brand_asset(g.truck_brand);
    tok["time"]           = fmt_clock(cfg::use_24h());
    tok["eta"]            = g.nav_time_min > 0.f
                          ? std::to_string((int)std::lround(g.nav_time_min)) : "";
    /* v5.0: arrival as a wall-clock time, e.g. 17:45 (12h style honours use_24h) */
    if (g.nav_time_min > 0.f) {
        time_t eta_t = time(nullptr) + (time_t)(g.nav_time_min * 60.f);
        struct tm eta_lt; localtime_s(&eta_lt, &eta_t);
        char ebuf[16];
        if (cfg::use_24h()) snprintf(ebuf, sizeof(ebuf), "%02d:%02d", eta_lt.tm_hour, eta_lt.tm_min);
        else { int h = eta_lt.tm_hour % 12; if (!h) h = 12;
               snprintf(ebuf, sizeof(ebuf), "%d:%02d %s", h, eta_lt.tm_min, eta_lt.tm_hour < 12 ? "AM" : "PM"); }
        tok["eta_clock"]  = ebuf;
    } else tok["eta_clock"] = "";
    /* session odometer: distance driven THIS SESSION, computed as
     * speed x time between pushes. No file, no baseline, nothing to
     * reset: it lives and dies with the session by design. */
    if (g.odom_t0 == 0) g.odom_t0 = now_s();
    {
        int64_t odom_now = now_s();
        float dt = (float)(odom_now - g.odom_t0);
        g.odom_t0 = odom_now;
        /* paused or stationary time adds nothing */
        if (dt > 0.f && dt < 120.f && !g_paused && g.speed_kmh > 1.f)
            g.driven_m += g.speed_kmh / 3.6f * dt;   /* km/h -> m/s, times s */
    }
    if (g.driven_m >= 1000.f) {
        /* v5.0.7: fmt_distance returns the number only; the unit comes
         * back through du - "9.4 km", not a bare "9.4" */
        const char* du = "";
        tok["driven"] = fmt_distance(g.driven_m, imp, &du);
        tok["driven"] += " ";
        tok["driven"] += du;
    } else
        tok["driven"] = g.driven_m >= 1.f
            ? std::to_string((int)std::lround(g.driven_m)) + " m" : "";
    tok["driven_tag"] = tok["driven"].empty()
        ? "" : " \u00b7 " + tok["driven"] + " driven";
    /* elapsed session time, ready to append: " ・ 1 h 24 min". Counts
     * wall-clock from plugin start, unlike {time} which is the clock. */
    {
        int64_t sess = now_s() - (g.session_start ? g.session_start : now_s());
        if (sess < 0) sess = 0;
        char sbuf[32];
        if      (sess >= 5400) snprintf(sbuf, sizeof(sbuf), "%dh %dmin",  (int)(sess / 3600), (int)((sess % 3600) / 60));
        else if (sess >=   60) snprintf(sbuf, sizeof(sbuf), "%d min",     (int)(sess / 60));
        else                   snprintf(sbuf, sizeof(sbuf), "just started");
        tok["session"]      = sbuf;
        tok["session_tag"] = (sess >= 60) ? " \u00b7 " + std::string(sbuf) + " in" : "";
    }
    tok["fuel_l"]        = std::to_string((int)std::lround(g.fuel_l));
    tok["jobs_done"]     = std::to_string(g_jobs_done);
    tok["state_name"]    = state_key(g_state);
    tok["income"]         = (g.income > 0 ? g.income : g_delivered_income) > 0
                          ? fmt_money(g.income > 0 ? g.income : g_delivered_income) : "";
    tok["country"]        = cc.empty() ? "" : country_name(cc);
    std::string ccu = cc;   /* uppercase for display, e.g. (DE), (RU) */
    for (auto& c : ccu) c = (char)std::toupper((unsigned char)c);
    tok["country_code"]   = cc.empty() ? "" : ccu;
    /* ready-to-append tag: " (FI)" when known, empty (never bare parens)
     * when the city could not be matched */
    tok["country_tag"]    = cc.empty() ? "" : " (" + ccu + ")";
    /* v5.0.4: per-city 3-letter codes, e.g. "Pori (FIN) → Oslo (NOR)".
     * Empty for unknown cities, so nothing renders as bare parens. */
    tok["src_tag"]        = city_cc_tag(tok["src"].c_str());
    tok["dest_tag"]       = city_cc_tag(tok["dest"].c_str());
    tok["ferry_tag"]      = city_cc_tag(g.ferry_to);
    tok["country_flag"]   = cc.empty() ? "" : "flag_" + cc;
    tok["country_emoji"]  = cc.empty() ? "" : country_emoji(cc);
    tok["city"]           = g.dest_city[0] ? g.dest_city : g.src_city;
    tok["ferry_from"]     = g.ferry_from;
    tok["ferry_to"]       = g.ferry_to;
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
        r.state = g.ferry_to[0] ? "On a ferry → {ferry_to}"
                                : "On a ferry";
        r.details = g.ferry_from[0]
            ? "Crossing from {ferry_from}" : "Crossing the water";
        break;

    case State::ON_TRAIN:
        r.state = g.ferry_to[0] ? "On a train → {ferry_to}"
                                : "On a train";
        r.details = g.ferry_from[0]
            ? "Rail freight from {ferry_from}" : "Rail freight";
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

    /* the ferry/train routes come from the gameplay event and would
     * otherwise leak a stale crossing into later states via templates */
    if (g_state != State::ON_FERRY && g_state != State::ON_TRAIN) {
        tok["ferry_from"].clear();
        tok["ferry_to"].clear();
        tok["ferry_tag"].clear();
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

    /* - fill tokens, clamp to Discord limits -----------
     * Every line is tidied after the fill: collapsed spaces, removed
     * empty parens, trimmed separator junk, no hanging prepositions. */
    auto fill = [&](std::string s) {
        std::string t = eval(s, tok);
        tidy_line(t);
        return clamped(t, 128);
    };
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
    ++g_push_count;
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
                       g.speed_kmh = sane_f(v->value_float.value) * 3.6f;      }
CB(cb_speedlim)  { if (v && v->type==SCS_VALUE_TYPE_float)
                       g.speed_limit_kmh = sane_f(v->value_float.value) * 3.6f;}
CB(cb_fuel)      { rdflt(v, g.fuel_l);          g.fuel_l        = sane_f(g.fuel_l);        }
CB(cb_w_chassis) { rdflt(v, g.wear_chassis);    g.wear_chassis  = sane_f(g.wear_chassis);  }
CB(cb_engine)    { rdblm(v, g.engine_on);                                     }
CB(cb_navdist)   { rdflt(v, g.nav_distance_m);  g.nav_distance_m = sane_f(g.nav_distance_m); }
CB(cb_navtime)   { rdflt(v, g.nav_time_min);    g.nav_time_min   = sane_f(g.nav_time_min);   }
CB(cb_cargodmg)  { rdflt(v, g.cargo_damage);    g.cargo_damage   = sane_f(g.cargo_damage);   }
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
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_income))            rds64(v, g.income);
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand_id))          rdstr(v, g.truck_brand,  sizeof(g.truck_brand));
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_name))              rdstr(v, g.truck_model,  sizeof(g.truck_model));
        else if (!strcmp(n, SCS_TELEMETRY_CONFIG_ATTRIBUTE_fuel_capacity))     rdflt(v, g.fuel_capacity_l);
    }

    if (saw_job_dest && !g.job_active) {
        g.job_active = true;   /* a job config appeared: delivery is on */
        g.last_cargo[0] = '\0';   /* fresh job: the old snapshot must
        g.last_dest[0]  = '\0';      never leak into the new card */
        g.last_src[0]   = '\0';
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
        /* snapshot BEFORE wiping: the completion card still needs
         * cargo and the route it was hauled on */
        strncpy(g.last_cargo, g.cargo_name, sizeof(g.last_cargo) - 1);
        strncpy(g.last_dest,  g.dest_city,  sizeof(g.last_dest)  - 1);
        strncpy(g.last_src,   g.src_city,   sizeof(g.last_src)   - 1);
        g.job_active = false;
        g.income = 0;
        g_delivered_income = 0;
        g.dest_city[0] = '\0';
        g.src_city[0] = '\0';
        g.cargo_name[0] = '\0';
        g_ferry_until = 0;
        g_train_until = 0;
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
        ++g_jobs_done;
    }
    else if (!strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_job_cancelled)) {
        strncpy(g.last_cargo, g.cargo_name, sizeof(g.last_cargo) - 1);
        strncpy(g.last_dest,  g.dest_city,  sizeof(g.last_dest)  - 1);
        strncpy(g.last_src,   g.src_city,   sizeof(g.last_src)   - 1);
        g.job_active = false;
        g.income = 0;
        g.dest_city[0] = '\0';
        g.src_city[0] = '\0';
        g.cargo_name[0] = '\0';
        g_ferry_until = 0;
        g_train_until = 0;
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
    else if (!strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_use_ferry) ||
             !strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_use_train)) {
        /* Ferry and train events fire when the crossing STARTS. The game
         * never sends an end event, so the state is held for event_hold
         * seconds (configurable) and then must expire on its own. The
         * real source/target names arrive as event attributes; the job
         * destination has nothing to do with the crossing route. */
        /* v5.0.4: the old index math (id[12] == 'f') was off by one
         * - index 12 is 'e' - so EVERY crossing sorted into the train
         * branch and ferries rode as "Rail freight". Match the event
         * id by name instead; no arithmetic to get wrong. */
        const bool is_ferry = strstr(id, "use.ferry") != nullptr;
        g.ferry_from[0] = '\0';
        g.ferry_to[0]   = '\0';
        if (gev->attributes) {
            for (const scs_named_value_t* a = gev->attributes; a->name; ++a) {
                if      (!strcmp(a->name, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_source_name) &&
                         a->value.type == SCS_VALUE_TYPE_string && a->value.value_string.value)
                    rdstr(&a->value, g.ferry_from, sizeof(g.ferry_from));
                else if (!strcmp(a->name, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_target_name) &&
                         a->value.type == SCS_VALUE_TYPE_string && a->value.value_string.value)
                    rdstr(&a->value, g.ferry_to,   sizeof(g.ferry_to));
            }
        }
        const int64_t until = now + (hold > 0 ? hold : 1);
        if (is_ferry) { g_ferry_until = until; g_train_until = 0; }
        else          { g_train_until = until; g_ferry_until = 0; }
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
    /* v5.0: a fault in a hot loop could log-spam the game file; only
     * the first three, and at most one per minute after that */
    int64_t now = (int64_t)time(nullptr);
    if (g_seh_faults <= 3 || now - g_last_fault_log >= 60) {
        g_last_fault_log = now;
        char buf[128];
        snprintf(buf, sizeof(buf), "[ETS2rpcMKII] recovered from internal fault in %s (total %d)",
                 where, g_seh_faults);
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
             cfg::VERSION, SCS_GET_MAJOR_VERSION(version), SCS_GET_MINOR_VERSION(version));
    log_msg(banner);

    if (SCS_GET_MAJOR_VERSION(version) != 1) {
        snprintf(banner, sizeof(banner),
                 "[ETS2rpcMKII] unsupported telemetry major version %u, plugin disabled.",
                 SCS_GET_MAJOR_VERSION(version));
        log_msg(banner);
        return SCS_RESULT_unsupported;
    }

    g.session_start = (int64_t)time(nullptr);
    g_session_t0    = g.session_start;
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

    /* v5.0: one-line session summary, so a user reporting a problem
     * pastes something diagnostic instead of nothing */
    int64_t now = (int64_t)time(nullptr);
    long mins = g_session_t0 ? (long)((now - g_session_t0) / 60) : 0;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "[ETS2rpcMKII] Session: %ld min, %llu presence updates, %d recovered faults. Shutdown.",
             mins, (unsigned long long)g_push_count, g_seh_faults);
    log_msg(buf);
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
