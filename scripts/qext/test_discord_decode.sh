#!/bin/sh
# Host check of Menu_Discord.cpp's picture decoder (GIF/WebP frame sheets): builds
# its Decode() from the source between kDcSheetBytes and struct Pic, runs it on
# generated animations. Needs SDL3, SDL3_image and ImageMagick on the PC.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
F="$ROOT/projects/sMenu/source/sl/menu/ui/Menu_Discord.cpp"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
a=$(grep -n 'constexpr size_t kDcSheetBytes' "$F" | cut -d: -f1)
b=$(grep -n '        struct Pic {' "$F" | cut -d: -f1)
{
    printf '#include <SDL3/SDL.h>\n#include <SDL3_image/SDL_image.h>\n#include <algorithm>\n#include <cassert>\n#include <cstdio>\n#include <string>\n#include <vector>\nnamespace {\n'
    sed -n "${a},$((b - 1))p" "$F"
    cat <<'EOF'
}
static std::string Read(const char *p) {
    std::string s;
    if (FILE *f = fopen(p, "rb")) { char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); }
    return s;
}
// argv: file frames_expected_min frames_expected_max total_ms (0 = a still)
int main(int, char **argv) {
    Decoded d = Decode(Read(argv[1]), 160, 120);
    const int lo = atoi(argv[2]), hi = atoi(argv[3]), ms = atoi(argv[4]);
    printf("%s: frames=%d sheet=%dx%d total=%dms\n", argv[1], d.frames, d.sheet ? d.sheet->w : 0,
           d.sheet ? d.sheet->h : 0, d.ends.empty() ? 0 : d.ends.back());
    assert(d.sheet && d.frames >= lo && d.frames <= hi);
    assert((d.ends.empty() ? 0 : d.ends.back()) == ms);
    if (d.frames > 1) assert(d.sheet->w == d.cols * 160 && d.sheet->w <= 4096);
    for (size_t j = 1; j < d.ends.size(); j++) assert(d.ends[j] > d.ends[j - 1]);
}
EOF
} > "$T/t.cpp"
g++ -std=c++20 "$T/t.cpp" -o "$T/t" $(pkg-config --cflags --libs sdl3 sdl3-image)
cd "$T"
magick -size 160x120 xc:red xc:green xc:blue -set delay 5 -loop 0 three.gif   # 50 ms
magick three.gif -loop 0 three.webp
mkdir fr
i=0; while [ $i -lt 200 ]; do
    magick -size 200x150 xc:black -fill white -draw "rectangle $i,40 $((i + 10)),110" "fr/$(printf %03d $i).png"
    i=$((i + 1))
done
magick -delay 3 fr/*.png -loop 0 long.gif                                    # 200 x 30 ms
magick -size 72x72 xc:yellow still.png
./t three.gif 3 3 150
./t three.webp 3 3 150
./t long.gif 2 120 6000     # over budget: fewer frames, same length
./t still.png 1 1 0
echo "decode tests passed"
