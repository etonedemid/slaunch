// Host preview of the 3D Mii as the menu draws it: Scene::Build's triangles,
// painted in order with no depth buffer (as SDL_RenderGeometry does), at
// three sizes and three moments. Needs mii-res (ShapeHigh.dat,
// NXTextureMidSRGB.dat, MiiBody.dat, anims.bin).
//   g++ -std=c++17 -O2 -I include scripts/mii/test_miibody.cpp source/sl/menu/mii/Mii*.cpp source/sl/menu/mii/texcompress_astc.cpp -lz -o /tmp/b && /tmp/b <res> <out.ppm> <studio hex>
#include <sl/menu/mii/MiiScene.hpp>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace sl::menu::mii;

static void Paint(std::vector<unsigned char> &px, int W, int H, const Image &tex, const std::vector<Vertex> &vs) {
    for (size_t i = 0; i + 2 < vs.size(); i += 3) {
        const Vertex *t = &vs[i];
        const float area = (t[1].x - t[0].x) * (t[2].y - t[0].y) - (t[2].x - t[0].x) * (t[1].y - t[0].y);
        if (std::fabs(area) < 1e-9f) continue;
        const int x0 = std::max(0, (int)std::floor(std::fmin(t[0].x, std::fmin(t[1].x, t[2].x))));
        const int x1 = std::min(W - 1, (int)std::ceil(std::fmax(t[0].x, std::fmax(t[1].x, t[2].x))));
        const int y0 = std::max(0, (int)std::floor(std::fmin(t[0].y, std::fmin(t[1].y, t[2].y))));
        const int y1 = std::min(H - 1, (int)std::ceil(std::fmax(t[0].y, std::fmax(t[1].y, t[2].y))));
        for (int py = y0; py <= y1; py++)
            for (int qx = x0; qx <= x1; qx++) {
                const float fx = qx + 0.5f, fy = py + 0.5f;
                const float w0 = ((t[1].x - fx) * (t[2].y - fy) - (t[2].x - fx) * (t[1].y - fy)) / area;
                const float w1 = ((t[2].x - fx) * (t[0].y - fy) - (t[0].x - fx) * (t[2].y - fy)) / area, w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float u = w0 * t[0].u + w1 * t[1].u + w2 * t[2].u, v = w0 * t[0].v + w1 * t[1].v + w2 * t[2].v;
                const int tx = std::min(tex.w - 1, std::max(0, (int)(u * tex.w))), ty = std::min(tex.h - 1, std::max(0, (int)(v * tex.h)));
                const uint8_t *tp = &tex.px[((size_t)ty * tex.w + tx) * 4];
                const float col[4] = { (w0 * t[0].r + w1 * t[1].r + w2 * t[2].r) * tp[0] / 255.0f,
                                       (w0 * t[0].g + w1 * t[1].g + w2 * t[2].g) * tp[1] / 255.0f,
                                       (w0 * t[0].b + w1 * t[1].b + w2 * t[2].b) * tp[2] / 255.0f,
                                       (w0 * t[0].a + w1 * t[1].a + w2 * t[2].a) * tp[3] / 255.0f };
                for (int k = 0; k < 3; k++) {
                    unsigned char &o = px[((size_t)py * W + qx) * 3 + k];
                    o = (unsigned char)std::fmin(255.0f, o * (1 - col[3]) + col[k] * 255.0f * col[3]);
                }
            }
    }
}

int main(int argc, char **argv) {
    assert(argc > 3);
    const std::string dir = argv[1];
    Parts base;
    assert(PartsFromStudioHex(argv[3], base));
    const int sizes[3][2] = { { 0, 0 }, { base.height, base.build }, { 127, 127 } };   // height, build
    const double times[3] = { 0.0, 3.0, 9.0 };
    const int cell = argc > 4 ? atoi(argv[4]) : 300, W = cell * 9, H = cell + 60;
    std::vector<unsigned char> px((size_t)W * H * 3, 45);
    for (int si = 0; si < 3; si++) {
        Parts p = base;
        p.height = (uint8_t)sizes[si][0];
        p.build = (uint8_t)sizes[si][1];
        if (const char *h = getenv("HAIR")) p.hair_type = (uint8_t)atoi(h);   // try a hairstyle
        Scene sc;
        assert(sc.Load((dir + "/ShapeHigh.dat").c_str(), (dir + "/NXTextureMidSRGB.dat").c_str(),
                       (dir + "/MiiBody.dat").c_str(), (dir + "/anims.bin").c_str(), p));
        std::vector<Vertex> vs;
        for (int ti = 0; ti < 3; ti++) {
            const auto t0 = std::chrono::steady_clock::now();
            sc.Build(times[ti], (float)((si * 3 + ti) * cell), 30.0f, (float)cell, 1.0f, vs);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (si == 1 && ti == 1) printf("tris %zu, build %.2f ms\n", vs.size() / 3, ms);
            Paint(px, W, H, sc.Atlas(), vs);
        }
    }
    FILE *f = fopen(argv[2], "wb");
    fprintf(f, "P6 %d %d 255\n", W, H);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
    puts("ok");
}
