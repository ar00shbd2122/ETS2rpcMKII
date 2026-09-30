# ETS2rpcMKII

[![build](https://github.com/ar00shbd2122/ETS2rpcMKII/actions/workflows/build.yml/badge.svg)](https://github.com/ar00shbd2122/ETS2rpcMKII/actions/workflows/build.yml)

Discord Rich Presence for Euro Truck Simulator 2, from live game telemetry.
One DLL, one ini. No Discord SDK, no bundled runtimes, no background services,
no accounts. What you are doing in the game shows up on your Discord profile,
and every line of it is yours to rewrite while the game is running.

> ## Honest disclosure: this runs on Discord's legacy Rich Presence tech
>
> The plugin speaks the same local `discord-ipc` protocol that Discord's old
> Game SDK used. Discord has marked that SDK *legacy* and stopped maintaining
> it in favor of the new Social SDK, which is built for registered game
> developers. Using it for ETS2 would mean registering ourselves as SCS
> Software's game - impersonating another studio is not something this project
> will ever do, out of respect for SCS.
>
> Today the Discord desktop client still honors the legacy pipe, and the plugin
> works. If Discord removes it one day, this project dies with the tech rather
> than pretending to be someone else's verified game. Use it knowing that.

Current release: **v5.0.6**. History at the bottom.

## What it looks like

```
Euro Truck Simulator 2
Close to Vyborg (RU) ・ 62% done
12 t of Construction materials ・ 148 km ・ ETA 17:45
 [Get on Steam]
```

Big image is the official round ETS2 icon. The small badge is your truck
brand's logo. Both are fixed on purpose; every piece of *text* is yours.

## Why this one and not the other RPC tools

Most ETS2 Rich Presence tools are a Python script plus a telemetry server,
or a fixed-template DLL. This is a single SCS telemetry plugin that talks to
Discord directly and treats configuration as a first-class feature:

- **Hot-reload everything.** Save `ets2rpcmkii.ini` and the presence changes
  within a couple of seconds, mid-drive. No restart, no rebuild.
- **You design the two text lines**, per state, with live tokens
  (`{cargo}`, `{dest}`, `{eta_clock}`, `{country_tag}`, ...). Unknown tokens
  vanish, so a typo cannot garble your profile.
- **Foolproof by design.** Delete the ini: defaults baked into the DLL take
  over and the file is regenerated. Type `evnt_hold` instead of `event_hold`:
  the log tells you. Write 9999 for a value capped at 600: the log tells you.
  A line can no longer end in bare `()` or a dangling "to" - a hygiene pass
  strips that class of garbage from every line.
- **Crash-proof.** Every SCS entry point runs under SEH guards; an internal
  fault is logged and swallowed, the game never goes down with us. Telemetry
  floats are NaN/inf guarded so a misbehaving mod cannot print "nan km/h".
- **Zero network.** The only thing it talks to is the local Discord pipe.

## States

Thirteen of them, with priority: `delivery_complete`, `tollgate`, `got_fine`,
`paused`, `on_ferry`, `on_train`, `speeding`, `cargo_damaged`,
`truck_damaged`, then the normal three (`delivery_active`, `resting`,
`free_roam`, `main_menu`).

Ferry and train show the real crossing route (`On a ferry → Rostock`),
not the job destination, and are held for `event_hold` seconds - the game
never sends an "arrived" event, so the state expires on its own.

## Configuration in one minute

```ini
[template]
button1_label = Get on Steam
button1_url   = https://store.steampowered.com/app/227300/Euro_Truck_Simulator_2/

[template.delivery_active]
state        = {src}{src_tag} → {dest}{dest_tag}{driven_tag}
details      = {cargo} ({mass}) ・ {distance} {distance_unit} ・ ETA {eta_clock}
```

Three rules: a line you don't set keeps the smart default, `n/a` clears a
field, anything else is your text with tokens filled.

**Buttons:** max two, and you can never see your own - other people can.
That is a Discord limitation, not a bug.

Full manual, every token, every state: [DOCS.md](DOCS.md).
Style presets in `templates/presets/` (distance, location, most-info):
copy one over the ini and edit to taste.

## Countries, in any game language

The game reports city names in your UI language. On a Russian client that is
Cyrillic (Выборг), on German it is Köln. The plugin folds every name through
a Unicode normalizer - accents stripped, Cyrillic transliterated - and matches
it against a built-in table of about 170 cities covering the base map plus
Russia, the Baltics, Belarus, Black Sea, West Balkans and Greece. Your own
`[countries]` mappings are tried first, so mod-map cities work too. If nothing
matches, the country tokens are simply empty - never a bare parenthesis.

## Brands

Badges for the 7 officially licensed ETS2 brands: scania, volvo, daf, man,
mercedes, renault, iveco. ATS brands and modded trucks get a generic lorry
pictogram, so the badge always shows something.

On **your own** Discord application (any id different from the built-in one)
you can additionally map brand substrings and country flags to your own
uploaded art. On the shared built-in application the art set is locked,
because nobody but its owner can extend it.

## Install

1. Drop `ets2rpcmkii.dll` and `ets2rpcmkii.ini` into
   `.../Euro Truck Simulator 2/bin/win_x64/plugins/` (create it if missing).
2. Start the **Discord desktop client**, then the game.
3. Check `Documents/Euro Truck Simulator 2/game.log.txt`. A healthy session:

   ```
   [ETS2rpcMKII] v5.0.0 initialising (telemetry API 1.0).
   [ETS2rpcMKII] Discord pipe opened, handshake sent.
   [ETS2rpcMKII] Discord handshake accepted (READY). Presence updates are live.
   [ETS2rpcMKII] Initialisation complete.
   ```

Every release also ships a zip with exactly those two files.

### Build it yourself

`build_msvc.bat` (Visual Studio Build Tools, Desktop C++) or CMake.
Every push builds on GitHub Actions and uploads a ready bundle.
The ini failsafe + schema suite (37 checks) runs on every build.

## Crash safety and failsafes, exhaustively

- Missing, empty, unreadable or deleted ini → baked-in defaults, file
  regenerated, all logged. Covered by tests.
- Garbage ini (BOM, binary junk, no equals signs) → parser survives, every
  lookup defaults. Covered by tests.
- Typos and out-of-range values → named in the log (`ini: ...`), the rest of
  the file stays live. Covered by tests.
- Discord down, quits, or rejects a presence → non-fatal, retried every 15 s,
  rejection reason logged once per connect.
- Identical presence pushes are suppressed (no flicker for watchers); a
  keepalive re-send every 60 s detects half-dead pipes.
- NaN or infinite telemetry values from a broken mod → clamped, never printed.
- Any internal fault → SEH-caught, logged with a rate limit, session summary
  (minutes, push count, recovered faults) written to the log on shutdown.

## Compatibility

| | |
|---|---|
| Game | ETS2 (Steam, Windows x64), tested on 1.61.1.1s |
| Telemetry API | SCS 1.00 (headers from the 1.61 era; works back to ~1.37) |
| Discord | desktop client, legacy `discord-ipc` pipe |

## Version history

| Version | What changed |
|---|---|
| **5.0.6** (current) | Simplicity release. The job-percentage machinery is gone: it needed a snapshot of the job's distance at start (the game never reports it), which reset to 0% whenever you reloaded a save mid-job, and fixing that properly wanted a state file on disk. In its place: a session odometer. The new `{driven}` and `{driven_tag}` tokens show how far you have driven this session (`・ 128 km driven`), computed as speed x time - no file, no baseline, nothing to reset. Still one DLL plus an optional ini. |
| 5.0.5 | Progress survives a restart via a persisted baseline file (superseded by 5.0.6, which removes the file again). |
| 5.0.4 | Ferry/train mixup fixed: an off-by-one in the event id check sorted every crossing into the train branch, so ferries rode as "Rail freight". Route lines now carry per-city country codes: `Pori (FIN) → Oslo (NOR)` via the new `{src_tag}`, `{dest_tag}` and `{ferry_tag}` tokens. Delivery-complete cards keep their city names again (the v5.0.3 snapshot fallback was overwritten before it could be used). Discord offline mode: three quick retries, then a quiet probe every 30 s instead of hammering, one log line for the outage, and status resumes the moment Discord answers. |
| 5.0.3 | Snapshot hygiene. `{distance}` no longer fakes "0 km" with no route, `{cargo}`/`{mass}` stay empty without a job, `{wear}` shows chassis wear (was cargo damage), paused/resting/fine defaults got honest static lines, and the shipped template matched the baked-in defaults again (a .gitignore rule had been eating it). |
| **5.0.2** | Honesty release. The "Close to {city}" line was never your current road - {city} is the job's source/destination, so it pretended to know where you are. Default delivery and paused lines now show the plain route: `{src} → {dest}`. Pausing in free-roam no longer prints "(0 kg)" with a nameless cargo: job-bound tokens stay empty when there is no job, and tidy_line removes the empty parens. |
| 5.0.1 | Job-progress fix: several 1.6x job configurations never send `planned_distance_km`, which left `{job_progress}` permanently empty and the template's static `% done` suffix dangling on the profile. The remaining distance at job start is now snapshotted as the baseline, the percentage always has a real denominator, and the new composite `{progress_tag}` token renders `・ 62% done` only when the percentage exists - never a bare suffix. |
| 5.0.0 | The foolproof release. Ini schema report: typos (`evnt_hold`), unknown template states and out-of-range values are named in game.log.txt instead of being silently ignored. Output hygiene: no line can render bare `()`, stray arrows or hanging words, whatever the ini says. Ferry/train rebuilt: timed hold plus the real crossing route from event attributes via new `{ferry_from}`/`{ferry_to}` tokens. Country detection: Unicode city folding (Cyrillic + accents) and ~170-city table incl. Russia, Baltics, Black Sea, Balkans, Greece. New tokens `{eta_clock}`, `{job_progress}`, `{fuel_l}`, `{jobs_done}`, `{state_name}`. Discord: identical-push suppression and a 60 s keepalive. Telemetry floats NaN/inf-guarded. Fault logging rate-limited; shutdown writes a session summary. Hot-reload stat throttled to 1/s. |
| 4.2.1 | Bare-paren fix in location lines; Cyrillic/accented city matching; city table grown to ~170 cities. |
| 4.2.0 | Images locked to the known-good set (official icon + 7 brand logos + generic); image fields in the ini ignored so a broken key can never ghost a mystery text line. |
| 4.1.0 | Real official SCS SDK headers; ini failsafe with automated tests; verbose startup logging. |
| 4.0.0 | Renamed from SwiftDrive; shipped ini + presets; Get on Steam button; build_msvc.bat. |
| 3.0.0 | Handcrafted per-field, per-state templates; 13 states; SEH guards. |
| 2.x / 1.x | SwiftDrive era. |

## License

MIT, see [LICENSE](LICENSE).
