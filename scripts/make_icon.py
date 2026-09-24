#!/usr/bin/env python3
# The sLaunch icon (icon.png, assets/icon.jpg) is now cropped straight out of
# the boot splash artwork (assets/splash.png) instead of being a separate
# drawing, so the mark on the console tile and the mark you see at boot are
# the same redesign, not two things that can drift apart.
from pathlib import Path
from PIL import Image

ROOT   = Path(__file__).resolve().parent.parent
SPLASH = ROOT / "assets" / "splash.png"
SIZE   = 256

def main():
    img = Image.open(SPLASH).convert("RGBA")
    bbox = img.convert("L").getbbox()
    cx = (bbox[0] + bbox[2]) / 2
    cy = (bbox[1] + bbox[3]) / 2
    left = round(cx - SIZE / 2)
    top  = round(cy - SIZE / 2)
    crop = img.crop((left, top, left + SIZE, top + SIZE))

    png_path = ROOT / "icon.png"
    crop.save(png_path)
    print(f"wrote {png_path} ({SIZE}x{SIZE})")

    # Switch NRO icons are JPEG with no alpha - flatten onto black, matching
    # the splash's own background.
    jpg_path = ROOT / "assets" / "icon.jpg"
    Image.alpha_composite(Image.new("RGBA", crop.size, (0, 0, 0, 255)), crop) \
         .convert("RGB").save(jpg_path, quality=95)
    print(f"wrote {jpg_path} ({SIZE}x{SIZE})")

if __name__ == "__main__":
    main()
