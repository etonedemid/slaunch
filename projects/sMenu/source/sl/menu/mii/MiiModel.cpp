#include <sl/menu/mii/MiiModel.hpp>
#include "MiiColors.hpp"
#include <zlib.h>
#include <cstdio>
#include <cstring>

namespace sl::menu::mii {

    namespace {
        // ---- file layout (little endian) --------------------------------
        // NFSR: magic, ver, file_size, max_size[12], max_alignment[12], then
        // ShapeElement tables, then hair and faceline transforms.
        constexpr size_t kShapeHdr = 4 + 4 + 4 + 48 + 48;
        constexpr size_t kShapeEl  = 88;   // common 16 + attribute 72
        // Table order and lengths in the file.
        enum ShapeTable { ST_Beard, ST_Faceline, ST_Mask, ST_HatNormal, ST_HatCap, ST_ForeheadNormal, ST_ForeheadCap,
                          ST_HairNormal, ST_HairCap, ST_Glasses, ST_Nose, ST_NoseLine, ST_Count };
        constexpr int kShapeLen[ST_Count] = { 4, 12, 12, 132, 132, 132, 132, 132, 132, 1, 18, 18 };
        constexpr size_t kHairXfSize = 72, kFacelineXfSize = 36;

        // NFTR: magic, ver, file_size, max_size[11], max_alignment[11], then
        // TextureElement tables in TexKind order.
        constexpr size_t kTexHdr = 4 + 4 + 4 + 44 + 44;
        constexpr size_t kTexEl  = 28;     // common 16 + attribute 12
        constexpr int kTexLen[11] = { 132, 62, 24, 2, 12, 12, 20, 2, 37, 6, 18 };

        size_t ShapeTableOffset(int table) {
            size_t o = kShapeHdr;
            for (int t = 0; t < table; t++) o += kShapeLen[t] * kShapeEl;
            return o;
        }
        size_t ShapeTablesEnd() { return ShapeTableOffset(ST_Count); }

        uint32_t U32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
        uint16_t U16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
        float    F32(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

        bool ReadFile(const char *path, std::vector<uint8_t> &out) {
            FILE *f = fopen(path, "rb");
            if (!f) return false;
            fseek(f, 0, SEEK_END);
            const long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            out.resize(n > 0 ? (size_t)n : 0);
            const bool ok = n > 0 && fread(out.data(), 1, out.size(), f) == out.size();
            fclose(f);
            if (!ok) out.clear();
            return ok;
        }

        // One zlib stream at [off, off+csize) of file, inflated to `size` bytes.
        bool Inflate(const std::vector<uint8_t> &file, uint32_t off, uint32_t csize, uint32_t size,
                     std::vector<uint8_t> &out) {
            if ((uint64_t)off + csize > file.size() || size == 0) return false;
            out.resize(size);
            uLongf n = size;
            if (uncompress(out.data(), &n, file.data() + off, csize) != Z_OK || n != size) { out.clear(); return false; }
            return true;
        }

        void SetColor(float dst[4], const float src[4]) { memcpy(dst, src, sizeof(float) * 4); }
    }

    float HalfToFloat(uint16_t h) {
        const uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
        uint32_t bits;
        if (e == 0) {
            if (m == 0) bits = s;
            else {   // subnormal
                int ee = -1;
                uint32_t mm = m;
                do { ee++; mm <<= 1; } while (!(mm & 0x400));
                bits = s | ((uint32_t)(127 - 15 - ee) << 23) | ((mm & 0x3FF) << 13);
            }
        } else if (e == 31) bits = s | 0x7F800000 | (m << 13);
        else bits = s | ((e + 127 - 15) << 23) | (m << 13);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }

