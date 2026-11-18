// Host check for sl::menu::mii: every shape slot parses, and a Mii's head
// builds and renders. Needs the console's MiiModel files (see mii-res below).
//   g++ -std=c++17 -O1 -I include scripts/mii/test_miimodel.cpp source/sl/menu/mii/MiiModel.cpp -lz -o /tmp/t && /tmp/t <res dir> <out.ppm> [studio hex]
#include <sl/menu/mii/MiiModel.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdlib>
using namespace sl::menu::mii;

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    Resources r;
    assert(r.Load((dir + "/ShapeMid.dat").c_str(), (dir + "/NXTextureMidSRGB.dat").c_str()));

    // Every hairstyle and faceline builds.
    for (int h = 0; h < 132; h++) {
        Parts p; p.hair_type = (uint8_t)h; p.faceline_type = (uint8_t)(h % 12); p.nose_type = (uint8_t)(h % 18);
        auto head = r.BuildHead(p);
        assert(head.size() >= 4);
    }
    // Every texture with a size inflates to at least its mip 0.
    const int lens[11] = { 132, 62, 24, 2, 12, 12, 20, 2, 37, 6, 18 };
    int ntex = 0;
    for (int k = 0; k < 11; k++)
        for (int i = 0; i < lens[k]; i++) {
            Texture t;
            if (r.GetTexture((TexKind)k, i, t)) { ntex++; assert(t.data.size() >= (size_t)(t.width * t.height / 2)); }
        }
    printf("textures ok: %d\n", ntex);

    Parts p;
    if (argc > 3) assert(PartsFromStudioHex(argv[3], p));
    auto head = r.BuildHead(p);
    FaceTextures ft;
    assert(BuildFaceTextures(r, p, ft) && ft.mask.w == 512);
    auto dump = [&](const Image &im, const char *name) {
        if (argc <= 2 || !im.w) return;
        FILE *f = fopen((std::string(argv[2]) + "." + name + ".pam").c_str(), "wb");
        fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", im.w, im.h);
        fwrite(im.px.data(), 1, im.px.size(), f);
        fclose(f);
    };
    dump(ft.mask, "mask"); dump(ft.faceline, "faceline"); dump(ft.noseline, "noseline"); dump(ft.glasses, "glasses");
    const bool flipV = getenv("FLIPV") != nullptr;
    printf("meshes %zu (faceline %d hair %d nose %d)\n", head.size(), p.faceline_type, p.hair_type, p.nose_type);

    // Flat-shaded front view, 3/4 turned, through a z-buffer.
    const int W = 400, H = 400;
    std::vector<float> z(W * H, -1e9f);
    std::vector<unsigned char> img(W * H * 3, 40);
    const float yaw = 0.5f, cy = std::cos(yaw), sy = std::sin(yaw), sc = 4.0f, oy = 25.0f;
    for (const Mesh &m : head) {
        const Image *tex = m.kind == ShapeKind::Mask ? &ft.mask : m.kind == ShapeKind::Faceline && ft.faceline.w ? &ft.faceline
                         : m.kind == ShapeKind::NoseLine ? &ft.noseline : m.kind == ShapeKind::Glasses ? &ft.glasses : nullptr;
        if ((m.kind == ShapeKind::Mask || m.kind == ShapeKind::NoseLine || m.kind == ShapeKind::Glasses) && (!tex || !tex->w || m.uv.empty())) continue;
        auto P = [&](int i, float &x, float &y, float &d) {
            const float px = m.pos[i * 3], py = m.pos[i * 3 + 1], pz = m.pos[i * 3 + 2];
            const float rx = px * cy + pz * sy, rz = -px * sy + pz * cy;
            x = W / 2 + rx * sc; y = H / 2 - (py - oy) * sc; d = rz;
        };
        for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
            float x[3], y[3], d[3];
            for (int k = 0; k < 3; k++) P(m.idx[t + k], x[k], y[k], d[k]);
            const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
            if (std::fabs(area) < 1e-6f) continue;
            auto light = [&](float w0, float w1, float w2) {
                if (m.nrm.empty()) return 0.8f;
                float n[3];
                for (int c = 0; c < 3; c++) n[c] = w0 * m.nrm[m.idx[t] * 3 + c] + w1 * m.nrm[m.idx[t + 1] * 3 + c] + w2 * m.nrm[m.idx[t + 2] * 3 + c];
                const float nx = n[0] * cy + n[2] * sy, ny = n[1], nz = -n[0] * sy + n[2] * cy, len = std::sqrt(nx * nx + ny * ny + nz * nz) + 1e-6f;
                return 0.45f + 0.6f * std::fmax(0.0f, (nx * -0.3f + ny * 0.4f + nz * 0.87f) / len);
            };
            const int x0 = std::max(0, (int)std::floor(std::fmin(x[0], std::fmin(x[1], x[2])))), x1 = std::min(W - 1, (int)std::ceil(std::fmax(x[0], std::fmax(x[1], x[2]))));
            const int y0 = std::max(0, (int)std::floor(std::fmin(y[0], std::fmin(y[1], y[2])))), y1 = std::min(H - 1, (int)std::ceil(std::fmax(y[0], std::fmax(y[1], y[2]))));
            for (int py = y0; py <= y1; py++)
                for (int px = x0; px <= x1; px++) {
                    const float w0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area;
                    const float w1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area;
                    const float w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    const float dd = w0 * d[0] + w1 * d[1] + w2 * d[2];
                    const bool decal = m.kind == ShapeKind::Mask || m.kind == ShapeKind::NoseLine || m.kind == ShapeKind::Glasses;
                    if (dd <= z[py * W + px] - (decal ? 0.5f : 0.0f)) continue;
                    float col[4] = { m.color[0], m.color[1], m.color[2], 1 };
                    if (tex && tex->w && !m.uv.empty()) {
                        const int *ix = nullptr; (void)ix;
                        const float u = w0 * m.uv[m.idx[t] * 2] + w1 * m.uv[m.idx[t + 1] * 2] + w2 * m.uv[m.idx[t + 2] * 2];
                        float v = w0 * m.uv[m.idx[t] * 2 + 1] + w1 * m.uv[m.idx[t + 1] * 2 + 1] + w2 * m.uv[m.idx[t + 2] * 2 + 1];
                        if (flipV) v = 1 - v;
                        float uu = u; if (uu > 1) uu = 2 - uu;   // faceline and glasses: half images, mirrored
                        const float u2 = uu; (void)u2;
                        const int tx = std::min(tex->w - 1, std::max(0, (int)(u2 * tex->w))), ty = std::min(tex->h - 1, std::max(0, (int)(v * tex->h)));
                        const uint8_t *tp = &tex->px[((size_t)ty * tex->w + tx) * 4];
                        for (int c = 0; c < 4; c++) col[c] = tp[c] / 255.0f;
                    }
                    if (col[3] <= 0.01f) continue;
                    const float l = light(w0, w1, w2);
                    if (!decal) z[py * W + px] = dd;
                    for (int c = 0; c < 3; c++) {
                        unsigned char &o = img[(py * W + px) * 3 + c];
                        o = (unsigned char)std::fmin(255.0f, o * (1 - col[3]) + col[c] * l * 255.0f * col[3]);
                    }
                }
        }
    }
    if (argc > 2) {
        FILE *f = fopen(argv[2], "wb");
        fprintf(f, "P6 %d %d 255\n", W, H);
        fwrite(img.data(), 1, img.size(), f);
        fclose(f);
    }
    puts("ok");
    return 0;
}
