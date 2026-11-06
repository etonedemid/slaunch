#!/bin/sh
# Builds sLaunch as a qlaunch-ext menu (the same chain as the framework's own
# build.sh, minus its NekoMenu) and stages the SD layout in SdOut-qext/.
#
# Needs dekotools on PATH (neko3d's headers), and `make` to have run once at the
# repo root: the assets and hbloader come from SdOut/.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
QEXT_DIR="${QEXT_DIR:-$HOME/qlaunch-ext}"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
# libnx master: the loader uses NACP fields the packaged 4.12.0 does not have.
LIBNX="${LIBNX:-$HOME/Desktop/Projects/switch/libnx-master-install/opt/devkitpro/libnx}"
NM="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm"
OUT="$ROOT/SdOut-qext"
SDL="$QEXT_DIR/extern/SDL-dlink"
NEKO="$SDL/extern/neko3d/neko3d-module"
FF="$QEXT_DIR/extern/FFmpeg-dlink"
J="-j$(nproc)"

[ -d "$QEXT_DIR/loader" ] || { echo "error: no qlaunch-ext at $QEXT_DIR" >&2; exit 1; }
[ -f "$ROOT/SdOut/slaunch/bin/hbloader/main" ] || { echo "error: run make at the repo root first" >&2; exit 1; }

# Our patches, once.
git -C "$QEXT_DIR" apply --check "$ROOT/scripts/qext/loader.patch" 2>/dev/null &&
    git -C "$QEXT_DIR" apply "$ROOT/scripts/qext/loader.patch"
git -C "$SDL" apply --check "$ROOT/scripts/qext/sdl.patch" 2>/dev/null &&
    git -C "$SDL" apply "$ROOT/scripts/qext/sdl.patch"

make -C "$NEKO" "$J"
for m in miniini json xxhash; do make -C "$QEXT_DIR/lib/$m-module" "$J"; done
# (Our patch adds to its configure line, so the script can be newer than the build.)
if [ ! -f "$FF/install/lib/libavcodec.a" ] || [ "$QEXT_DIR/build-ffmpeg.sh" -nt "$FF/install/lib/libavcodec.a" ]; then
    sh "$QEXT_DIR/build-ffmpeg.sh"
    rm -f "$FF/dnro/ffmpeg.elf" "$FF/dnro/ffmpeg.dnro"
fi
make -C "$FF/dnro" "$J" FFPREFIX="$FF/install" DLINK="$QEXT_DIR/extern/libnx-dlink"

make -C "$ROOT/projects/sMenu" -f qext.mk "$J" QEXT_DIR="$QEXT_DIR" LIBNX="$LIBNX"
make -C "$QEXT_DIR/plugins/ForwarderPlugin" "$J" LIBNX="$LIBNX"
UNDEF="$ROOT/projects/sMenu/build-qext/sMenu.undef.all"
sort -u "$ROOT/projects/sMenu/build-qext/sMenu.undef" "$FF/dnro/ffmpeg.undef" \
    "$QEXT_DIR/plugins/ForwarderPlugin/ForwarderPlugin.undef" > "$UNDEF"

# The module loader sets up no TLS block, so thread_local data faults on first use.
if "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-readelf" -SW "$ROOT/projects/sMenu/build-qext/sMenu.elf" | grep -E '\.t(bss|data) ' | awk '$6 != "000000" {found=1} END {exit !found}'; then
    echo "error: sMenu.dnro has thread_local data" >&2
    exit 1
fi

make -C "$QEXT_DIR/loader/nroloader" "$J"
# A menu with new imports needs the loader relinked.
rm -f "$QEXT_DIR/loader/loader.elf" "$QEXT_DIR/loader/loader.nso" "$QEXT_DIR/loader/loader.nsp"
make -C "$QEXT_DIR/loader" "$J" QEXT_EXTRA_UNDEF="$UNDEF" LIBNX="$LIBNX"
HOST_ELF="$QEXT_DIR/loader/loader.elf" sh "$SDL/build.sh"