    bool PartsFromStudio(const uint8_t b[46], Parts &p) {
        p.beard_color = b[0];      p.beard_type = b[1];        p.build = b[2];
        p.eye_aspect = b[3];       p.eye_color = b[4];         p.eye_rotate = b[5];
        p.eye_scale = b[6];        p.eye_type = b[7];          p.eye_x = b[8];          p.eye_y = b[9];
        p.eyebrow_aspect = b[10];  p.eyebrow_color = b[11];    p.eyebrow_rotate = b[12];
        p.eyebrow_scale = b[13];   p.eyebrow_type = b[14];     p.eyebrow_x = b[15];     p.eyebrow_y = b[16];
        p.faceline_color = b[17];  p.faceline_make = b[18];    p.faceline_type = b[19]; p.faceline_wrinkle = b[20];
        p.favorite_color = b[21];  p.gender = b[22];
        p.glass_color = b[23];     p.glass_scale = b[24];      p.glass_type = b[25];    p.glass_y = b[26];
        p.hair_color = b[27];      p.hair_flip = b[28];        p.hair_type = b[29];     p.height = b[30];
        p.mole_scale = b[31];      p.mole_type = b[32];        p.mole_x = b[33];        p.mole_y = b[34];
        p.mouth_aspect = b[35];    p.mouth_color = b[36];      p.mouth_scale = b[37];   p.mouth_type = b[38];
        p.mouth_y = b[39];
        p.mustache_scale = b[40];  p.mustache_type = b[41];    p.mustache_y = b[42];
        p.nose_scale = b[43];      p.nose_type = b[44];        p.nose_y = b[45];
        // Index ranges the tables below are sized for.
        return p.faceline_type < 12 && p.faceline_color < 10 && p.hair_type < 132 && p.hair_color < 100 &&
               p.nose_type < 18 && p.eye_type < 62 && p.eyebrow_type < 24 && p.mouth_type < 37 &&
               p.beard_color < 100 && p.glass_color < 100 && p.favorite_color < 12 && p.mustache_type < 6;
    }

    bool PartsFromStudioHex(const std::string &hex, Parts &out) {
        if (hex.size() != 94) return false;
        uint8_t raw[47], b[46];
        for (int i = 0; i < 47; i++) {
            unsigned v;
            if (sscanf(hex.c_str() + i * 2, "%2x", &v) != 1) return false;
            raw[i] = (uint8_t)v;
        }
        uint8_t prev = raw[0];
        for (int i = 0; i < 46; i++) { b[i] = (uint8_t)(((uint8_t)(raw[i + 1] - 7)) ^ prev); prev = raw[i + 1]; }
        return PartsFromStudio(b, out);
    }

    bool Resources::Load(const char *shape_path, const char *texture_path) {
        m_shape.clear();
        m_tex.clear();
        std::vector<uint8_t> s, t;
        if (!ReadFile(shape_path, s) || !ReadFile(texture_path, t)) return false;
        if (s.size() < ShapeTablesEnd() + 132 * kHairXfSize + 12 * kFacelineXfSize || memcmp(s.data(), "NFSR", 4)) return false;
        size_t texEnd = kTexHdr;
        for (int n : kTexLen) texEnd += n * kTexEl;
        if (t.size() < texEnd || memcmp(t.data(), "NFTR", 4)) return false;
        m_shape.swap(s);
        m_tex.swap(t);
        return true;
    }

    // Element `index` of shape table `table`: positions, normals, uvs, indices.
    bool Resources::ShapeMesh(int table, int index, Mesh &out) const {
        if (table < 0 || table >= ST_Count || index < 0 || index >= kShapeLen[table]) return false;
        const uint8_t *e = m_shape.data() + ShapeTableOffset(table) + (size_t)index * kShapeEl;
        const uint32_t off = U32(e), size = U32(e + 4), csize = U32(e + 8);
        if (size == 0) return false;   // an unused slot
        uint32_t attr_off[5], attr_size[5];
        for (int i = 0; i < 5; i++) { attr_off[i] = U32(e + 16 + i * 4); attr_size[i] = U32(e + 36 + i * 4); }
        const uint32_t idx_off = U32(e + 56), idx_size = U32(e + 60);

        std::vector<uint8_t> d;
        if (!Inflate(m_shape, off, csize, size, d)) return false;
        auto in = [&](uint32_t o, uint32_t n) { return (uint64_t)o + n <= d.size(); };

        const uint32_t nv = attr_size[0] / 8;    // position: 4 x half
        if (!nv || !in(attr_off[0], nv * 8) || !in(idx_off, idx_size)) return false;
        out.pos.resize(nv * 3);
        for (uint32_t v = 0; v < nv; v++)
            for (int c = 0; c < 3; c++) out.pos[v * 3 + c] = HalfToFloat(U16(d.data() + attr_off[0] + v * 8 + c * 2));
        out.nrm.clear();
        if (attr_size[1] && in(attr_off[1], nv * 4)) {   // normal: 10_10_10_2 snorm
            out.nrm.resize(nv * 3);
            for (uint32_t v = 0; v < nv; v++) {
                const uint32_t pk = U32(d.data() + attr_off[1] + v * 4);
                out.nrm[v * 3 + 0] = (float)((int32_t)(pk << 22) >> 22) / 511.0f;
                out.nrm[v * 3 + 1] = (float)((int32_t)(pk << 12) >> 22) / 511.0f;
                out.nrm[v * 3 + 2] = (float)((int32_t)(pk << 2) >> 22) / 511.0f;
            }
        }
        out.uv.clear();
        if (attr_size[2] && in(attr_off[2], nv * 4)) {   // uv: 2 x half
            out.uv.resize(nv * 2);
            for (uint32_t v = 0; v < nv; v++)
                for (int c = 0; c < 2; c++) out.uv[v * 2 + c] = HalfToFloat(U16(d.data() + attr_off[2] + v * 4 + c * 2));
        }
        out.idx.resize(idx_size / 2);
        memcpy(out.idx.data(), d.data() + idx_off, out.idx.size() * 2);
        for (uint16_t i : out.idx) if (i >= nv) return false;
        return true;
    }

