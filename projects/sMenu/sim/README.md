# sMenu desktop simulator

Runs the sLaunch menu on a PC, so a UI change can be looked at without building
an NRO, copying to the card, ejecting and rebooting the console - and so
screenshots can be captured at up to 4K instead of photographing a 720p screen.

It compiles **the shipping sources**. `Menu.cpp`, `Gfx.cpp`, `Theme.cpp` and the
rest are built unmodified against a libnx shim (`include/switch.h`) instead of
the real libnx. There is no second copy of the UI to keep in step, so the
simulator cannot drift from what the console does.

## Build and run

Both builds are produced in WSL. The Linux one runs there through WSLg; the
Windows one is cross-compiled and copied out to run natively.

```bash
make -C projects/sMenu/sim
```

Then transplant your card and run:

```bash
projects/sMenu/sim/sync-sd.sh /mnt/e
```

```bash
cd projects/sMenu/sim && ./slaunch-sim
```

Requires `sdl2`, `sdl2_image`, `sdl2_ttf`, `sdl2_mixer` and `libcurl` - all
already present in this WSL image. A window needs WSLg, which works here.

### Windows

```bash
make -C projects/sMenu/sim win
```

Cross-compiled with `mingw-w64`, which is already installed. SDL2 for MinGW is
not in the Arch repositories, so `scripts/get-win-deps.sh` stages the official
prebuilt packages from libsdl-org into `~/.slaunch-windeps` - headers, import
libraries and DLLs, nothing built from source. `make win` runs it for you if the
prefix is missing.

The result lands in `dist-win/`: `slaunch-sim.exe` plus the four SDL DLLs. Copy
that folder anywhere on the Windows side, put the card next to the exe as a
plain `sdmc` folder, and run it.

Both builds compile the identical list of menu sources - nothing is swapped out
or reimplemented for Windows. The only difference is the card folder: it is
`sdmc`, without the colon, because Windows does not allow a colon in a filename.
The paths are rewritten on their way into the C library instead - see
`include/sim_win_compat.h`, which also explains why `remove()` alone is
redirected by the linker rather than by a macro.

Audio works on Windows, where WSL usually has no device.

## Controls

| Key | Button | | Key | Button |
|---|---|---|---|---|
| Arrows / WASD | D-pad | | `Q` / `E` | L / R |
| `Z` / Enter | A | | `1` / `2` | Minus / Plus |
| `X` / Backspace | B | | Mouse | Touchscreen |
| `C` | X | | `F12` | Screenshot |
| `V` | Y | | `F10` | Debug overlay |

`Esc` quits. A real gamepad works too if one is plugged in.

## Screenshots

`F12` writes a PNG into `shots/` under the card root. Captures can also be
scripted, which is how the release images are made:

```bash
./slaunch-sim --scale 3 --shot --nav "lllldddaa"
```

`--nav` presses buttons before capturing (`l r u d` directions, `a b x y`
buttons, `p`/`m` Plus/Minus, `[`/`]` L/R, `_` to wait a beat for a screen that
is still loading), spaced far enough apart for the menu's slide animations to
settle. The timing is wall-clock, not frame counts,
and accounts for the welcome screen on its own - so the same command captures
the same screen on both builds regardless of frame rate.

`--scale N` renders the same 1280x720 layout into an N-times-larger surface:
2 gives 2560x1440, 3 gives 3840x2160. This is not an upscale - fonts are
rasterised at the output resolution, so text is genuinely sharp.

Other options: `--user NAME`, `--battery N`, `--charging`, `--offline`,
`--oobe`, `--suspended ID`, `--sd PATH`, `--settle MS`, `--help`.

### If a change seems to have no effect

`make clean` and build again. The sources live on the Windows filesystem and
their timestamps can read as older than the object files to WSL, so `make` says
"nothing to be done" for a file you just edited. The resulting binary is linked
from mismatched objects and can fail in confusing ways - in one case exiting
silently with no output at all. Header dependencies are tracked (`-MMD`), but
nothing can fix a clock that disagrees with itself.

## How the card is simulated

Every path in the menu starts with `sdmc:/`. Linux allows a colon in a filename,
so `sync-sd.sh` builds a directory literally named `sdmc:` and the simulator
changes into its parent before starting. Every existing path then resolves with
**no path rewriting anywhere in the menu**.

That root lives at `~/.slaunch-sim`, deliberately outside the repo: a colon is
not legal in a Windows filename, so an in-tree copy would be a hazard for any
Windows tool that walks the checkout, and a card's worth of art reads far faster
from the Linux filesystem than over the interop mount. Override with
`SLAUNCH_SIM_ROOT` or `--sd`.

`sync-sd.sh` only ever reads the card. It copies `slaunch/` whole, minus the
blur cache (keyed on each wallpaper's mtime, which copying changes, so every
entry would miss anyway - it rebuilds itself) and the console's own logs.

**Homebrew entries without the homebrew.** The menu lists `.nro` files by
walking `sdmc:/switch`, but takes each one's name and icon from
`cache/hb_manifest.txt` and `cache/hbicons/`. So `sync-sd.sh` recreates just the
paths as empty files: the same list appears, with the right names and icons,
without copying binaries that are routinely gigabytes and that the simulator
could not launch anyway.

## What is real and what is not

Real: every screen, layout, animation, theme, font, icon pack, wallpaper, blur,
the 3D coverflow, touch, momentum scrolling, your actual game list and box art,
and the worker threads (so the loading paths behave as they do on hardware).

Not real, and deliberately so:

- **Actions do nothing.** Launching a game or opening a system applet is a
  request to the sSystem daemon, which does not exist here. The simulator prints
  the action and carries on, so every screen stays reachable and nothing
  pretends to have launched.
- **The font is not Nintendo's.** The console draws with its shared system font,
  which is licensed and not in this repo, so the simulator falls back to a
  system TTF. Layout is right, but glyph widths are not identical - **check text
  spacing on hardware, not here.** Drop a dump at
  `~/.slaunch-sim/sdmc:/slaunch/sim/font.ttf` and it matches exactly.
- **Playtimes are invented**, derived from each application id. Playtime feeds
  the XMB sublabel and the Recently-played / Most-played sorts, so zeroing it
  would make those screens impossible to look at. Stable across runs, but not
  your real hours.
- **Battery, firmware and network** are fixed values, adjustable by flag.
- **Kernel introspection fails**, so the debug overlay shows its
  service-unavailable path - which is what the console shows when it is denied
  too.

`--scale 1` is pixel-exact against the console layout. Above 1, text metrics are
divided back down and can differ by a pixel, so use 1 to check spacing and
higher only for pictures.
