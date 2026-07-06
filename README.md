# sLaunch

A fast, clean, **SDL2-based HOME Menu replacement** for the Nintendo Switch
(Atmosphère CFW). Themes with wallpapers, custom fonts, a first-run setup flow,
and a single scrollable menu of your games + system shortcuts.

> Status: **boots and runs on hardware.** 

<img width="1280" height="720" alt="2026070522182400-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/5f1b99aa-48eb-4218-b5c6-50ea6d116164" />
<img width="1280" height="720" alt="2026070522175200-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/17ef74c8-767e-40e7-b4d8-11dbb7ca7859" />


## Architecture

sLaunch follows the same split as [uLaunch](https://github.com/Xortroll/uLaunch):
a privileged daemon that *is* the HOME Menu, plus a graphical applet that
renders the UI. A system applet cannot create an SDL/GPU window, so the UI has
to live in a library-applet slot.

```
sSystem   (libstratosphere sysmodule, runs as qlaunch / program 0100000000001000)
  |- launches/suspends/terminates games (libnx applet API)
  |- ECS: registers sMenu's SD folder as external code for an applet slot and
  |       serves it over a libstratosphere fs server (ldr:shel cmd 65000)
  \- SMI: talks to the menu over the library-applet in/out-data channel

sMenu     (SDL2 library applet, served into the shop applet slot via ECS)
  |- renders the menu (SDL2 + SDL2_ttf + SDL2_image), system font via pl
  |- themes (5 built-in + custom), fonts, OOBE, all persisted to the SD
  \- asks sSystem (over SMI) to launch games / open system applets
```

SD card layout produced by the build:

```
atmosphere/contents/0100000000001000/exefs/{main,main.npdm}   sSystem daemon (qlaunch)
slaunch/bin/sMenu/{main,main.npdm}                            sMenu applet (ECS exefs)
slaunch/fonts/                                                bundled fonts
slaunch/config/                                               theme/font config (created at runtime)
slaunch/                                               optional user wallpapers (.jpg and .png)
```

## Building

Requires devkitPro (devkitA64 + libnx + the switch SDL2 stack) and a built
**libstratosphere** (Atmosphère 1.11.2). On Linux/WSL2:

```sh
export DEVKITPRO=/opt/devkitpro
# one-time: build libstratosphere into $DEVKITPRO/AtmosphereLibs
git clone --depth=1 --branch 1.11.2 https://github.com/Atmosphere-NX/Atmosphere /opt/atmosphere
make -C /opt/atmosphere/libraries/libstratosphere -j$(nproc)
mkdir -p $DEVKITPRO/AtmosphereLibs/include $DEVKITPRO/AtmosphereLibs/lib
cp -r /opt/atmosphere/libraries/libstratosphere/include/. $DEVKITPRO/AtmosphereLibs/include/
cp -r /opt/atmosphere/libraries/libvapours/include/.      $DEVKITPRO/AtmosphereLibs/include/
find /opt/atmosphere/libraries/libstratosphere -name '*.a' -exec cp {} $DEVKITPRO/AtmosphereLibs/lib/ \;

make            # builds everything into SdOut/
```

`make` produces the full SD layout under `SdOut/`; copy it to your SD card.
The daemon Makefile expects the Atmosphère checkout at `/opt/atmosphere`
(override with `ATMOSPHERE_DIR=`).

## Status & diagnostics

The daemon and applet each write a small bring-up log to the SD card, which
makes hardware issues diagnosable without a debugger:

- `slaunch/daemon.log` - daemon boot + ECS register/launch results
- `slaunch/ecs.log`    - ECS filesystem-server thread status
- `slaunch/boot.log`   - sMenu applet: `main enter` / `gfx.Init OK|FAILED`

If the menu doesn't appear, those logs (plus `atmosphere/crash_reports/` and
`atmosphere/fatal_reports/`) point at the exact stage.

**Recovery:** delete `atmosphere/contents/0100000000001000/` from the SD to
return to the stock HOME Menu.

## Credits

- The daemon's ECS content-serving, the sysmodule structure, the qlaunch/applet
  NPDMs, and the applet/daemon model are adapted from
  **[uLaunch](https://github.com/Xortroll/uLaunch)** by Xortroll & contributors
  (GPLv2). The homebrew-loader-style NPDM base is from
  **[nx-hbloader](https://github.com/switchbrew/nx-hbloader)**.
- Bundled fonts are SIL OFL / Apache licensed - see `assets/fonts/ATTRIBUTION.md`.