    std::vector<Mesh> Resources::BuildHead(const Parts &p) const {
        std::vector<Mesh> out;
        if (!Loaded() || p.faceline_type >= 12) return out;

        // Where hair, nose and beard sit for this faceline.
        const uint8_t *fx = m_shape.data() + ShapeTablesEnd() + 132 * kHairXfSize + p.faceline_type * kFacelineXfSize;
        const float hair_t[3]  = { F32(fx + 0),  F32(fx + 4),  F32(fx + 8) };
        const float nose_t[3]  = { F32(fx + 12), F32(fx + 16), F32(fx + 20) };
        const float beard_t[3] = { F32(fx + 24), F32(fx + 28), F32(fx + 32) };
        static const float kNone[4] = { 0, 0, 0, 0 };

        auto add = [&](ShapeKind kind, int table, int index, const float t[3], const float s[3], const float *color) {
            Mesh m;
            m.kind = kind;
            if (!ShapeMesh(table, index, m)) return;
            for (size_t v = 0; v < m.pos.size(); v += 3)
                for (int c = 0; c < 3; c++) m.pos[v + c] = m.pos[v + c] * s[c] + t[c];
            if (s[0] < 0) {   // mirrored hair: flip normals and winding back the right way round
                for (size_t v = 0; v < m.nrm.size(); v += 3) m.nrm[v] = -m.nrm[v];
                for (size_t i = 0; i + 2 < m.idx.size(); i += 3) std::swap(m.idx[i + 1], m.idx[i + 2]);
            }
            SetColor(m.color, color ? color : kNone);
            out.push_back(std::move(m));
        };

        const float zero[3] = { 0, 0, 0 }, one[3] = { 1, 1, 1 };
        const float *skin = kFacelineColor[p.faceline_color];
        add(ShapeKind::Faceline, ST_Faceline, p.faceline_type, zero, one, skin);
        add(ShapeKind::Mask, ST_Mask, p.faceline_type, zero, one, nullptr);

        const float hs[3] = { p.hair_flip ? -1.0f : 1.0f, 1, 1 };
        add(ShapeKind::Hair, ST_HairNormal, p.hair_type, hair_t, hs, kCommonColor[p.hair_color]);
        add(ShapeKind::Forehead, ST_ForeheadNormal, p.hair_type, hair_t, hs, skin);

        const float ns = 0.175f * p.nose_scale + 0.4f;
        const float nscale[3] = { ns, ns, ns };
        const float npos[3] = { nose_t[0], nose_t[1] + (p.nose_y - 8.0f) * -1.5f, nose_t[2] };
        add(ShapeKind::Nose, ST_Nose, p.nose_type, npos, nscale, skin);
        add(ShapeKind::NoseLine, ST_NoseLine, p.nose_type, npos, nscale, nullptr);

        if (p.beard_type != 0 && p.beard_type < 4)
            add(ShapeKind::Beard, ST_Beard, p.beard_type, beard_t, one, kCommonColor[p.beard_color]);

        if (p.glass_type != 0) {
            const float gs = 0.15f * p.glass_scale + 0.4f;
            const float gscale[3] = { gs, gs, gs };
            const float gpos[3] = { nose_t[0], nose_t[1] + (p.glass_y - 11.0f) * -1.5f + 5.0f, nose_t[2] + 2.0f };
            add(ShapeKind::Glasses, ST_Glasses, 0, gpos, gscale, nullptr);
        }
        return out;
    }

