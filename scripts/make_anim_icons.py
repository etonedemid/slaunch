#!/usr/bin/env python3
# Rasterizes the sInstaller animation sprites (assets/anim/*.svg -> *.png):
# an SD card, a Switch, and the GitHub mark, used by the Installing and
# Checking-for-updates screens. Requires rsvg-convert.
#
# The source SVGs are plain black-on-transparent (svgrepo's default export),
# so each is recolored to white-on-transparent to match every other icon in
# sInstaller - alpha untouched, RGB forced to white.
import subprocess
import sys
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ANIM = ROOT / "assets" / "anim"
SIZE = 300

def main():
    for svg in sorted(ANIM.glob("*.svg")):
        png = svg.with_suffix(".png")
        subprocess.run(["rsvg-convert", "-w", str(SIZE), "-h", str(SIZE),
                         "--keep-aspect-ratio", str(svg), "-o", str(png)], check=True)
        img = Image.open(png).convert("RGBA")
        r, g, b, a = img.split()
        white = Image.merge("RGBA", (Image.new("L", img.size, 255),) * 3 + (a,))
        white.save(png)
        print(f"wrote {png}")

if __name__ == "__main__":
    main()
