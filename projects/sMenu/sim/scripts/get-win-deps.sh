#!/bin/sh
# Stage the Windows cross-build dependencies.
#
# SDL2 and libcurl for MinGW are not in the Arch repositories, so the official
# prebuilt packages are fetched here. They are headers, import libraries and
# DLLs - nothing is compiled from source.
#
# libcurl matters for more than convenience: with it, the Windows build compiles
# the real sl/menu/net/Http.cpp, so the simulator runs the shipping networking
# code rather than a substitute.
set -e

DEST="$1"
mkdir -p "$DEST"
cd "$DEST"

SDL_URLS="https://github.com/libsdl-org/SDL/releases/download/release-2.30.12/SDL2-devel-2.30.12-mingw.tar.gz
https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.8/SDL2_image-devel-2.8.8-mingw.tar.gz
https://github.com/libsdl-org/SDL_ttf/releases/download/release-2.22.0/SDL2_ttf-devel-2.22.0-mingw.tar.gz
https://github.com/libsdl-org/SDL_mixer/releases/download/release-2.8.1/SDL2_mixer-devel-2.8.1-mingw.tar.gz"

CURL_URL="https://curl.se/windows/latest.cgi?p=win64-mingw.zip"

for u in $SDL_URLS; do
    f=$(basename "$u")
    if [ -f "$f" ]; then
        echo "have  $f"
    else
        echo "get   $f"
        curl -sSL -o "$f" "$u"
    fi
done

if [ -f curl-win64-mingw.zip ]; then
    echo "have  curl-win64-mingw.zip"
else
    echo "get   curl-win64-mingw.zip"
    curl -sSL -o curl-win64-mingw.zip "$CURL_URL"
fi

echo "extracting..."
for f in *.tar.gz; do tar xzf "$f"; done
if command -v unzip >/dev/null 2>&1; then
    unzip -q -o curl-win64-mingw.zip
else
    echo "unzip not available - libcurl will be skipped" >&2
fi

# Each package unpacks an x86_64-w64-mingw32/ tree; merge them into one prefix
# so the Makefile needs a single -I and -L.
mkdir -p prefix
for d in SDL2-*/x86_64-w64-mingw32 SDL2_image-*/x86_64-w64-mingw32 \
         SDL2_ttf-*/x86_64-w64-mingw32 SDL2_mixer-*/x86_64-w64-mingw32; do
    [ -d "$d" ] && cp -a "$d/." prefix/
done

# curl's zip has a flat bin/ include/ lib/ layout under one directory.
for d in curl-*-win64-mingw; do
    [ -d "$d" ] || continue
    [ -d "$d/include" ] && cp -a "$d/include/." prefix/include/
    [ -d "$d/lib" ]     && cp -a "$d/lib/."     prefix/lib/
    [ -d "$d/bin" ]     && cp -a "$d/bin/."     prefix/bin/
done

echo "prefix:"
echo "  headers: $(ls prefix/include/SDL2 2>/dev/null | wc -l) SDL2, curl: $([ -f prefix/include/curl/curl.h ] && echo yes || echo NO)"
echo "  dlls:    $(ls prefix/bin/*.dll 2>/dev/null | wc -l)"
