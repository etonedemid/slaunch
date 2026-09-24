# Third-party code

sLaunch is licensed under the GNU General Public License v3.0 (see `LICENSE`).
This file documents why, and credits the code that made the license change
necessary.

## FFmpeg (nvtegra branch) — hardware video decode

sMenu's video wallpaper support decodes video on the Switch's own hardware
NVDEC block. There is no standalone library for this on Horizon OS; the only
maintained implementation is a small set of files inside
[averne/FFmpeg](https://github.com/averne/FFmpeg) (the `nvtegra` branch),
which implement a custom Tegra X1 hardware-acceleration backend for FFmpeg:

| File | What it does |
|------|--------------|
| `libavutil/hwcontext_nvtegra.c` / `.h` | NVDEC device and frame context: nvmap/nvhost allocations, syncpoint management |
| `libavutil/nvtegra.c` / `.h` | Shared Tegra host1x command-buffer helpers |
| `libavcodec/nvtegra_decode.c` / `.h` | Shared NVDEC decode helper (command buffer building, bitstream repacking) |
| `libavcodec/nvtegra_h264.c` / `.h` | H.264-specific hardware decode command sequences |

These files are copyright averne, licensed GPLv2-or-later (see each file's
header). Building `sMenu` with them compiled in makes the resulting binary a
GPL-covered work, which is why the whole sLaunch repository moved from MIT to
GPL-3.0 rather than keeping the two licenses separate.

`scripts/build-ffmpeg.sh` clones the pinned FFmpeg commit and cross-builds a
trimmed static library (H.264 decode + MOV demuxing only) for the Switch
target; see that script for the exact commit and configure flags.

## SwitchWave — the inspiration, not the code

[averne/SwitchWave](https://github.com/averne/SwitchWave) is a full
hardware-accelerated media player for the Switch, built on this FFmpeg fork
plus a custom mpv fork and a deko3d graphics backend. sLaunch does **not**
use SwitchWave's mpv, deko3d, or `libuam` (its deko3d shader compiler) —
only the FFmpeg demux/decode/NVDEC-hwaccel layer above is ported. Rendering
stays on sLaunch's own SDL2 pipeline (`Gfx.cpp`).

## haze (Atmosphère) — USB file transfer

sMenu's MTP server is haze, from
[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) (`troposphere/haze`,
tag 1.11.2), copyright Atmosphère-NX, GPLv2. It is vendored under `libs/haze`
without its standalone app, and with a fixed-size object heap so it can share
the menu's process; `libs/haze/README.md` lists the changes.

## Other bundled code

See `assets/fonts/ATTRIBUTION.md`, `assets/hbloader/ATTRIBUTION.md`,
`assets/icon_packs/Minimal/ATTRIBUTION.md`, and
`assets/icons/buttons/ATTRIBUTION.md` for fonts, the hbloader fork, and icon
pack credits.
