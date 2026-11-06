#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>

// BC1 (DXT1) encoder for opaque art: 4x4 pixels into 8 bytes, 1/8 the memory of
// RGBA, and the GPU reads it directly. The end points are the block's colour
// bounding box, inset a little and flipped along the box's real diagonal (after
// van Waveren, "Real-Time DXT Compression"): fast enough for the art worker, with
// quality close to a full search on the photographs this is used for.
// No SDL or libnx in here, so scripts/qext/test_bc1.cpp can run it on a PC.
namespace sl::menu::gfx::bc1 {

    inline size_t Size(int w, int h) { return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 8; }

    inline uint16_t To565(const int c[3]) {
        return (uint16_t)((((c[0] * 31 + 127) / 255) << 11) | (((c[1] * 63 + 127) / 255) << 5) |
                          ((c[2] * 31 + 127) / 255));
    }
    inline void From565(uint16_t v, int c[3]) {
        const int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        c[0] = (r << 3) | (r >> 2);
        c[1] = (g << 2) | (g >> 4);
        c[2] = (b << 3) | (b >> 2);
    }

    // block: 16 RGBA pixels, in rows.
    inline void EncodeBlock(const uint8_t *block, uint8_t out[8]) {
        int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
        for (int i = 0; i < 16; i++)
            for (int k = 0; k < 3; k++) {
                const int v = block[i * 4 + k];
                if (v < lo[k]) lo[k] = v;
                if (v > hi[k]) hi[k] = v;
            }
        // The box has four diagonals. Pick the one the pixels lie along: green
        // stays as it is, red and blue swap ends where they run against it.
        const int mid[3] = { (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2 };
        int rg = 0, bg = 0;
        for (int i = 0; i < 16; i++) {
            const int g = block[i * 4 + 1] - mid[1];
            rg += (block[i * 4] - mid[0]) * g;
            bg += (block[i * 4 + 2] - mid[2]) * g;
        }
        int a[3] = { hi[0], hi[1], hi[2] }, b[3] = { lo[0], lo[1], lo[2] };
        if (rg < 0) { a[0] = lo[0]; b[0] = hi[0]; }
        if (bg < 0) { a[2] = lo[2]; b[2] = hi[2]; }
        // Inset by 1/16: the outliers pull a bounding box too wide.
        for (int k = 0; k < 3; k++) {
            const int d = (a[k] - b[k]) / 16;
            a[k] -= d;
            b[k] += d;
        }
        uint16_t c0 = To565(a), c1 = To565(b);
        if (c0 < c1) { const uint16_t t = c0; c0 = c1; c1 = t; }   // c0 > c1: four colours

        uint32_t bits = 0;
        if (c0 != c1) {
            int p[4][3];
            From565(c0, p[0]);
            From565(c1, p[1]);
            for (int k = 0; k < 3; k++) {
                p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
                p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
            }
            for (int i = 0; i < 16; i++) {
                int best = 0, best_d = 0x7fffffff;
                for (int s = 0; s < 4; s++) {
                    const int dr = block[i * 4] - p[s][0];
                    const int dg = block[i * 4 + 1] - p[s][1];
                    const int db = block[i * 4 + 2] - p[s][2];
                    const int d = dr * dr + dg * dg + db * db;
                    if (d < best_d) { best_d = d; best = s; }
                }
                bits |= (uint32_t)best << (i * 2);
            }
        }
        out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8);
        out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
        out[4] = (uint8_t)bits; out[5] = (uint8_t)(bits >> 8);
        out[6] = (uint8_t)(bits >> 16); out[7] = (uint8_t)(bits >> 24);
    }

    // rgba: w x h pixels, `pitch` bytes a row. Edges past the image repeat its last
    // pixels. Returns malloc'd data of Size(w, h) bytes, or null. Alpha is ignored.
    inline uint8_t *Encode(const uint8_t *rgba, int w, int h, int pitch) {
        if (!rgba || w <= 0 || h <= 0) return nullptr;
        uint8_t *out = (uint8_t *)malloc(Size(w, h));
        if (!out) return nullptr;
        uint8_t *o = out;
        uint8_t block[64];
        for (int y = 0; y < h; y += 4)
            for (int x = 0; x < w; x += 4) {
                for (int j = 0; j < 4; j++) {
                    const uint8_t *row = rgba + (size_t)(y + j < h ? y + j : h - 1) * pitch;
                    for (int i = 0; i < 4; i++) {
                        const uint8_t *px = row + (size_t)(x + i < w ? x + i : w - 1) * 4;
                        block[(j * 4 + i) * 4 + 0] = px[0];
                        block[(j * 4 + i) * 4 + 1] = px[1];
                        block[(j * 4 + i) * 4 + 2] = px[2];
                    }
                }
                EncodeBlock(block, o);
                o += 8;
            }
        return out;
    }

}