    bool Resources::GetTexture(TexKind kind, int index, Texture &out) const {
        const int k = (int)kind;
        if (!Loaded() || k < 0 || k >= 11 || index < 0 || index >= kTexLen[k]) return false;
        size_t o = kTexHdr;
        for (int t = 0; t < k; t++) o += kTexLen[t] * kTexEl;
        const uint8_t *e = m_tex.data() + o + (size_t)index * kTexEl;
        out.width  = U16(e + 20);
        out.height = U16(e + 22);
        out.format = e[24];
        out.mips   = e[25];
        if (!out.width || !out.height || U32(e + 4) == 0) return false;
        return Inflate(m_tex, U32(e), U32(e + 8), U32(e + 4), out.data);
    }
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
#include "astc_shim.h"
#include <cmath>

namespace sl::menu::mii {

    namespace {
        int DivUp(int a, int b) { return (a + b - 1) / b; }

        // Tegra block-linear: 64-byte x 8-row GOBs, stacked `bh` GOBs tall.
        // bh for mip 0 as the NVN/tegra_swizzle rule picks it.
        int BlockHeightMip0(int rows) {
            const int r = rows + rows / 2;
            return r >= 128 ? 16 : r >= 64 ? 8 : r >= 32 ? 4 : r >= 16 ? 2 : 1;
        }
        size_t GobOffset(int x, int y) {
            return (size_t)(((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16));
        }
        // Deswizzles mip 0: `wb` x `hb` elements of `bpe` bytes.
        bool Deswizzle(const std::vector<uint8_t> &in, int wb, int hb, int bpe, std::vector<uint8_t> &out) {
            const int bh = BlockHeightMip0(hb), rowBytes = wb * bpe, gobsX = DivUp(rowBytes, 64);
            const size_t blockBytes = (size_t)512 * bh;
            out.assign((size_t)rowBytes * hb, 0);
            for (int y = 0; y < hb; y++)
                for (int xb = 0; xb < rowBytes; xb++) {
                    const size_t src = (size_t)(y / (8 * bh)) * gobsX * blockBytes + (size_t)(xb / 64) * blockBytes +
                                       (size_t)((y % (8 * bh)) / 8) * 512 + GobOffset(xb, y);
                    if (src >= in.size()) return false;
                    out[(size_t)y * rowBytes + xb] = in[src];
                }
            return true;
        }

        // One BC4 channel block (8 bytes) to 16 values.
        void Bc4Block(const uint8_t *b, uint8_t v[16]) {
            const int a0 = b[0], a1 = b[1];
            int pal[8] = { a0, a1 };
            if (a0 > a1) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
            else { for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5; pal[6] = 0; pal[7] = 255; }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
            for (int i = 0; i < 16; i++) v[i] = (uint8_t)pal[(bits >> (3 * i)) & 7];
        }
    }

