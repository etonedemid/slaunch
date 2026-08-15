# sLaunch

A fast, clean, **SDL2-based HOME Menu replacement** for the Nintendo Switch
(Atmosphere CFW). Themes with wallpapers, custom fonts, Lua widgets, icon packs,
translations, and lots of UI modes.

[Discord](https://discord.gg/dv28MgtaNn)




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
  |- themes (5 built-in + custom), fonts, locales, OOBE, persisted to the SD
  \- asks sSystem (over SMI) to launch games / open system applets

hbloader  (fork of nx-hbloader, served into an applet or donor-game slot)
  \- loads a specific .nro named by slaunch/hbtarget.txt, then exits back to
     the menu instead of reloading itself (see projects/hbloader/README.md)
```

SD card layout produced by the build:

```
atmosphere/contents/0100000000001000/exefs/{main,main.npdm}   sSystem daemon (qlaunch)
slaunch/bin/sMenu/{main,main.npdm}                            sMenu applet (ECS exefs)
slaunch/bin/hbloader/                                         homebrew loader, applet mode
slaunch/bin/hbloader_app/                                     homebrew loader, full-RAM title mode
slaunch/fonts/                                                bundled fonts (incl. Noto Sans CJK)
slaunch/icons/                                                built-in system icons
slaunch/icon_packs/<pack>/                                    icon packs (bundled and user-made)
slaunch/lang/                                                 translations + template.txt
slaunch/music/                                                background music (mp3/ogg/flac)
slaunch/sounds/                                               UI sound effects
slaunch/widgets/                                              Lua home-screen widgets
slaunch/themes/                                               user wallpapers (.jpg/.png)
slaunch/config/                                               settings, saved at runtime
```

### Translations

The menu is fully translatable. `slaunch/lang/template.txt` lists every string
it can show; copy it to `<code>.txt` (e.g. `fr.txt`, `pt-BR.txt`) in the same
folder and translate the right-hand side of each `=`. The console's system
language picks the file, trying the regional form first and then the two-letter
one. Missing strings, and missing files, fall back to English.

Shipped: `ru`, `ja`, `de`, `es`, `zh` (Simplified).

Non-Latin scripts need a font with those glyphs. sLaunch selects the console's
own shared font from the system language, so Japanese, Korean and Chinese render
correctly with nothing installed; a full Noto Sans CJK is also bundled and
selectable under **Theming > Fonts** for reading names in other scripts.

### Widgets

Lua widgets drawn on the home screen and draggable with touch. See
[docs/WIDGETS.md](docs/WIDGETS.md).

## Building

Requires devkitPro (devkitA64 + libnx + the switch SDL2 stack) and a built
**libstratosphere** (Atmosphere 1.11.2). On Linux/WSL2:

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

Individual targets: `make ssystem`, `make smenu`, `make sinstaller`,
`make hbloader`, `make assets`. `make package` zips `SdOut/`.

`make` produces the full SD layout under `SdOut/`; copy it to your SD card.
The daemon Makefile expects the Atmosphere checkout at `/opt/atmosphere`
(override with `ATMOSPHERE_DIR=`).

Eject the card properly before removing it. A half-written NSO on the qlaunch
slot crash-loops the console on boot.

## Status & diagnostics

The daemon and applet each write a small bring-up log to the SD card, which
makes hardware issues diagnosable without a debugger:

- `slaunch/daemon.log` - daemon boot + ECS register/launch results, power path
- `slaunch/ecs.log`    - ECS filesystem-server thread status
- `slaunch/boot.log`   - sMenu applet: `main enter` / `gfx.Init OK|FAILED`

If the menu doesn't appear, those logs (plus `atmosphere/crash_reports/` and
`atmosphere/fatal_reports/`) point at the exact stage. A crash report names the
faulting module and offset; `aarch64-none-elf-nm -SCn` on the matching
`build/*.elf` turns that offset into a function.

**Recovery:** delete `atmosphere/contents/0100000000001000/` from the SD to
return to the stock HOME Menu.

## Credits

- The daemon's ECS content-serving, the sysmodule structure, the qlaunch/applet
  NPDMs, and the applet/daemon model are adapted from
  **[uLaunch](https://github.com/Xortroll/uLaunch)** by Xortroll & contributors
  (GPLv2). The bundled homebrew loader is a fork of
  **[nx-hbloader](https://github.com/switchbrew/nx-hbloader)** (ISC).
- The `Minimal` icon pack is by
  **[MeepCat55](https://github.com/meepcat55)**.
- Bundled fonts are SIL OFL / Apache licensed - see `assets/fonts/ATTRIBUTION.md`.
