# sLaunch — Installation & Build Guide

sLaunch is a blazing-fast, text-based Nintendo Switch HOME Menu replacement.
No SDL, no GPU rendering — just raw framebuffer text at 160×45 characters.

## Requirements

- Nintendo Switch running **Atmosphere** custom firmware
- **devkitPro** with **devkitA64** and **libnx**
- Atmosphere `libstratosphere` headers (for ECS / `ldrShellAtmosphereRegisterExternalCode`)

Install devkitPro packages:
```
dkp-pacman -S devkitA64 libnx switch-tools
```

## Build

```
make          # builds both sSystem and sMenu
make ssystem  # backend only
make smenu    # UI only
make package  # produces slaunch.zip
make clean
```

Output is written to `SdOut/`. Copy its contents to your SD card root.

## SD Card Layout (what gets installed)

```
SD:/
├── atmosphere/
│   └── contents/
│       └── 0100000000001000/      <- qlaunch title ID
│           └── exefs/
│               └── main           <- sSystem NSO (replaces qlaunch)
└── slaunch/
    └── bin/
        └── sMenu/
            └── main               <- sMenu NSO (text UI, loaded via ECS)
```

## How It Works

```
Nintendo Switch boots
        |
        v
  Atmosphere CFW
        |
        v
  sSystem starts (as qlaunch replacement)
  [title ID 0100000000001000]
        |
        +-- registers sMenu NSO with Atmosphere ECS
        |   (eShop applet slot → sdmc:/slaunch/bin/sMenu/main)
        |
        +-- launches sMenu as a library applet
        |   (user sees the text-based game list)
        |
        +-- main loop:
              |-- receives SMI commands from sMenu (via AppletStorage)
              |-- handles HOME/sleep/gamecard system events
              `-- manages the running application lifecycle

sMenu (text UI):
  - draws using libnx console (framebuffer, no GPU)
  - reads input with padUpdate / padGetButtonsDown
  - sends SMI commands to sSystem when user selects a game
```

## Controls

| Button       | Action                          |
|--------------|---------------------------------|
| Up / Down    | Navigate game list              |
| L / R        | Page up / page down             |
| A            | Launch selected game            |
| B            | Options (close running game)    |
| - (Minus)    | Resume suspended game           |
| X            | Open Album                      |
| Y            | Open User page                  |
| + (Plus)     | Power menu (sleep)              |

## Uninstalling

Delete `atmosphere/contents/0100000000001000/` from your SD card.
The official HOME Menu (qlaunch) will be restored automatically.

## Architecture Notes

**SMI Protocol** — sMenu and sSystem communicate via `AppletStorage` blobs
(32-byte magic header + payload). No IPC sessions or service calls involved.
Both directions use the AppletStorage queue:
- sMenu → sSystem: `appletPushToGeneralChannel` / `appletHolderPopOutData`
- sSystem → sMenu: `appletHolderPushInData` / `appletPopFromGeneralChannel`

**ECS (External Content Storage)** — Atmosphere's loader intercepts the eShop
library applet launch and loads `sMenu/main` (our NSO) instead. This lets us
run arbitrary code in a library applet slot without a separate NRO loader.

**Zero SDL** — sMenu uses only `consoleInit()` + `printf()` + ANSI escape codes.
Startup time is under 100ms. No audio service conflicts.

## Differences from uLaunch

| Feature          | uLaunch          | sLaunch             |
|------------------|------------------|---------------------|
| UI rendering     | SDL2 + Plutonium | libnx console       |
| Graphics         | GPU (NVN)        | CPU framebuffer     |
| Startup time     | ~2s              | <100ms              |
| Theming          | Full (images, audio) | ANSI colors only |
| Homebrew browser | Yes              | Planned             |
| Folder support   | Yes              | Planned             |
| Dependencies     | SDL2, freetype, etc. | libnx only      |