    bool Decode(const Texture &t, std::vector<uint8_t> &rgba) {
        const int w = t.width, h = t.height;
        rgba.assign((size_t)w * h * 4, 0);
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        std::vector<uint8_t> lin;
        switch (t.format) {
        case 0: case 1: case 2: {   // R, RG, RGBA
            const int bpp = t.format == 0 ? 1 : t.format == 1 ? 2 : 4;
            if (!Deswizzle(t.data, w, h, bpp, lin)) return false;
            for (int i = 0; i < w * h; i++)
                for (int c = 0; c < bpp; c++) rgba[(size_t)i * 4 + c] = lin[(size_t)i * bpp + c];
            return true;
        }
        case 3: case 4: {           // BC4, BC5
            const int bpe = t.format == 3 ? 8 : 16, xb = DivUp(w, 4), yb = DivUp(h, 4);
            if (!Deswizzle(t.data, xb, yb, bpe, lin)) return false;
            for (int by = 0; by < yb; by++)
                for (int bx = 0; bx < xb; bx++) {
                    const uint8_t *blk = lin.data() + ((size_t)by * xb + bx) * bpe;
                    for (int ch = 0; ch < bpe / 8; ch++) {
                        uint8_t v[16];
                        Bc4Block(blk + ch * 8, v);
                        for (int i = 0; i < 16; i++) {
                            const int x = bx * 4 + i % 4, y = by * 4 + i / 4;
                            if (x < w && y < h) rgba[((size_t)y * w + x) * 4 + ch] = v[i];
                        }
                    }
                }
            return true;
        }
        case 6: {                   // ASTC 4x4 (Mesa's decoder)
            if (!Deswizzle(t.data, DivUp(w, 4), DivUp(h, 4), 16, lin)) return false;
            sl_astc4x4_decode(rgba.data(), lin.data(), (unsigned)w, (unsigned)h, false);
            return true;
        }
        default:
            return false;           // BC7 is not used by the Mid textures
        }
    }

    namespace {
        // FFL's modulation modes (vee shader.wgsl), applied to raw channels.
        enum Mode { M_Direct, M_LayeredRgb, M_Alpha, M_LumAlpha, M_Lum };
        void Modulate(std::vector<uint8_t> &px, Mode m, const float *c0, const float *c1 = nullptr, const float *c2 = nullptr) {
            for (size_t i = 0; i < px.size(); i += 4) {
                const float r = px[i] / 255.0f, g = px[i + 1] / 255.0f, b = px[i + 2] / 255.0f, a = px[i + 3] / 255.0f;
                float o[4] = { r, g, b, a };
                switch (m) {
                case M_Direct: break;
                case M_LayeredRgb:
                    for (int c = 0; c < 3; c++) o[c] = r * c0[c] + g * c1[c] + b * c2[c];
                    o[3] = a;
                    break;
                case M_Alpha:    for (int c = 0; c < 3; c++) o[c] = c0[c]; o[3] = r; break;
                case M_LumAlpha: for (int c = 0; c < 3; c++) o[c] = r * c0[c]; o[3] = g; break;
                case M_Lum:      for (int c = 0; c < 3; c++) o[c] = r * c0[c]; o[3] = 1; break;
                }
                for (int c = 0; c < 4; c++) px[i + c] = (uint8_t)std::lround(std::fmin(1.0f, std::fmax(0.0f, o[c])) * 255.0f);
            }
        }

        void Fill(Image &img, int w, int h, const float *c) {
            img.w = w; img.h = h;
            img.px.assign((size_t)w * h * 4, 0);
            if (c) for (size_t i = 0; i < img.px.size(); i += 4)
                for (int k = 0; k < 4; k++) img.px[i + k] = (uint8_t)std::lround(c[k] * 255.0f);
        }

        // Bilinear sample of a straight-alpha image, premultiplied out.
        void Sample(const std::vector<uint8_t> &src, int sw, int sh, float u, float v, float out[4]) {
            const float fx = u * sw - 0.5f, fy = v * sh - 0.5f;
            const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
            const float tx = fx - x0, ty = fy - y0;
            for (int c = 0; c < 4; c++) out[c] = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    const int x = std::min(sw - 1, std::max(0, x0 + i)), y = std::min(sh - 1, std::max(0, y0 + j));
                    const float wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
                    const uint8_t *p = &src[((size_t)y * sw + x) * 4];
                    const float a = p[3] / 255.0f;
                    for (int c = 0; c < 3; c++) out[c] += wgt * a * p[c] / 255.0f;
                    out[3] += wgt * a;
                }
        }

        // Source-over of a premultiplied sample onto straight-alpha dst.
        void Over(uint8_t *d, const float s[4]) {
            const float da = d[3] / 255.0f, oa = s[3] + da * (1 - s[3]);
            for (int c = 0; c < 3; c++) {
                const float dc = d[c] / 255.0f * da;
                const float oc = oa > 0 ? (s[c] + dc * (1 - s[3])) / oa : 0;
                d[c] = (uint8_t)std::lround(std::fmin(1.0f, oc) * 255.0f);
            }
            d[3] = (uint8_t)std::lround(std::fmin(1.0f, oa) * 255.0f);
        }

