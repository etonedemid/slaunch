<img width="64" height="64" alt="icon" src="https://github.com/user-attachments/assets/0a70ab24-5098-4e7b-b6ce-dba046e9230e"/>  for the Nintendo Switch

slaunch is a home replacement that supports theming, widgets, bg music, several UI modes and more.

[Discord](https://discord.gg/dv28MgtaNn) 

<img width="1280" height="720" alt="2026092401121200-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/fc5fcfc0-1348-454c-a057-2459c590818a" />
<img width="1280" height="720" alt="2026092323471800-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/a70ddc69-c610-4c96-9c30-7dcecc787b1d" />

<img width="1280" height="720" alt="2026092323492100-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/8bb60ac8-9c7f-4063-a435-9d5e31cc17e0" />
<img width="1280" height="720" alt="2026092323512500-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/ee98c207-121e-4938-857b-cbbd519c0d7d" />

<img width="1280" height="720" alt="2026092323472900-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/301dde03-aaad-4990-9e10-1253490fdafe" />


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
  |- loads a specific .nro named by slaunch/hbtarget.txt, then exits back to
  |  the menu instead of reloading itself (see projects/hbloader/README.md)
  \- can hand a chainload back to sSystem so it runs with full RAM (opt-in)
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
slaunch/themes/                                               user wallpapers (.jpg/.png/.mp4)
slaunch/config/                                               console settings, saved at runtime
slaunch/config/users/<account id>/                            each account's own settings
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

### Video wallpapers

A theme's wallpaper can be an `.mp4` file instead of a still image - it plays
back looped and silent behind the menu, decoded on the Switch's own NVDEC
hardware block (see `THIRDPARTY.md` for where that decoder comes from).

- **Format**: H.264 video in an `.mp4` container. Other codecs/containers are
  out of scope for now.
- **Keep it short and modest resolution.** Decode cost scales with both;
  a small looping clip (a few seconds, well under full HD) looks the same
  behind the menu as a longer or larger one and starts up faster.
- **No audio.** The menu has its own background-music system
  (**Theming > Music**); video wallpapers never play a soundtrack.
- **Blur is not supported** for a video wallpaper - re-blurring every decoded
  frame live is not worth the performance cost for a cosmetic effect. The
  Blur toggle greys out in the theme editor while a video wallpaper is active
  (the Dim and Snow overlays still work, since those do not need to inspect
  the frame contents).

### Widgets

Lua widgets drawn on the home screen and draggable with touch. See
[docs/WIDGETS.md](docs/WIDGETS.md).

### Per-account settings

Everything you choose belongs to the Switch account that chose it. Theme,
custom themes, font, icon pack, UI mode, layout tuning, tile sizes and colours,
favourites, manual order, renamed entries, hidden system entries, pinned
homebrew, widget placement and music all live in

```
slaunch/config/users/<account id>/
```

so two people sharing a console get two different menus. The folder is named
after the account's 32-hex-digit id, with a `name.txt` inside holding the
nickname - that is how you tell them apart from a PC.

Content is still shared, because it is content: `themes/`, `icon_packs/`,
`fonts/`, `music/`, `widgets/`, `covers/`, `lang/`. So is anything that
describes the console rather than a person - the SteamGridDB key, the homebrew
donor title, `takeover.txt` - which stays directly in `slaunch/config/`.

Updating from an earlier version loses nothing: the first account to open the
menu inherits the old `slaunch/config/` files, including its first-run setup, so
that account sees exactly what it saw before. The originals are copied rather
than moved, so downgrading still finds them. Any other account starts on the
defaults and goes through first-run setup once, as a new account should.

### USB file transfer

While the menu is on screen, plugging the console into a computer shows the SD
card as an MTP device ("Nintendo Switch"), so files can be copied on and off
without taking the card out. Windows and most Linux file managers support MTP
out of the box; macOS needs an MTP client such as OpenMTP. Auto-sleep is held
off while a computer is connected.

The server stops whenever the menu hands off to a game, homebrew or a system
applet, so homebrew that uses USB itself (DBI, Goldleaf, ...) gets it free. A
copy still running at that moment is cut off.

### Content filter

Off unless you ask for it. With

```
sdmc:/slaunch/config/content_filter.txt    containing: enabled=1
```

the covers and news the menu fetches are checked for adult and extreme-violence
content before anything is downloaded or shown - by keyword, by ESRB/PEGI/USK
rating and by Steam store tags. It filters what sLaunch pulls off the internet;
it is not a parental control over what is installed on the console. See
[docs/CONTENT_FILTER.md](docs/CONTENT_FILTER.md).

## Building

Requires devkitPro (devkitA64 + libnx + the switch SDL2 stack) and a built
**libstratosphere** (Atmosphere 1.11.2). On Linux/WSL2:

```sh
# one-time on Arch: devkitPro ships its own repos, signed with their key
sudo pacman-key --recv-keys BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
sudo pacman-key --lsign-key BC26F752D25B92CE272E0F44F7FD5492264BB9D0
sudo pacman -U https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.xz
# add to /etc/pacman.conf:
#   [dkp-libs]
#   Server = https://pkg.devkitpro.org/packages
#   [dkp-linux]
#   Server = https://pkg.devkitpro.org/packages/linux/$arch
sudo pacman -Sy switch-dev switch-sdl2 switch-sdl2_ttf switch-sdl2_image \
               switch-sdl2_mixer switch-curl switch-mbedtls
# recommended: SDL2_image 2.8 and SDL2_mixer 2.8 from packaging/devkitpro
# (see its README) in place of devkitPro's 2.0.4 - that SDL2_image decodes
# JPEGs with libjpeg, and a corrupt one crashes the menu
# devkitPro's profile script exports DEVKITPRO but not the compiler's own bin
# dir, which the Makefiles need on PATH:
#   export DEVKITA64=$DEVKITPRO/devkitA64; export PATH=$DEVKITA64/bin:$PATH

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

## Firmware

Developed and tested on 18.x; 9.0.0 and up is the range it tries to support.

The menu is not a program of its own - it is served into a library-applet slot,
and *which program a slot launches* is decided by a table inside `am` that is
not the same on every firmware. The shop slot (the one sLaunch takes over,
because web applets get the largest applet heap) launches `010000000000100B`
on older firmware and `0100000000001042` on newer. There is no way to read that
table, so the daemon registers its exefs for **both** ids and lets the firmware
launch whichever one it believes in.

If the menu still never appears, the daemon notices - a slot that opens and
closes without the menu ever saying a word twice in a row means the firmware
launched its own applet - and falls through to the `offlineWeb` slot, writing
every step to `slaunch/daemon.log`. To pin a slot by hand, put a program id in
`slaunch/config/takeover.txt`:

```
0x010000000000100B
```

The daemon then tries that slot first, and only drops back to the built-in
list if it never brings the menu up. Taking over a slot costs you whatever that
applet did, so prefer one the menu has no entry for.

## Credits

- The daemon's ECS content-serving, the sysmodule structure, the qlaunch/applet
  NPDMs, and the applet/daemon model are adapted from
  **[uLaunch](https://github.com/Xortroll/uLaunch)** by Xortroll & contributors
  (GPLv2). The bundled homebrew loader is a fork of
  **[nx-hbloader](https://github.com/switchbrew/nx-hbloader)** (ISC).
- USB file transfer is **haze** from
  **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)** (GPLv2) - see
  `THIRDPARTY.md`.
- The `Minimal` icon pack is by
  **[MeepCat55](https://github.com/meepcat55)**.
- Button prompt icons are from **Xelu's Free Controller and Key Prompts**
  (CC0) - see `assets/icons/buttons/ATTRIBUTION.md`.
- Bundled fonts are SIL OFL / Apache licensed - see `assets/fonts/ATTRIBUTION.md`.
- Video wallpaper decoding uses the NVDEC hardware-acceleration backend from
  **[averne/FFmpeg](https://github.com/averne/FFmpeg)** (`nvtegra` branch,
  GPLv2+), the same decoder used by
  **[SwitchWave](https://github.com/averne/SwitchWave)** - see
  `THIRDPARTY.md`.