# Everything the menu imports must exist, or it dies at dlopen with a bare
# COULD_NOT_RESOLVE on a black screen.
{ "$NM" -D --defined-only "$QEXT_DIR/loader/loader.elf" | awk '{print $NF}'
  cat "$SDL/dnro/sdl3.def" "$NEKO/neko3d.def" "$FF/dnro/ffmpeg.def" "$QEXT_DIR"/lib/*-module/*.def
} | tr -d '\r' | sort -u > "$UNDEF.have"
MISSING=$(comm -23 "$UNDEF" "$UNDEF.have")
if [ -n "$MISSING" ]; then
    echo "error: sMenu imports nothing provides:" >&2
    echo "$MISSING" >&2
    exit 1
fi
echo "import check passed"

rm -rf "$OUT"
mkdir -p "$OUT/atmosphere/contents/0100000000001000" "$OUT/lib" \
         "$OUT/qlaunch-ext/menus" "$OUT/qlaunch-ext/bin" "$OUT/qlaunch-ext/plugins" "$OUT/slaunch"
# Loose files rather than exefs.nsp, in the same place sSystem's are. Atmosphere prefers
# loose files, so an nsp would lose to an existing sSystem; these overwrite it, which
# makes copying this layout over an old install the whole upgrade, by zip or sInstaller.
mkdir -p "$OUT/atmosphere/contents/0100000000001000/exefs"
cp "$QEXT_DIR/loader/loader.nso" "$OUT/atmosphere/contents/0100000000001000/exefs/main"
cp "$QEXT_DIR/loader/loader.npdm" "$OUT/atmosphere/contents/0100000000001000/exefs/main.npdm"
cp "$NEKO/neko3d.dnro" "$SDL/dist/lib/sdl3.dnro" "$FF/dnro/ffmpeg.dnro" \
   "$QEXT_DIR/lib/miniini-module/miniini.dnro" "$QEXT_DIR/lib/json-module/json.dnro" \
   "$QEXT_DIR/lib/xxhash-module/xxhash.dnro" "$OUT/lib/"
cp "$ROOT/projects/sMenu/build-qext/sMenu.dnro" "$OUT/qlaunch-ext/menus/menu.dnro"
cp "$QEXT_DIR/loader/nroloader/nroloader.nsp" "$OUT/qlaunch-ext/bin/"
# Off unless asked for: its default donor is one particular game, so its tile fails
# for anyone without it. Set the donor in qlaunch-ext/config/plugin/forwarder.ini.
if [ -n "${QEXT_FORWARDER:-}" ]; then
    mkdir -p "$OUT/qlaunch-ext/forwarders" "$OUT/qlaunch-ext/icons" "$OUT/qlaunch-ext/config/plugin"
    cp "$QEXT_DIR/plugins/ForwarderPlugin/ForwarderPlugin.dnro" "$OUT/qlaunch-ext/plugins/"
    cp "$QEXT_DIR/plugins/ForwarderPlugin/forwarders/"*.ini "$OUT/qlaunch-ext/forwarders/"
    cp "$QEXT_DIR/plugins/ForwarderPlugin/icon/"*.jpg "$OUT/qlaunch-ext/icons/"
    cp "$QEXT_DIR/plugins/ForwarderPlugin/config/forwarder.ini" "$OUT/qlaunch-ext/config/plugin/"
fi
# The menu's assets and hbloader, as the applet build ships them.
cp -r "$ROOT"/SdOut/slaunch/* "$OUT/slaunch/"
rm -rf "$OUT/slaunch/bin/sMenu" "$OUT/slaunch/bin/sMenu-vk" "$OUT/slaunch/bin/hbloader_app"
# sLaunch Control, for the installer and the zip.
if [ -d "$ROOT/SdOut/switch/.packages" ]; then
    mkdir -p "$OUT/switch"
    cp -r "$ROOT/SdOut/switch/.packages" "$OUT/switch/"
fi
echo "built: $OUT"