        // A full-size layer over `dst` (faceline wrinkles/makeup/beard, glasses, noseline).
        void DrawFull(Image &dst, const std::vector<uint8_t> &src, int sw, int sh) {
            for (int y = 0; y < dst.h; y++)
                for (int x = 0; x < dst.w; x++) {
                    float s[4];
                    Sample(src, sw, sh, (x + 0.5f) / dst.w, (y + 0.5f) / dst.h, s);
                    Over(&dst.px[((size_t)y * dst.w + x) * 4], s);
                }
        }

        // ---- mask placement (RFL/FFL, as vee positioning.rs has it) --------
        constexpr float kTexScaleX = 0.88961464f, kTexScaleY = 0.9276675f, kMagicY = 1.1600001f;
        float Scale2Dim(float s) { return 1.0f + 0.4f * s; }
        float Rot2Ang(int r) { return (360.0f / 32.0f) * (float)(r % 32); }
        constexpr float TexUnit(float x) { return x / 64.0f; }
        const uint8_t kEyeRotOffset[62] = {
            3, 4, 4, 4, 3, 4, 4, 4, 3, 4, 4, 4, 4, 3, 3, 4, 4, 4, 3, 3, 4, 3, 4, 3, 3, 4, 3, 4, 4, 3, 4,
            4, 4, 3, 3, 3, 4, 4, 3, 3, 3, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 3, 4, 4, 3, 4, 4 };
        const uint8_t kBrowRotOffset[24] = {
            26, 26, 27, 25, 26, 25, 26, 25, 28, 25, 26, 24, 27, 27, 26, 26, 25, 25, 26, 26, 27, 26, 25, 27 };
        enum Origin { O_Center, O_Left, O_Right };

        // One part quad: centre/edge at (x, y) in y-down pixels of a res x res
        // target, w x h, turned by `deg` (the RFL quad() geometry, y-up inside).
        void DrawPart(Image &dst, const std::vector<uint8_t> &src, int sw, int sh,
                      float x, float y, float w, float h, float deg, Origin o) {
            if (w <= 0 || h <= 0) return;
            const float res = (float)dst.w, sx = w * kTexScaleX, sy = h * kTexScaleY;
            const float baseX = o == O_Center ? -0.5f : o == O_Right ? -1.0f : 0.0f;
            const float a = deg * 3.14159265f / 180.0f, ca = std::cos(a), sa = std::sin(a);
            // Bounds of the turned quad: local (lx, ly) -> screen offset (dx, -dy).
            float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
            for (int c = 0; c < 4; c++) {
                const float lx = (baseX + (c & 1)) * sx, ly = ((c >> 1) - 0.5f) * sy;
                const float dx = lx * ca + ly * sa, dy = -lx * sa + ly * ca;
                bx0 = std::min(bx0, dx); bx1 = std::max(bx1, dx); by0 = std::min(by0, -dy); by1 = std::max(by1, -dy);
            }
            const int x0 = std::max(0, (int)(x + bx0) - 2), x1 = std::min(dst.w - 1, (int)(x + bx1) + 2);
            const int y0 = std::max(0, (int)(y + by0) - 2), y1 = std::min(dst.h - 1, (int)(y + by1) + 2);
            for (int py = y0; py <= y1; py++)
                for (int px = x0; px <= x1; px++) {
                    // y-up offset from the anchor, turned back by +deg (the quad turns by -deg).
                    const float dx = px + 0.5f - x, dy = (res - (py + 0.5f)) - (res - y);
                    const float lx = (dx * ca - dy * sa) / sx, ly = (dx * sa + dy * ca) / sy;
                    if (lx < baseX || lx > baseX + 1 || ly < -0.5f || ly > 0.5f) continue;
                    const float u = o == O_Left ? 1.0f - lx : lx - baseX;   // the left part is the mirrored image
                    const float v = 0.5f - ly;                                // image rows run top down
                    float s[4];
                    Sample(src, sw, sh, u, v, s);
                    if (s[3] <= 0) continue;
                    Over(&dst.px[((size_t)py * dst.w + px) * 4], s);
                }
        }

        bool Tex(const Resources &r, TexKind k, int i, std::vector<uint8_t> &px, int &w, int &h) {
            Texture t;
            if (!r.GetTexture(k, i, t) || !Decode(t, px)) return false;
            w = t.width; h = t.height;
            return true;
        }
    }

