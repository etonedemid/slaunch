#!/bin/sh
# Copies the qlaunch-ext layout over a mock sSystem install and checks the loader is
# what Atmosphere will run, and that the user's data survived. Run after build-qext.sh.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
LAYOUT="$ROOT/SdOut-qext"
SD="$(mktemp -d)"
C="$SD/atmosphere/contents/0100000000001000"

mkdir -p "$C/exefs" "$SD/slaunch/config/users/x" "$SD/slaunch/bin/sMenu"
echo old-ssystem > "$C/exefs/main"
echo old-npdm > "$C/exefs/main.npdm"
echo keep > "$SD/slaunch/config/users/x/settings.txt"

cp -r "$LAYOUT/." "$SD/"

cmp "$C/exefs/main" "$LAYOUT/atmosphere/contents/0100000000001000/exefs/main"
cmp "$C/exefs/main.npdm" "$LAYOUT/atmosphere/contents/0100000000001000/exefs/main.npdm"
[ ! -e "$C/exefs.nsp" ]                                    # would be ignored, so must not ship
[ "$(cat "$SD/slaunch/config/users/x/settings.txt")" = keep ]
[ ! -e "$SD/qlaunch-ext/plugins/ForwarderPlugin.dnro" ]    # donor is game-specific
for f in neko3d sdl3 ffmpeg miniini json xxhash; do [ -f "$SD/lib/$f.dnro" ]; done
for f in qlaunch-ext/menus/menu.dnro qlaunch-ext/bin/nroloader.nsp slaunch/bin/hbloader/main slaunch/bin/hbloader/hbl.nsp slaunch/bin/hbloader_app/main slaunch/bin/hbloader_app/main.npdm; do [ -f "$SD/$f" ]; done
# The loader, not sSystem: this name is in its npdm.
strings "$C/exefs/main.npdm" | grep -q qlaunch-ext-ldr
echo "upgrade ok"
