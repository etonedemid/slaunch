#!/usr/bin/env python3
# Converts assets/splash.png into the raw pixel blob InstallSplash() embeds
# into Atmosphere's package3 as a CNT_TYPE_BMP content entry.
#
# This used to be the old fixed-offset fusee-primary splash format (768px
# stride, read from a hardcoded file offset with no table-of-contents entry
# at all). That format is dead: fusee-primary was removed from Atmosphere in
# 1.10.0, and hekate's own pkg3 loader - what every current console actually
# boots through - defines CNT_TYPE_BMP but never processes it. See
# scripts/hekate-pkg3-splash.patch, which adds that processing back, reusing
# hekate's own gfx_render_bmp_argb() the same way it already draws a custom
# logopath BMP. That function wants an exact 720x1280 32bpp buffer with rows
# stored bottom-up (matching a BMP file's own row order) - no 768 stride, no
# padding - so this just does the same rotate hekate's bootlogo.bmp needs
# (scripts/make_hekate_bootlogo.py) and reuses Pillow's BMP writer to get
# that exact byte layout, then strips the 54-byte BMP header off.
import sys
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC  = ROOT / "assets" / "splash.png"
DST  = ROOT / "assets" / "splash.bin"

BMP_HEADER_SIZE = 54   # BITMAPFILEHEADER(14) + BITMAPINFOHEADER(40), no palette
EXPECTED_SIZE   = 720 * 1280 * 4

def main():
    img = Image.open(SRC).convert("RGBA")
    if img.size != (1280, 720):
        sys.exit(f"{SRC} is {img.size}, expected 1280x720")

    rotated = img.transpose(Image.ROTATE_90)   # -> 720x1280, matches the hekate bootlogo
    tmp = DST.with_suffix(".tmp.bmp")
    rotated.save(tmp)
    body = tmp.read_bytes()[BMP_HEADER_SIZE:]
    tmp.unlink()

    assert len(body) == EXPECTED_SIZE, f"got {len(body):#x} bytes, expected {EXPECTED_SIZE:#x}"
    DST.write_bytes(body)
    print(f"wrote {DST} ({len(body)} bytes)")

if __name__ == "__main__":
    main()