    bool BuildFaceTextures(const Resources &r, const Parts &p, FaceTextures &out) {
        static const float kBlack[4] = { 0, 0, 0, 1 };
        out = {};
        std::vector<uint8_t> px;
        int w = 0, h = 0;

        // Faceline: skin, then wrinkles, makeup, beard stubble.
        const float *skin = kFacelineColor[p.faceline_color];
        bool any = false;
        auto layer = [&](TexKind k, int idx, Mode m, const float *c) {
            if (!Tex(r, k, idx, px, w, h)) return;
            if (!any) { Fill(out.faceline, 512, 512, skin); any = true; }
            Modulate(px, m, c);
            DrawFull(out.faceline, px, w, h);
        };
        if (p.faceline_wrinkle) layer(TexKind::Wrinkle, p.faceline_wrinkle, M_Alpha, kCommonColor[p.faceline_color]);
        if (p.faceline_make)    layer(TexKind::Makeup, p.faceline_make, M_Direct, nullptr);
        if (p.beard_type >= 4)  layer(TexKind::Beard, p.beard_type - 4, M_Alpha, kCommonColor[p.beard_color]);

        if (Tex(r, TexKind::NoseLine, p.nose_type, px, w, h)) {
            Fill(out.noseline, 256, 256, nullptr);
            Modulate(px, M_Alpha, kBlack);
            DrawFull(out.noseline, px, w, h);
        }
        if (p.glass_type && Tex(r, TexKind::Glass, p.glass_type, px, w, h)) {
            Fill(out.glasses, 512, 512, nullptr);
            Modulate(px, M_LumAlpha, kCommonColor[p.glass_color]);
            DrawFull(out.glasses, px, w, h);
        }

        MaskKit kit;
        BuildMaskKit(r, p, kit);
        DrawMask(kit, Expression{}, out.mask);
        return true;
    }

    bool BuildMaskKit(const Resources &r, const Parts &p, MaskKit &out) {
        static const float kWhite[4] = { 1, 1, 1, 1 }, kBlack[4] = { 0, 0, 0, 1 };
        out = {};
        out.p = p;
        auto get = [&](Image &img, TexKind k, int i, Mode m, const float *c0, const float *c1 = nullptr, const float *c2 = nullptr) {
            if (!Tex(r, k, i, img.px, img.w, img.h)) { img = {}; return false; }
            Modulate(img.px, m, c0, c1, c2);
            return true;
        };
        const float *ec = kCommonColor[p.eye_color], *mc = kCommonColor[p.mouth_color], *lc = kUpperLipColor[p.mouth_color];
        get(out.eye[Expression::OwnEyes], TexKind::Eye, p.eye_type, M_LayeredRgb, kBlack, kWhite, ec);
        get(out.eye[Expression::SmileEyes], TexKind::Eye, 60, M_LayeredRgb, kBlack, kWhite, ec);   // the ^ arc
        get(out.eye[Expression::WideEyes], TexKind::Eye, 61, M_LayeredRgb, kBlack, kWhite, ec);    // the round eye
        // Closed: the arc upside down, the lids lowered.
        Image &cl = out.eye[Expression::ClosedEyes];
        cl = out.eye[Expression::SmileEyes];
        for (int y = 0; y < cl.h / 2; y++)
            for (int x = 0; x < cl.w * 4; x++) std::swap(cl.px[(size_t)y * cl.w * 4 + x], cl.px[(size_t)(cl.h - 1 - y) * cl.w * 4 + x]);
        static const int kMouthTex[Expression::kMouths] = { -1, 19, 22, 36, 36, 30, 35 };
        for (int m = 0; m < Expression::kMouths; m++)
            get(out.mouth[m], TexKind::Mouth, m ? kMouthTex[m] : p.mouth_type, M_LayeredRgb, mc, lc, kWhite);
        get(out.brow, TexKind::Eyebrow, p.eyebrow_type, M_Alpha, kCommonColor[p.eyebrow_color]);
        if (p.mustache_type) get(out.mustache, TexKind::Mustache, p.mustache_type, M_Alpha, kCommonColor[p.beard_color]);
        if (p.mole_type) get(out.mole, TexKind::Mole, 1, M_Alpha, kBlack);
        return true;
    }

