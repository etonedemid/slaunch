# Why the boot splash needs a hekate patch

`sInstaller`'s `InstallSplash()` writes a boot logo into `atmosphere/package3`.
That alone does nothing on a normal console: package3 is loaded by hekate's
own parser (`bootloader/hos/pkg3.c` upstream), and that parser defines a
`CNT_TYPE_BMP` content type but never processes it. Drawing it was
fusee-primary's job, and Atmosphère removed fusee-primary entirely in 1.10.0 -
there's no current, working binary anywhere that still does this.

So the splash needs two changes on top of each other:

1. **`scripts/hekate-pkg3-splash.patch`** - a ~15-line patch against hekate
   source (tested against v6.5.3) that adds the missing `CNT_TYPE_BMP` case
   and draws it with hekate's own `gfx_render_bmp_argb()`, the same function
   that already renders a custom `logopath` BMP. Apply it to a matching
   hekate checkout, build with `DEVKITARM` (`devkitARM` package, separate
   from the `devkitA64` toolchain the rest of this repo uses), and reflash
   the result as both `payload.bin` and `bootloader/update.bin` on the SD
   card - hekate's own binary, not anything this repo's build produces.

2. **A table-of-contents entry in package3** pointing at the pixel data.
   package3 isn't a fixed layout; it's a header (`"PK31"` at byte 0, an
   `"FSS0"` meta struct at the offset stored in byte 4) followed by a list of
   `(offset, size, type, name)` entries. `InstallSplash()` (in
   `projects/sInstaller/source/main.cpp`) parses that table, adds or updates
   a `type = 7` (`CNT_TYPE_BMP`) entry pointing at `0x400000`, and writes the
   pixel data there. That offset is free in every layout observed so far -
   nothing else claims it, and the entry named `fusee` always starts exactly
   at `0x7C0000` - but `InstallSplash()` checks for a collision before
   writing rather than assuming it.

Pixel format: exactly 720x1280, 32bpp BGRA, rows stored bottom-up (a plain
BMP's own row order) - no stride padding, unlike the old fusee-primary
format this replaced. `scripts/make_splash.py` produces it from
`assets/splash.png` by reusing Pillow's own BMP writer and stripping the
54-byte header, so it's pixel-identical to `scripts/make_hekate_bootlogo.py`'s
`bootloader/res/bootscreen.bmp` - one source image, one transform, two
containers.

## What this can't do

The Nintendo Switch logo shown after this is drawn by system firmware (NAND),
not by anything on the SD card. No patch here touches that.
