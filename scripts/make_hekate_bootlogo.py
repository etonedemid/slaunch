#!/usr/bin/env python3
# Builds hekate-format boot images from sLaunch's own art, for consoles that
# boot through hekate (the common case) rather than straight through fusee -
# hekate draws its own logo before Atmosphere ever gets a chance to show the
# package3 splash InstallSplash() writes, so that splash needs its own copy
# here to actually be seen.
#
# Formats reverse-engineered from a real, working hekate theme (BMP, 40-byte
# BITMAPINFOHEADER, no compression, 32 bits/pixel - Pillow's default BMP
# writer matches this exactly for an RGBA image):
#   - full-screen boot logo: 720x1280, source rotated 90 CCW (matches
#     hekate's documented bootlogo.bmp requirement)
#   - boot-menu entry icon: 192x192, square, not rotated
import sys
from pathlib import Path
from PIL import Image

ROOT     = Path(__file__).resolve().parent.parent
SPLASH   = ROOT / "assets" / "splash.png"
ICON     = ROOT / "icon.png"
OUT_DIR  = ROOT / "assets" / "hekate"

def make_bootlogo():
    img = Image.open(SPLASH).convert("RGBA")
    if img.size != (1280, 720):
        sys.exit(f"{SPLASH} is {img.size}, expected 1280x720")
    rotated = img.transpose(Image.ROTATE_90)   # -> 720x1280, same as make_splash.py
    dst = OUT_DIR / "bootscreen.bmp"
    rotated.save(dst)
    print(f"wrote {dst} ({rotated.size[0]}x{rotated.size[1]})")

def make_menu_icon(name):
    img = Image.open(ICON).convert("RGBA").resize((192, 192), Image.LANCZOS)
    dst = OUT_DIR / name
    img.save(dst)
    print(f"wrote {dst} (192x192)")

def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    make_bootlogo()
    make_menu_icon("atmo_cfw.bmp")
    make_menu_icon("atmo_sm.bmp")

if __name__ == "__main__":
    main()