    void DrawMask(const MaskKit &k, const Expression &e, Image &mask, int res) {
        const Parts &p = k.p;
        Fill(mask, res, res, nullptr);
        const float bs = TexUnit((float)res);
        auto part = [&](const Image &img, float x, float y, float w, float h, float deg, Origin o) {
            if (!img.px.empty()) DrawPart(mask, img.px, img.w, img.h, x, y, w, h, deg, o);
        };
        {
            const Image &eye = k.eye[e.eyes].px.empty() ? k.eye[Expression::OwnEyes] : k.eye[e.eyes];
            const int tex = e.eyes == Expression::OwnEyes ? p.eye_type : e.eyes == Expression::WideEyes ? 61 : 60;
            const float sc = Scale2Dim(p.eye_scale), sy = 0.12f * p.eye_aspect + 0.64f;
            const float sx = kTexScaleX * p.eye_x, ey = 18.451525f + kMagicY * kTexScaleY * p.eye_y;
            const float ew = TexUnit(342.0f) * sc, eh = TexUnit(288.0f) * sc * sy;
            const float ang = Rot2Ang(p.eye_rotate + 32 - kEyeRotOffset[tex % 62]);
            part(eye, bs * (32 + sx), ey * bs, ew * bs, eh * bs, 360 - ang, O_Left);
            part(eye, bs * (32 - sx), ey * bs, ew * bs, eh * bs, ang, O_Right);
        }
        {
            const float sc = Scale2Dim(p.eyebrow_scale), sy = 0.12f * p.eyebrow_aspect + 0.64f;
            const float sx = kTexScaleX * p.eyebrow_x, ey = 16.549807f + kMagicY * kTexScaleY * (p.eyebrow_y + e.brow_y);
            const float ew = TexUnit(324.0f) * sc, eh = TexUnit(288.0f) * sc * sy;
            const float ang = Rot2Ang(p.eyebrow_rotate + 32 + e.brow_rot + kBrowRotOffset[p.eyebrow_type % 24]);
            part(k.brow, bs * (32 + sx), ey * bs, ew * bs, eh * bs, 360 - ang, O_Left);
            part(k.brow, bs * (32 - sx), ey * bs, ew * bs, eh * bs, ang, O_Right);
        }
        {
            const Image &mouth = k.mouth[e.mouth].px.empty() ? k.mouth[Expression::OwnMouth] : k.mouth[e.mouth];
            // The stand-ins are drawn a little larger than the Mii's own mouth
            // so a small mouth still shows the expression.
            static const float kGrow[Expression::kMouths] = { 1.0f, 1.35f, 1.35f, 1.1f, 0.75f, 1.35f, 1.35f };
            const float sc = std::max(Scale2Dim(p.mouth_scale), 2.2f) * kGrow[e.mouth];
            const float own = Scale2Dim(p.mouth_scale);
            const bool o = e.mouth == Expression::Open || e.mouth == Expression::Talk;
            const float sy = o ? 1.0f : 0.12f * p.mouth_aspect + 0.64f;
            const float my = 29.25885f + kMagicY * kTexScaleY * p.mouth_y;
            const float ms = e.mouth == Expression::OwnMouth ? own : sc;
            part(mouth, bs * 32, my * bs, TexUnit(396.0f) * ms * bs, TexUnit(288.0f) * ms * sy * bs, 0, O_Center);
        }
        if (!k.mustache.px.empty()) {
            const float sc = Scale2Dim(p.mustache_scale), my = 31.763554f + kMagicY * kTexScaleY * p.mustache_y;
            const float mw = TexUnit(288.0f) * sc, mh = TexUnit(576.0f) * sc;
            part(k.mustache, bs * 32, my * bs, mw * bs, mh * bs, 0, O_Left);
            part(k.mustache, bs * 32, my * bs, mw * bs, mh * bs, 0, O_Right);
        }
        if (!k.mole.px.empty()) {
            const float sc = Scale2Dim(p.mole_scale);
            const float mx = 17.766165f + 2 * kTexScaleX * p.mole_x, my = 17.95986f + kMagicY * kTexScaleY * p.mole_y;
            part(k.mole, mx * bs, my * bs, sc * bs, sc * bs, 0, O_Center);
        }
    }
}
