// g++ -std=c++17 -O2 -fsanitize=address,undefined -I projects/sMenu/include scripts/qext/test_bc1.cpp -o /tmp/tb && /tmp/tb
#include "sl/menu/gfx/Bc1.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace sl::menu::gfx;

// A decoder per the BC1 spec, in the four-colour mode the encoder must always use.
static void Decode(const uint8_t *in, int w, int h, std::vector<uint8_t> &out) {
    out.assign((size_t)w * h * 3, 0);
    const uint8_t *b = in;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4, b += 8) {
            const uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
            const uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
            assert(c0 >= c1);
            int p[4][3];
            bc1::From565(c0, p[0]); bc1::From565(c1, p[1]);
            for (int k = 0; k < 3; k++) {
                p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
                p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
            }
            for (int i = 0; i < 16; i++) {
                const int px = x + i % 4, py = y + i / 4;
                if (px >= w || py >= h) continue;
                const int s = (bits >> (i * 2)) & 3;
                for (int k = 0; k < 3; k++) out[(size_t)(py * w + px) * 3 + k] = (uint8_t)p[s][k];
            }
        }
}

static double Psnr(int w, int h, int pad, void (*fill)(int, int, uint8_t *)) {
    const int pitch = w * 4 + pad;
    std::vector<uint8_t> src((size_t)pitch * h, 0xEE);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) fill(x, y, &src[(size_t)y * pitch + (size_t)x * 4]);
    uint8_t *enc = bc1::Encode(src.data(), w, h, pitch);
    assert(enc);
    std::vector<uint8_t> dec;
    Decode(enc, w, h, dec);
    double err = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int k = 0; k < 3; k++) {
                const double d = (double)src[(size_t)y * pitch + (size_t)x * 4 + k] - dec[(size_t)(y * w + x) * 3 + k];
                err += d * d;
            }
    free(enc);
    err /= (double)w * h * 3;
    return err == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / err);
}

int main() {
    assert(bc1::Size(480, 720) == 21600 * 8);
    assert(bc1::Size(516, 290) == 129 * 73 * 8);   // rounds up to whole blocks
    assert(bc1::Size(1, 1) == 8);

    auto flat = [](int, int, uint8_t *p) { p[0] = 200; p[1] = 40; p[2] = 90; p[3] = 255; };
    auto grad = [](int x, int y, uint8_t *p) { p[0] = x * 255 / 480; p[1] = y * 255 / 720; p[2] = 128; p[3] = 255; };
    // Red rising as green falls: only right if the diagonal flip works.
    auto anti = [](int x, int, uint8_t *p) { p[0] = x % 256; p[1] = 255 - x % 256; p[2] = 30; p[3] = 255; };
    auto photo = [](int x, int y, uint8_t *p) {
        p[0] = (uint8_t)(128 + 90 * sin(x * .03) + 30 * sin(y * .11));
        p[1] = (uint8_t)(128 + 80 * cos(y * .02) + 25 * sin((x + y) * .07));
        p[2] = (uint8_t)(120 + 70 * sin((x - y) * .025));
        p[3] = 255;
    };
    const double f = Psnr(64, 64, 0, flat), g = Psnr(480, 720, 12, grad),
                 a = Psnr(256, 64, 0, anti), ph = Psnr(516, 290, 0, photo);
    printf("psnr flat %.1f gradient %.1f anti %.1f photo-like %.1f\n", f, g, a, ph);
    fflush(stdout);
    assert(f > 44 && g > 38 && a > 35 && ph > 38);

    std::vector<uint8_t> big((size_t)480 * 720 * 4);
    for (int y = 0; y < 720; y++) for (int x = 0; x < 480; x++) photo(x, y, &big[(size_t)y * 1920 + (size_t)x * 4]);
    const auto t0 = std::chrono::steady_clock::now();
    uint8_t *e = bc1::Encode(big.data(), 480, 720, 1920);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    free(e);
    printf("480x720 in %.1f ms on this PC\n", ms);
    puts("bc1 ok");
}
