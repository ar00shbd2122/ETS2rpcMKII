# ETS2rpcMKII documentation

The full manual: how the plugin thinks, what every state does, what
every token means, and how to code your own presence style.
No prior knowledge assumed. Everything here also applies to the
shipped presets, which are just plain ini files you can copy from.

## Contents

1. [How it works](#how-it-works)
2. [Install in 60 seconds](#install-in-60-seconds)
3. [How a presence is assembled](#how-a-presence-is-assembled)
4. [The three template layers](#the-three-template-layers)
5. [Fields you can set](#fields-you-can-set)
6. [All 13 states](#all-13-states)
7. [All tokens](#all-tokens)
8. [The rules: absent, n/a, custom](#the-rules-absent-na-custom)
9. [Settings reference](#settings-reference)
10. [Images and art assets](#images-and-art-assets)
11. [Buttons](#buttons)
12. [Worked examples](#worked-examples)
13. [Troubleshooting](#troubleshooting)

## How it works

The DLL is loaded by the game itself (SCS plugin API). It reads live
telemetry (speed, cargo, position, fuel and so on), reduces it to one
of 13 states, builds a Discord presence from your ini templates, and
pushes it to the running Discord desktop client over its local pipe.
No Discord SDK, no extra DLLs, no network.

The ini is hot-reloaded: save it and the presence changes within a
couple of seconds, even mid-drive.

## Install in 60 seconds

1. Copy `ets2rpcmkii.dll` into
   `Steam/steamapps/common/Euro Truck Simulator 2/bin/win_x64/plugins/`
   (create the `plugins` folder if it does not exist).
2. Copy `ets2rpcmkii.ini` next to the DLL (same folder). This step is
   optional: if the ini is missing the plugin falls back to the defaults
   baked into the DLL and writes a fresh editable file for you.
3. Start the Discord desktop client, then start ETS2.
4. Check `game.log.txt` in `Documents/Euro Truck Simulator 2` for the
   startup lines. A healthy session looks like this:

   ```
   [ETS2rpcMKII] v4.1.0 initialising (telemetry API 1.0).
   [ETS2rpcMKII] Discord pipe opened, handshake sent.
   [ETS2rpcMKII] Discord handshake accepted (READY). Presence updates are live.
   [ETS2rpcMKII] Initialisation complete.
   ```

Building from source: run `build_msvc.bat` (Visual Studio Build Tools)
or use CMake. See README.md.

## How a presence is assembled

Every update, the plugin:

1. Resolves the current state (see state list below).
2. Starts with the built-in smart defaults for that state.
3. Applies your `[template]` layer on top (only fields you set).
4. Applies your `[template.<state>]` layer on top of that.
5. Fills tokens (`{speed}` and friends) with live values.
6. Clamps text to Discord's limits (128 bytes per line, UTF-8 safe).
7. Sends it to Discord.

You only ever touch step 3 and 4. Anything you do not set stays at the
smart default, so an empty ini still gives a complete presence.

## The three template layers

```ini
[template]                  ; layer 1: applies to EVERY state
button1_label = Get on Steam

[template.delivery_active]  ; layer 2: one state only
state = Hauling {cargo} to {dest}

[template.free_roam]        ; layer 3: another state, same idea
details = Near {city} {country_emoji}
```

A field set in a state section beats the same field in `[template]`.
A field set nowhere uses the built-in default.

Valid state keys for sections:

`main_menu`, `free_roam`, `delivery_active`, `delivery_complete`,
`paused`, `on_ferry`, `on_train`, `resting`, `got_fine`, `tollgate`,
`cargo_damaged`, `truck_damaged`, `speeding`

## Fields you can set

Every template section accepts these six fields:

| Field | Meaning | Discord limit |
|---|---|---|
| `state` | first text line | 128 bytes |
| `details` | second text line | 128 bytes |
| `button1_label` | first button text | 32 chars |
| `button1_url` | first button link (http/https only) | 512 chars |
| `button2_label` | second button text | 32 chars |
| `button2_url` | second button link | 512 chars |

Images are deliberately NOT in this list. Since v4.2 the big image is
always the official game icon and the small badge always the truck
brand logo (the generic `generic` art for modded and unknown
trucks). `large_image`, `large_text`, `small_image` and `small_text`
lines in the ini are ignored, so a broken art key can never leak a
mystery text line onto your profile.

There is no timestamp field; the elapsed timer is controlled by the
`show_time` toggle in `[presence]` and starts when the game starts.

## All 13 states

What triggers each state, and its built-in default text:

| Key | Trigger | Default state / details |
|---|---|---|
| `main_menu` | engine off, no job | Planning the next haul / In main menu |
| `free_roam` | driving without a job | Exploring Europe / Driving a {truck} - {speed} |
| `delivery_active` | job in progress, engine on | En route to {dest} - {distance} left, ETA {eta} min / Delivering {cargo} ({mass}) |
| `delivery_complete` | for `event_hold` seconds after dropping cargo | Delivered to {dest} / Job complete - earned {income} |
| `paused` | game paused | Game paused / Game paused |
| `on_ferry` | ferry event, held for `event_hold` seconds | On a ferry → {ferry_to} / Crossing from {ferry_from} |
| `on_train` | train event, held for `event_hold` seconds | On a train → {ferry_to} / Rail freight from {ferry_from} |
| `resting` | job active but engine off | Taking a break near {dest} / Resting - engine off |
| `got_fine` | for `event_hold` seconds after a fine | Fined - watch the road / Traffic fine received |
| `tollgate` | for ~10 seconds at a tollgate | Paying the toll / Passing a tollgate |
| `cargo_damaged` | cargo damage above threshold | En route to {dest} / Cargo damaged - {damage}% |
| `truck_damaged` | chassis wear above threshold | Find a garage / Truck needs repairs |
| `speeding` | speed over limit by `speeding_threshold_pct` | Speeding - {speed} in a {speed_limit} zone / Running {over_limit}% over the limit |

Priority order when several are true at once:
`delivery_complete`, then `tollgate`, then `got_fine`, then `paused`,
then `on_ferry`, then `on_train`, then `speeding`, then
`cargo_damaged`, then `truck_damaged`, then the normal three.

## All tokens

Use tokens anywhere in `state`, `details` and button labels. Unknown
tokens vanish silently, so a typo can never garble your profile.

### Job and route

| Token | Value | Empty when |
|---|---|---|
| `{cargo}` | cargo name, e.g. Construction materials | no job |
| `{mass}` | formatted weight, e.g. 24 t | no job |
| `{dest}` | destination city | no job |
| `{src}` | source city | no job |
| `{src_tag}` `{dest_tag}` | ready-to-append ISO code per city, ` (FIN)`, e.g. `Pori (FIN)` | city unknown |
| `{company}` | destination company, else source | unknown |
| `{distance}` | remaining distance, rounded | no route |
| `{distance_remaining}` | same value as `{distance}` (alias) | no route |
| `{distance_unit}` | km or mi | never |
| `{eta}` | minutes of driving left | no route |
| `{eta_clock}` | arrival as wall-clock time, e.g. 17:45 | no route |
| `{job_progress}` | route completion percentage, 0 to 100 | unknown baseline |
| `{progress_tag}` | ready-to-append `・ 62% done`, empty when the percentage is unknown, so a line never ends in a bare `% done` | unknown baseline |
| `{income}` | job pay with thousands separators | no job |

### Driving

| Token | Value |
|---|---|
| `{speed}` | current speed, rounded |
| `{speed_unit}` | km/h or mph |
| `{speed_limit}` | current limit with unit, e.g. 90 km/h |
| `{over_limit}` | percent over the limit, only while speeding |
| `{fuel}` | fuel percent, 0 to 100 |
| `{damage}` | cargo damage percent |

### Truck

| Token | Value |
|---|---|
| `{truck}` | brand plus model, e.g. Scania S |
| `{brand}` | brand id, e.g. scania |
| `{model}` | model id, e.g. s_2016 |
| `{game_version}` | game version number from the SDK |

### Place and time

| Token | Value | Empty when |
|---|---|---|
| `{city}` | nearest meaningful city (destination, else source) | unknown |
| `{country}` | country name, e.g. Germany | city unknown |
| `{country_code}` | two letter code, uppercase, e.g. RU | city unknown |
| `{country_tag}` | ready-to-append tag, ` (RU)`, empty when unknown, so lines never end in bare parentheses | city unknown |
| `{country_emoji}` | the flag as a real emoji | city unknown |
| `{ferry_from}` | ferry/train crossing origin, e.g. Ostersund | not on a ferry/train |
| `{ferry_to}` | ferry/train crossing destination | not on a ferry/train |
| `{ferry_tag}` | ready-to-append ISO code for the crossing destination, ` (FIN)` | not on a ferry/train |
| `{time}` | local clock, format from `use_24h` | never |
| `{newline}` | line break (Discord shows one line per field, so mostly useful in tooltips) | never |
| `{state}` `{details}` | the current default text (useful in `[template]` to decorate defaults) | never |

Country resolution: a built-in table of roughly 170 ETS2 cities (base map
plus Russia, Baltics, Black Sea, West Balkans and Greece), and any extra
`[countries]` mappings you add. City names are matched through a Unicode
folder, so a Russian client reporting Cyrillic city names (Выборг) still
resolves correctly. If nothing matches, the country tokens are empty and
`{city}` still shows the raw city name.

Progress across restarts: `{job_progress}` is measured against the
remaining distance the plugin saw when the job started. That baseline
is persisted in a small `ets2rpcmkii.job` file next to the ini, so
quitting the game and loading a save mid-job continues the percentage
instead of restarting at 0%. The file is keyed by cargo and route (a
different job never inherits a stale baseline), refreshed every 30 s,
and deleted when the job is delivered or cancelled.

Per-city route tags: `{src_tag}` and `{dest_tag}` resolve each city
independently through the same table, so a route can read
`Pori (FIN) → Oslo (NOR)` even while the location you are driving
through is unknown. The codes are the ISO 3166-1 three-letter forms
(FIN, NOR, DEU, RUS, ...); cities off the map render nothing, never
bare parentheses.

After token fill every line is tidied automatically: empty `()` from
vanished tokens is removed, stray separators, arrows and hanging words
like a trailing `to` are trimmed. A template can no longer render that
garbage even by accident.

## The rules: absent, n/a, custom

Three states for every field, in every section:

1. **Line absent** (or starts with `;`): keep the default from the
   layer below, ultimately the built-in smart default.
2. **`n/a`** (also accepted: `na`, `none`, `-`): clear the field. The
   line will be empty and the button will not render.
3. **Anything else**: your text, with tokens filled. Used verbatim.

Example:

```ini
[template.main_menu]
state        = Browsing the job market
;details     =          ; absent, so the default details stay
```

## Settings reference

### [discord]

| Key | Default | Meaning |
|---|---|---|
| `application_id` | built-in | your Discord application id (digits only) |

### [presence]

| Key | Default | Meaning |
|---|---|---|
| `show_speed` | 1 | speed in default texts |
| `show_fuel` | 1 | fuel percent in default texts |
| `show_time` | 1 | elapsed timer on the profile |
| `show_truck_badge` | 1 | small brand-logo badge on the profile |

### [behaviour]

| Key | Default | Meaning |
|---|---|---|
| `units` | metric | or `imperial` (mi, mph, short tons) |
| `update_interval` | 5 | seconds between pushes, minimum 3 |
| `event_hold` | 20 | seconds that fines, tollgates and deliveries stay visible |
| `use_24h` | 1 | 24h clock for `{time}` |
| `speeding_threshold_pct` | 15 | speeding state at this percent over the limit |
| `cargo_damage_threshold` | 45 | cargo damaged state at this percent |
| `chassis_wear_threshold` | 65 | truck damaged state at this percent |

### [countries]

Any key is a city substring mapped to a country name for the
`{country}` tokens, e.g. `dresden = Germany`. The built-in table
already covers roughly 90 ETS2 cities, so this section is only for
gaps and mod map cities.

## Images and art assets

An image field is an asset key, not a file. Keys must match art
uploaded in the Discord Developer Portal under your application,
Rich Presence, Art Assets. Keys are lowercase, 32 characters max,
and may only contain letters, numbers, underscores and hyphens:
no slashes, no dots, no spaces. The upload name is the file name
without the .png extension: ets2.png uploads as ets2. If the portal
shows "String value did not match validation regex", the name field
still contains a slash or a dot.

Out of the box there are exactly these keys, and they are fixed:

- `ets2` for the big image: the official round ETS2 app icon
- brand keys for the corner badge, the 7 officially licensed ETS2
  brands only: `scania`, `volvo`, `daf`, `man`, `mercedes`, `renault`,
  `iveco`
- everything else, including ATS brands (ford, mack, kenworth,
  peterbilt) and modded trucks, shows `generic`: the lorry pictogram

## Custom art on your own application

The application id shipped with the repo serves a locked art set that
nobody but the owner can extend. To map your own brand logos and
country flags you must run your OWN Discord application. The DLL
checks the id: same as the repo id (or missing) means custom mappings
in `[brands]` and `[countries]` are ignored; any other id unlocks them.

### Step by step

1. Create an application at discord.com/developers/applications
   (New Application, give it any name, save).
2. Copy the Application ID from the General Information page.
3. Open Rich Presence, Art Assets and upload, with these exact names:

   | Asset name | What | Required |
   |---|---|---|
   | `ets2` | official round ETS2 icon | yes: without it the big image is not sent |
   | `generic` | fallback logo for unknown and modded trucks | yes for the badge to always show |
   | `generic_c` | fallback art for unmapped countries | optional |
   | your brand keys | e.g. `renault_trucks`, `my_mod_logo` | optional |
   | your flag keys | e.g. `flag_de`, circular images | optional |

4. Put your application id in `ets2rpcmkii.ini`:

   ```ini
   [discord]
   application_id = your_own_id_here
   ```

5. Map your art:

   ```ini
   [brands]
   my_mod_truck = my_mod_logo      ; substring of the brand id
   renault      = renault_trucks   ; overrides the built-in logo

   [countries]
   dresden = flag_de               ; city match -> flag art + (DE) text
   ```

6. Restart the game (the ini hot-reloads, but art checks happen per
   push and a restart is the clean test).

### The failsafe chain, enforced by the DLL

- An ini value is only sent if it is a valid asset key: 1 to 32
  characters from a-z, 0-9, underscore, hyphen. `Flag DE` (space,
  uppercase), `flag/de` (slash) are rejected before anything is sent.
- Unknown truck: `generic`. Known country with no flag art mapped:
  `generic_c` (own app only). Nothing else is invented.
- If a core key (`ets2`, `generic`, `generic_c`) is missing from your
  portal, that image is simply not sent. Text and buttons keep working.
  A missing key can never ghost a mystery text line onto the profile.
- On the shared repo application custom mappings are never consulted,
  so the locked art set is the whole truth there.

`bash tools/download_assets.sh` fetches all of the above into
`assets/portal/` ready for upload: backgrounds stripped, padded to a
square, and sized into the portal's 512x512 to 1024x1024 window.
No art uploaded yet? Text-only presence works fine; empty image
fields are simply skipped.

Why images are not configurable: a small image key that has no art
behind it does not just vanish. Discord renders the badge tooltip as
a plain text line instead, which looks like a random third
description. Locking the images to the known-good set makes that
impossible, and every truck still gets a badge through `generic`.

## Buttons

```ini
[template]
button1_label = Get on Steam
button1_url   = https://store.steampowered.com/app/227300/Euro_Truck_Simulator_2/
button2_label = Join my Convoy
button2_url   = https://discord.gg/truckersmp
```

Facts, verified against current Discord behaviour:

- Maximum two buttons, both must have label and http(s) url.
- You cannot see your own buttons. Other people can. This is a Discord
  limitation, not a bug.
- Button labels are plain text. Links open in the browser.

## Worked examples

### Minimal: keep defaults, add the Steam button

```ini
[template]
button1_label = Get on Steam
button1_url   = https://store.steampowered.com/app/227300/Euro_Truck_Simulator_2/
```

### Your truck and the clock everywhere

```ini
[template.delivery_active]
state        = {truck} - {time}
details      = Heading to {dest}

[template.free_roam]
state        = {truck} - {country} - {time}
details      = Just driving
```

### The shipped default style

```ini
[template.delivery_active]
state        = {mass} ・ {distance} {distance_unit} to {dest}
details      = {truck} ・ {speed} {speed_unit}

[template.free_roam]
state        = Cruising through {country}
details      = {truck} ・ {speed} {speed_unit}
```

(The shipped file uses a middle dot as the separator; any character
you like works, for example `|` or `-`.)

### Roleplay style

```ini
[template.delivery_active]
state        = Hauling {mass} of {cargo}
details      = {distance} {distance_unit} to {dest}, arriving around {eta} min
small_image  = {country_flag}
small_text   = Currently in {country}

[template.got_fine]
state        = Pulled over
details      = Wallet lighter by {fine}
```

### Clean presence: nothing but two lines

```ini
[template]
large_image  = n/a
large_text   = n/a
small_image  = n/a
small_text   = n/a
button1_label = n/a
button1_url   = n/a
button2_label = n/a
button2_url   = n/a

[template.delivery_active]
state   = {cargo} to {dest}
details = {distance} {distance_unit} left
```

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Nothing on my profile | Discord desktop client must run before or with the game. Check `game.log.txt` for the connect line. |
| Log says `Discord handshake accepted` but no presence shows | The handshake was fine, so check whether a second RPC tool is fighting over the slot (for example an old telemetry server plugin with its own Discord output). |
| Log says `Discord rejected the presence: ...` | Discord answered with an error. If the message mentions the client id, the `application_id` in `ets2rpcmkii.ini` is dead: create your own application at discord.com/developers/applications and put its id in the ini. |
| A third text line appears under my details | That is the small badge tooltip leaking: the brand logo for your truck is not uploaded yet. Run `tools/download_assets.sh` and upload everything in `assets/portal/` to the Developer Portal. Since v4.2 the badge itself can no longer be misconfigured from the ini. |
| Profile still shows the OLD image after I re-uploaded an asset | Discord caches presence art per session. Quit Discord fully (tray icon, Quit - closing the window is not enough), start it again, then restart the game. If it still shows the old art, delete the asset in the portal and upload the file fresh under the same name. |
| My buttons are missing | You cannot see your own buttons. Ask a friend to check. |
| A line shows raw `{tokenn}` | Typos vanish, so a raw token means the token name is wrong but unknown tokens never print. If you truly see braces, the line came from an old cached presence: save the ini again. |
| Country tokens are empty | The city was not matched. The built-in table covers ~170 cities including Russia and the Baltics; add a mapping in `[countries]`, e.g. `myhometown = Germany`, for anything else. |
| Emoji flag shows as a box | Your platform font lacks flag glyphs. Discord on desktop and mobile renders them fine; the game log does not matter. |
| Presence stuck on one state | Check the thresholds in `[behaviour]`. A very low `cargo_damage_threshold` pins the damaged state. |
| I broke everything | Delete `ets2rpcmkii.ini`. A fresh default is written on the next launch or reload. If you delete it while the game runs, the baked-in defaults take over instantly (the log says `ets2rpcmkii.ini disappeared`) and the file is regenerated. |
