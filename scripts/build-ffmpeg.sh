#!/usr/bin/env bash
# Builds a trimmed static FFmpeg (H.264 decode + MOV demuxing + the nvtegra
# NVDEC hwaccel, nothing else) cross-compiled for the Switch, and installs it
# into $DEVKITPRO/FFmpegLibs - same shape as the Atmosphere libstratosphere
# step in .github/workflows/build.yml, which projects/sMenu/Makefile then
# links via LIBDIRS/LIBS.
#
# Source: https://github.com/averne/FFmpeg (nvtegra branch) - see
# THIRDPARTY.md for what was ported and why sLaunch is GPL-3.0.
#
# Configure flags come from averne/SwitchWave (proven working against this
# exact fork - see its Makefile FFMPEG_CONFIG variable), trimmed of everything
# SwitchWave needs for its full player (libdav1d/libwebp/libass/freetype/
# fribidi/mbedtls/zlib/bzlib, screenshot encoders) that a silent looping
# background video does not: only avformat+avcodec+avutil, one demuxer (mov),
# one decoder (h264), and its nvtegra hardware decode path.
set -euo pipefail

: "${DEVKITPRO:?DEVKITPRO must be set (source the devkitPro profile script first)}"

FFMPEG_REPO="https://github.com/averne/FFmpeg.git"
FFMPEG_REF="caeec83b791be08ed43468a2ef426d6901d51c78" # nvtegra branch, pinned
PREFIX="$DEVKITPRO/FFmpegLibs"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "--- Fetching averne/FFmpeg @ $FFMPEG_REF (shallow) ---"
# A plain `git clone` pulls FFmpeg's entire multi-decade history (a very
# large, slow fetch) just to check out one pinned commit. GitHub supports
# fetching a single commit SHA directly, so fetch only that.
mkdir -p "$WORKDIR/ffmpeg"
git -C "$WORKDIR/ffmpeg" init -q
git -C "$WORKDIR/ffmpeg" remote add origin "$FFMPEG_REPO"
git -C "$WORKDIR/ffmpeg" fetch --depth=1 origin "$FFMPEG_REF"
git -C "$WORKDIR/ffmpeg" checkout -q FETCH_HEAD

echo "--- Configuring FFmpeg (aarch64-none-elf / horizon, H.264 decode only) ---"
cd "$WORKDIR/ffmpeg"
PATH="$DEVKITPRO/devkitA64/bin:$PATH" ./configure \
    --prefix="$PREFIX" \
    --enable-gpl --enable-version3 \
    --enable-nvtegra \
    --enable-static --disable-shared \
    --target-os=horizon --enable-cross-compile \
    --cross-prefix=aarch64-none-elf- --arch=aarch64 --cpu=cortex-a57 --enable-neon \
    --enable-pic --disable-autodetect --disable-runtime-cpudetect --disable-debug \
    --disable-programs --disable-doc --disable-network \
    --disable-avdevice --disable-swscale --disable-swresample \
    --disable-encoders --disable-muxers --disable-filters \
    --disable-decoders --enable-decoder=h264 --enable-decoder=aac \
    --disable-demuxers --enable-demuxer=mov \
    --disable-protocols --enable-protocol=file \
    --enable-hwaccel=h264_nvtegra \
    --extra-cflags="-isystem $DEVKITPRO/libnx/include -D__SWITCH__" \
    --extra-ldflags="-L$DEVKITPRO/libnx/lib"
    # --extra-ldflags: FFmpeg's own configure adds "-lnx" to every link-based
    # feature test for --target-os=horizon (its libnx dependency, unrelated
    # to what this trimmed build actually needs from libnx) - without this,
    # every such check silently fails to link, which surfaced as "Threading
    # is enabled, but no atomics are available" (the atomics check itself was
    # fine; it just couldn't link at all, same as every other link check).
    # -D__SWITCH__: the nvtegra driver headers (nvhost_ioctl.h etc.) branch
    # on this to use libnx's <switch.h> _NV_IO* macros instead of Linux's
    # <linux/ioctl.h>, which devkitA64's bare aarch64-none-elf toolchain does
    # not have. Every devkitPro project defines this manually (see
    # projects/sMenu/Makefile's LUA_DEFINES) - it is not a compiler builtin.

echo "--- Building ---"
PATH="$DEVKITPRO/devkitA64/bin:$PATH" make -j"$(nproc)" install

echo "FFmpeg libs installed to $PREFIX"
