#!/bin/sh
# Transplant a Switch SD card into the simulator.
#
#   ./sync-sd.sh /mnt/e          # card mounted at E: under WSL
#   ./sync-sd.sh                 # defaults to /mnt/e
#
# Creates a directory literally named "sdmc:" next to this script. Linux allows
# a colon in a filename, so every "sdmc:/..." path already in the menu resolves
# into it with no path rewriting anywhere in the code - which is what lets the
# simulator run the shipping sources unmodified.
#
# This is a one-way copy INTO the simulator. It never writes to the card.

set -e

SRC="${1:-/mnt/e}"
# Outside the repo on purpose: a colon is illegal in a Windows filename, so an
# in-tree copy would be a hazard for anything on the Windows side that walks the
# checkout, and reading a card's worth of art is far faster from the Linux
# filesystem than over the interop mount.
ROOT="${SLAUNCH_SIM_ROOT:-$HOME/.slaunch-sim}"
DEST="$ROOT/sdmc:"

if [ ! -d "$SRC/slaunch" ]; then
    echo "no $SRC/slaunch - is the card mounted?" >&2
    echo "usage: $0 [path-to-sd-root]" >&2
    exit 1
fi

echo "card:      $SRC"
echo "simulator: $DEST"

mkdir -p "$DEST/slaunch"

# Everything the menu reads, minus three things:
#   cache/blur  - keyed on each wallpaper's size and mtime, both of which change
#                 when the file is copied, so every entry would miss anyway. It
#                 rebuilds itself on first draw.
#   *.log       - the console's own boot/cover logs; the simulator writes its
#                 own and mixing them makes both useless.
#   config/kb_* - keyboard handoff to a daemon that does not exist here.
# cp rather than rsync: a WSL image is often minimal and rsync is not part of
# it, while cp always is.
echo "copying slaunch/ ..."
rm -rf "$DEST/slaunch"
mkdir -p "$DEST/slaunch"
cp -a "$SRC/slaunch/." "$DEST/slaunch/"
rm -rf "$DEST/slaunch/cache/blur"
rm -f  "$DEST/slaunch/config/kb_req.txt" "$DEST/slaunch/config/kb_result.txt"
find "$DEST/slaunch" -maxdepth 1 -name '*.log' -delete

# Homebrew entries without the homebrew.
#
# The menu lists .nro files by walking sdmc:/switch, but it takes each one's
# NAME and ICON from cache/hb_manifest.txt (keyed by path) and cache/hbicons/.
# So recreating the paths as empty files is enough to reproduce exactly the same
# list, with the right names and icons, without copying the binaries - which are
# routinely gigabytes and which the simulator could not launch anyway.
MANIFEST="$DEST/slaunch/cache/hb_manifest.txt"
if [ -f "$MANIFEST" ]; then
    n=0
    # Line format: key<TAB>path<TAB>name
    while IFS="$(printf '\t')" read -r _key path _name; do
        case "$path" in
            sdmc:/*) ;;
            *) continue ;;
        esac
        rel="${path#sdmc:/}"
        mkdir -p "$DEST/$(dirname "$rel")"
        [ -e "$DEST/$rel" ] || : > "$DEST/$rel"
        n=$((n + 1))
    done < "$MANIFEST"
    echo "homebrew:  $n placeholder .nro entries"
else
    echo "homebrew:  no manifest on the card, so no homebrew entries"
fi

# A place to drop the console's shared font. The simulator looks here first; see
# README.md for why that matters and what happens without it.
mkdir -p "$DEST/slaunch/sim"

games=0
[ -f "$DEST/slaunch/cache/applist.txt" ] && games=$(wc -l < "$DEST/slaunch/cache/applist.txt")
icons=0
[ -d "$DEST/slaunch/cache/icons" ] && icons=$(find "$DEST/slaunch/cache/icons" -name '*.jpg' | wc -l)
covers=0
[ -d "$DEST/slaunch/covers" ] && covers=$(find "$DEST/slaunch/covers" -name '*.jpg' | wc -l)

echo
echo "games:     $games"
echo "icons:     $icons"
echo "covers:    $covers"
echo "size:      $(du -sh "$DEST" | cut -f1)"
echo
echo "run: ./slaunch-sim          (card root: $ROOT)"
