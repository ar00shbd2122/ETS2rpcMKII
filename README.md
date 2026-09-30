<div align="center">

<img src="docs/assets/banner.svg" width="100%" alt="ETS2rpcMKII">

# ETS2rpcMKII

Discord Rich Presence for Euro Truck Simulator 2

[![Build](https://github.com/ar00shbd2122/ETS2rpcMKII/actions/workflows/build.yml/badge.svg)](../../actions)
![ETS2 1.61.1.1s](https://img.shields.io/badge/ETS2-Tested_on_1.61.1.1s-success?style=flat-square)
![Windows x64](https://img.shields.io/badge/Windows-x64-blue?style=flat-square)
![Discord Desktop](https://img.shields.io/badge/Discord-Desktop-5865F2?style=flat-square)
![License](https://img.shields.io/badge/License-MIT-lightgrey?style=flat-square)

### One DLL. One INI.

No Discord SDK • No telemetry servers • No launchers • No background services

**Drop it into ETS2. Start Discord. Drive.**

[Installation](#installation) •
[Documentation](DOCS.md) •
[Latest Release](../../releases/latest)

</div>

---

<p align="center">
  <img src="docs/assets/images/hero.png" width="900" alt="ETS2rpcMKII Preview">
</p>

---

## Contents

- [Why ETS2rpcMKII?](#why-ets2rpcmkii)
- [Installation](#installation)
- [Screenshots](#screenshots)
- [Configuration](#configuration)
- [Template Presets](#template-presets)
- [Community Templates](#community-templates)
- [Features](#features)
- [Activity States](#activity-states)
- [Discord Game SDK Note](#a-note-about-discords-legacy-game-sdk)
- [ATS Compatibility](#ats-compatibility)
- [Reliability](#reliability)
- [Documentation](#documentation)
- [Compatibility](#compatibility)
- [License](#license)

---

# Why ETS2rpcMKII?

Most ETS2 Rich Presence tools are built around:

- Python scripts
- Local telemetry servers
- Launchers
- Fixed templates
- Multiple dependencies

ETS2rpcMKII takes a different approach.

It is a native SCS telemetry plugin that talks directly to Discord and updates your Rich Presence using live game telemetry.

No local web server.

No extra software.

No accounts.

No internet traffic.

No dependency chain.

Just a plugin.

---

# Installation

## Step 1 — Download the latest release

Open the repository's Releases page and download the newest ZIP archive.

```text
ETS2rpcMKII-vX.X.X.zip
```

Extract the ZIP anywhere.

---

## Step 2 — Locate your plugins folder

Navigate to:

```text
Euro Truck Simulator 2
└── bin
    └── win_x64
        └── plugins
```

If the folder does not exist, create it.

---

## Step 3 — Choose your setup

### Minimal Setup (Recommended)

Copy only:

```text
ets2rpcmkii.dll
```

into:

```text
Euro Truck Simulator 2
└── bin
    └── win_x64
        └── plugins
```

That's it.

ETS2rpcMKII contains fully functional built-in defaults and can run entirely from a single DLL.

No INI file required.

No templates required.

---

### Customizable Setup

If you want to customize your Rich Presence:

Copy:

```text
ets2rpcmkii.dll
ets2rpcmkii.ini
```

into the same folder.

```text
Euro Truck Simulator 2
└── bin
    └── win_x64
        └── plugins
            ├── ets2rpcmkii.dll
            └── ets2rpcmkii.ini
```

The plugin automatically reloads configuration changes while the game is running.

No restart required.

---

## Step 4 — Start Discord

ETS2rpcMKII requires the Discord Desktop client.

Make sure Discord is running before launching the game.

---

## Step 5 — Start ETS2

Launch Euro Truck Simulator 2 normally.

Your Discord profile should begin updating automatically.

---

## Optional: Verify Installation

Open:

```text
Documents
└── Euro Truck Simulator 2
    └── game.log.txt
```

A healthy startup should contain messages similar to:

```text
[ETS2rpcMKII] Discord pipe opened, handshake sent.
[ETS2rpcMKII] Discord handshake accepted (READY).
[ETS2rpcMKII] Presence updates are live.
```

---

# Screenshots

<table>
<tr>
<td width="50%">

### Delivery

<img src="docs/assets/images/delivery.png">

</td>
<td width="50%">

### Ferry Crossing

<img src="docs/assets/images/ferry.png">

</td>
</tr>

<tr>
<td>

### Resting

<img src="docs/assets/images/resting.png">

</td>
<td>

### Speeding

<img src="docs/assets/images/speeding.png">

</td>
</tr>
</table>

---

# Configuration

Every activity state can be customized.

Example:

```ini
[template.delivery_active]

state   = {src}{src_tag} → {dest}{dest_tag}
details = {cargo} ({mass}) ・ {distance} km ・ ETA {eta_clock}
```

Save the file.

Keep driving.

The plugin automatically reloads your changes.

No restart required.

For a complete token reference, see DOCS.md.

---

# Template Presets

The files inside `templates/` are example configurations.

They are:

- Optional
- Not loaded automatically
- Not required for operation

To use one:

1. Open the preset.
2. Copy its contents.
3. Paste them into `ets2rpcmkii.ini`.
4. Save.

Or create your own layout from scratch.

---

# Community Templates

Have a layout you're proud of?

Open a pull request and add it to `templates/`.

The goal is for the folder to become a collection of community-made presets rather than only the layouts shipped with the project.

Minimal.

Information-dense.

Realistic.

Roleplay-focused.

Ridiculous.

If someone else might enjoy it, it's welcome.

---

# Features

- Native SCS telemetry plugin
- Live Discord Rich Presence
- Hot-reload configuration
- Fully customizable templates
- 13 automatic activity states
- Country detection
- Truck brand badges
- Ferry route detection
- Train route detection
- Automatic recovery and fail-safes
- Zero network traffic
- No external dependencies

---

# Activity States

| Driving | Events |
|----------|----------|
| Delivery | Tollgate |
| Free Roam | Fine |
| Resting | Speeding |
| Paused | Cargo Damage |
| Ferry | Truck Damage |
| Train | Delivery Complete |
| Main Menu | |

---

# A Note About Discord's Legacy Game SDK

> [!NOTE]
>
> ETS2rpcMKII communicates through the same local Discord interface used by Discord's legacy Game SDK.
>
> Discord has archived the Game SDK in favour of the newer Social SDK, which is intended for registered game developers.
>
> Using the Social SDK for ETS2 Rich Presence would require registering as Euro Truck Simulator 2's developer. ETS2 belongs to SCS Software, and this project will not attempt to impersonate another studio's game or application.
>
> Today, Discord Desktop still supports the legacy interface and the plugin works normally.
>
> If Discord removes support in the future, ETS2rpcMKII will stop working rather than pretending to be an official integration.

---

# ATS Compatibility

> [!NOTE]
>
> ETS2rpcMKII was developed and tested exclusively with Euro Truck Simulator 2.
>
> The underlying telemetry logic is expected to work in American Truck Simulator as well, since both games expose very similar telemetry systems.
>
> However, ATS has not been officially tested and is not currently a supported target.
>
> The shared Discord application bundled with ETS2rpcMKII only contains artwork for ETS2 truck brands:
>
> - Scania
> - Volvo
> - DAF
> - MAN
> - Mercedes-Benz
> - Renault
> - Iveco
> - Generic Truck
>
> ATS trucks will therefore most likely display the generic truck badge rather than their manufacturer logo.
>
> If you want proper ATS branding, you can configure your own Discord application and upload your own assets.
>
> Instructions are available in DOCS.md.
>
> Anyone is also welcome to fork the project and create a dedicated ATS version.

---

# Reliability

Designed around failure recovery.

- Missing INI files automatically fall back to built-in defaults
- Deleted INI files are regenerated
- Invalid configuration values are reported in `game.log.txt`
- Discord outages are handled automatically
- Duplicate presence updates are suppressed
- Telemetry anomalies are sanitized
- Internal faults are isolated from the game process

The game should never crash because the Rich Presence plugin did.

---

# Documentation

| File | Purpose |
|--------|----------|
| DOCS.md | Full configuration guide |
| templates/ | Example and community presets |
| CHANGELOG.md | Release history |
| LICENSE | MIT license |

---

# Compatibility

| Component | Support |
|------------|---------|
| Euro Truck Simulator 2 | Supported |
| American Truck Simulator | Untested |
| Windows | x64 |
| Discord Desktop | Supported |
| Discord Browser | Not Supported |

---

# License

MIT

See [LICENSE](LICENSE).
