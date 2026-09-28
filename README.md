# ETS2rpcMKII

[![build](https://github.com/YOUR_USERNAME/ets2rpcmkii/actions/workflows/build.yml/badge.svg)](https://github.com/YOUR_USERNAME/ets2rpcmkii/actions/workflows/build.yml)

> ## READ THIS FIRST: legacy Discord tech, and there will be no migration
>
> This plugin is built on the tech of Discord's **legacy Game SDK**: it does
> not ship or load the SDK binary, but it speaks the same **legacy Rich
> Presence protocol** (the local `discord-ipc` named pipe) that the Game SDK
> used.
>
> Discord has moved the Game SDK to **Legacy status** and states it is **no
> longer being maintained**, in favor of the new **Discord Social SDK**. The
> Social SDK path is designed for registered, and for full features verified,
> game developers: you register and claim or submit a *game* identity. Doing
> that for Euro Truck Simulator 2 would mean presenting ourselves as the
> game's developer. That is **impersonation of SCS Software**, and out of
> respect for the studio, **this project will never do it.**
>
> **What this means for you:** the tool works today because the Discord
> desktop client still honors the legacy pipe. If Discord ever patches legacy
> Rich Presence out or breaks the protocol, **this tool will NOT get an
> update to the Social SDK.** It would die with the legacy tech. Use it
> knowing that.

**The Discord Rich Presence plugin for Euro Truck Simulator 2, Mark II.**

Current release: **v4.2.0**. Full history in [Version history](#version-history).

One self-contained DLL. No Discord SDK, no bundled DLLs, no OAuth, no
background services. Live telemetry on your Discord profile, and every
single field of it is yours to redesign from `ets2rpcmkii.ini`,
hot-reloaded while the game runs. Every build ships with a ready-made ini
plus a `templates/` folder of style presets, and a **Get on Steam** button
on your profile, just like official game profiles.

```
Scania S 2016 (87 km/h)
24 t - 132 km remaining
  [Get on Steam]
 small badge: brand logo or the flag of the country you drive in
```

Full manual with every state, field and token: [DOCS.md](DOCS.md)

## Compatibility

| | |
|---|---|
| Game | Euro Truck Simulator 2 (Steam, Windows x64) |
| Tested game version | **1.61.x** (verified on 1.61.1.1s) |
| Telemetry API | SCS Telemetry **1.00** (`SCS_TELEMETRY_VERSION 1.00`) |
| SDK game version | eut2 **1.18** (`SCS_SDK_GAME_VERSION_eut2`, from the official headers) |
| Discord | desktop client running; the legacy `discord-ipc` Rich Presence pipe |
| Install location | `ETS2/bin/win_x64/plugins/ets2rpcmkii.dll` |

Older game versions back to roughly 1.37 expose the same telemetry API 1.00,
so the plugin should keep working across updates; only the SDK headers it was
built against are from the 1.61 era.

## What is new in MKII (v4)

- **Rebuilt on the real official SCS SDK** (v4.1.0): the plugin now uses
  the genuine SCS Telemetry SDK headers (API 1.00, game version 1.18),
  reads job data from the configuration event the way the real SDK
  delivers it, walks event attributes with the correct struct layout,
  and logs every startup step so a rejected load can never be silent.
- **Ini failsafe** (v4.1.0): if `ets2rpcmkii.ini` is missing, unreadable or
  deleted while the game runs, the defaults baked into the DLL take over
  instantly and the file is regenerated, so the presence always comes up.
  Discord handshake results (READY or a rejection with the reason) are now
  logged too. Covered by an automated test that runs on every build.
- **Ships pre-configured**: every build drops the DLL *and* a working
  `ets2rpcmkii.ini` next to it. No blank first run.
- **`templates/` style presets** included with each build:

  | Preset | Vibe |
  |---|---|
  | `templates/ets2rpcmkii.ini` | balanced default, truck and location style |
  | `templates/presets/distance.ini` | journey focused: km left, ETA, elapsed time |
  | `templates/presets/location.ini` | location focused: country flag badge, city names |
  | `templates/presets/most_info.ini` | everything: cargo, weight, fuel, damage, income |

- **Get on Steam button** on every preset, plus an optional second button.
- **New tokens**: `{city}`, `{distance_remaining}`, `{game_version}`,
  `{country_emoji}`.
- Renamed from SwiftDrive to **ETS2rpcMKII** (DLL: `ets2rpcmkii.dll`,
  config: `ets2rpcmkii.ini`).

## Why it is different

| | ETS2rpcMKII | Typical RPC tool |
|---|---|---|
| Install | **one DLL + one ini** | exe + DLLs + SDK runtimes |
| Configure | **ini, hot-reloaded in-game** | rebuild or restart |
| Presets | **3 style presets shipped** | fixed templates |
| Text | **handcraft every line** | fixed templates |
| Buttons | **Get on Steam + one custom, per state** | rarely |
| Countries | **flag badge or flag emoji of your current country** | no |
| Crash safety | **SEH guards: internal faults never take the game down** | hope |
| States | **13 incl. speeding, tollgate, delivered+income** | 3 to 5 |

## Quick start

1. **Get the DLL**: download from [Actions artifacts](../../actions)
   (every push builds a ready bundle: DLL + ini + presets), or build:

   - **No CMake?** Just run **`build_msvc.bat`** (double-click). It uses
     Visual Studio Build Tools directly and outputs `build/ets2rpcmkii.dll`.
   - Or the classic CMake way:

   ```bash
   cmake -S . -B build -A x64
   cmake --build build --config Release
   ```

2. **Install** into `Steam/steamapps/common/Euro Truck Simulator 2/bin/win_x64/plugins/`
   (CMake copies the DLL + `ets2rpcmkii.ini` + `templates/` presets there
   automatically if that is where your game lives; `-DPLUGINS_DIR="..."`
   overrides). The ini is pre-given, you only edit it if you want to.

3. **Run**: start the **Discord desktop client**, then ETS2.
   `game.log.txt` shows `[ETS2rpcMKII] Discord pipe connected.`

### Switching presets

Copy any `templates/presets/*.ini` over `ets2rpcmkii.ini` next to the DLL
and save. The presence changes within seconds, no restart. Edit further to
taste; delete the file to regenerate defaults.

## Configuration in one minute

Hot-reloaded: save, and the presence updates within seconds. The full
reference lives in [DOCS.md](DOCS.md); the short version:

```ini
[template]
button1_label = Get on Steam
button1_url   = https://store.steampowered.com/app/227300/Euro_Truck_Simulator_2/

[template.delivery_active]
state        = {mass} - {distance} {distance_unit} to {dest}
details      = {truck} - {speed} {speed_unit}
```

Three rules: an absent line keeps the smart default, `n/a` clears a field,
anything else is your text with tokens filled. Unknown tokens vanish, so
typos can never garble output.

Images are fixed since v4.2: the big image is always the official ETS2
game icon, the small badge always the truck brand logo (generic
`generic` art for modded trucks). Image lines in the ini are
ignored on purpose, so a broken art key can never ghost a mystery text
line onto the profile.

### Buttons and Discord facts, verified

- **You cannot see your own buttons.** Other people see them. That is a
  Discord limitation, not a plugin bug.
- Max **two buttons**, label up to 32 chars, url up to 512, http(s) only.
- There is **no official SCS Software or ETS2 Discord server or page** to
  route presence to. SCS communicate via their forum and blog. The largest
  community hubs are **TruckersMP** (`discord.gg/truckersmp`) and
  **TruckSim** (`discord.gg/trucksim`); put whichever invite you like in
  `button2_url`.
- **The big game modal** (the card with IGDB metadata, reviews and
  Add to Profile) is Discord-side metadata. It cannot be triggered by a
  custom Rich Presence, only by link embeds or a verified publisher claim.
- Rich Presence always displays under **your** application id. A presence
  cannot masquerade as another app's official page.

### Country support

The `{country}`, `{country_code}` and `{country_emoji}` tokens resolve
from city names via a built-in table of about 90 ETS2 cities plus your
own `[countries]` mappings, so your text lines can always name where
you are driving.

### Brands and mods

Built-in logo badges for the 7 officially licensed ETS2 brands: scania,
volvo, daf, man, mercedes, renault, iveco. ATS brands (ford, mack,
kenworth, peterbilt), modded trucks and anything unknown automatically
get the generic `generic` lorry pictogram, so the badge always shows
something.

## Art assets

Text-only presence works with zero setup. For images:

```bash
bash tools/download_assets.sh
```

fetches the official ETS2 app icon, the 7 licensed ETS2 brand logos and
a generic truck pictogram into `assets/portal/`, all background-free.
Upload them in the Developer Portal under your application, Rich
Presence, Art Assets.
The asset NAME is the file name without .png (ets2.png uploads as ets2);
the name field takes letters, numbers, underscores and hyphens only.

## Crash safety

- Every SCS entry point and the frame/event hooks run under **SEH guards**.
  An internal fault is logged and swallowed, the game keeps running
  (MinGW builds use direct calls).
- Discord gone, quit, or never installed? Non-fatal, retried every 15 s.
- Malformed ini? BOM handled, oversized files rejected, bad lines ignored,
  every value defaulted.
- Oversized or garbled IPC frames are capped and dropped; text is clamped
  to Discord's 128 byte limit without splitting UTF-8.

## Build and CI

Windows, CMake 3.16 or newer, VS 2022 Build Tools (Desktop C++) or MSYS2
MinGW-w64, or simply `build_msvc.bat`. Every push builds on GitHub Actions
and uploads a ready bundle (DLL + pre-given ini + preset templates), see
the [Actions tab](../../actions).

| Path | Role |
|---|---|
| `src/plugin.cpp` | telemetry, states, presence assembly |
| `src/discord_ipc.*` | small Win32 named-pipe Discord RPC client |
| `src/config.*` | ini parser, hot reload, template engine |
| `templates/ets2rpcmkii.ini` | pre-given default config, shipped with builds |
| `templates/presets/` | distance, location, most-info style presets |
| `include/scs/` | vendored SCS Telemetry SDK headers |
| `tools/download_assets.sh` | art and flag fetcher for the portal upload |
| `DOCS.md` | the full manual |

## Version history

The version lives in one place: `src/plugin_version.h` (`ETS2RPCMKII_VERSION`).
The plugin prints it to `game.log.txt` on every launch, so the log always
tells you exactly which build is running.

| Version | Name | What changed |
|---|---|---|
| **4.2.0** (current) | ETS2rpcMKII | Images are now fixed and no longer configurable: the big image is always the official ETS2 game icon, the small badge always the truck brand logo, with a generic `generic` pictogram for modded and unknown trucks. Image fields in the ini are ignored, so a broken art key can never ghost a mystery text line onto the profile. The `[brands]` mapping section is retired (it needed art uploads that custom URLs cannot provide) and the country flag badge is removed; `{country}` text tokens still work everywhere. Discord validation errors and handshake results are logged. |
| 4.1.0 | ETS2rpcMKII | Rebuilt on the real official SCS Telemetry SDK headers (API 1.00, game version 1.18). Job data (cities, cargo, income, distance) now comes from the configuration event, which is where the real SDK delivers it. Safe attribute parsing with the correct `scs_named_value_t` layout. `truck.fuel` handled as liters with the ratio computed from capacity. Fines read `fine.amount` (s64), toll event is `player.tollgate.paid`. Startup logging on every step: init banner with API version, init complete, job data received, Discord connect. Registration failures are logged instead of swallowed. Ini failsafe: missing or deleted ini regenerates the baked-in defaults, covered by an automated test that runs on every build. |
| 4.0.0 | ETS2rpcMKII | Renamed from SwiftDrive. DLL is `ets2rpcmkii.dll`, config is `ets2rpcmkii.ini`. Ships a pre-given ini with every build plus a `templates/` folder of style presets (distance, location, most_info). Get on Steam button on every preset. New tokens: `{city}`, `{distance_remaining}`, `{game_version}`, `{country_emoji}` (works even with `country_mode = 0`). Build script `build_msvc.bat` for compiling without CMake. |
| 3.0.0 | SwiftDrive RPC | Handcrafted field templates: every presence field (state, details, images, tooltips, two buttons) settable per state with `n/a` to clear. Country flag badge mode with about 90 built-in city mappings. 13 presence states including speeding, tollgate, cargo and truck damage. SEH crash guards. Metric and imperial units. |
| 2.x | SwiftDrive | Legacy `[templates]` text wrapper. Still parsed for compatibility today (see the v2 compat section in the config). |
| 1.x | SwiftDrive | Initial telemetry to Discord presence over the raw named pipe. |

Compatibility notes:

- The ini parser still understands the old `swiftdrive.ini` style
  `[templates]` section, but the current file name is `ets2rpcmkii.ini`.
- Presets and the shipped ini are plain ini files; any field you do not
  set falls back to the built-in smart defaults, so old configs keep working.

## License

MIT, see [LICENSE](LICENSE).
