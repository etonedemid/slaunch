#include <sl/menu/mii/MiiScene.hpp>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace sl::menu::mii {

    namespace {
        // Atlas, 2048x1024. Every picture goes in twice as wide, its right half
        // mirrored, because the faceline's and glasses' meshes run u over 0..2
        // and sample with mirrored repeat.
        constexpr int kAW = 2048, kAH = 1024;
        struct Slot { int x, y; };
        constexpr Slot kNoseline{ 0, 0 }, kWhite{ 512, 0 }, kMask{ 0, 512 }, kFaceline{ 1024, 0 }, kGlasses{ 1024, 512 };

        void Blit(Image &atlas, const Image &img, Slot s) {
            for (int y = 0; y < img.h; y++)
                for (int x = 0; x < img.w; x++) {
                    const uint8_t *p = &img.px[((size_t)y * img.w + x) * 4];
                    memcpy(&atlas.px[((size_t)(s.y + y) * kAW + s.x + x) * 4], p, 4);
                    memcpy(&atlas.px[((size_t)(s.y + y) * kAW + s.x + 2 * img.w - 1 - x) * 4], p, 4);
                }
        }

        // Turned a little toward the viewer's left, as the dashboard stood avatars.
        constexpr float kYaw = 0.25f;
        // Frame height in Mii units: the tallest Mii (127, 127) fits.
        constexpr float kFrameUnits = 200.0f;
        // The head's parts, painted in this order (no depth buffer): each
        // lies over the ones before it wherever they overlap from the front.
        int Layer(ShapeKind k) {
            switch (k) {
            case ShapeKind::Faceline: return 0;
            case ShapeKind::Mask:     return 1;
            case ShapeKind::Beard:    return 2;
            case ShapeKind::Nose:     return 3;
            case ShapeKind::NoseLine: return 4;
            case ShapeKind::Hair:     return 5;
            case ShapeKind::Forehead: return 6;   // the skin under the fringe, over the hair behind it
            case ShapeKind::Hat:      return 7;
            case ShapeKind::Glasses:  return 8;
            }
            return 0;
        }
        // Seconds to blend from one animation into the next.
        constexpr float kBlend = 0.35f;

        float Lit(float nx, float ny, float nz) {
            const float l = std::sqrt(nx * nx + ny * ny + nz * nz) + 1e-9f;
            return 0.45f + 0.6f * std::fmax(0.0f, (nx * -0.3f + ny * 0.45f + nz * 0.85f) / l);
        }
        // Which way a mesh's triangles wind against its normals: +1, -1, or 0
        // when it has no normals to tell by.
        float Winding(const std::vector<float> &pos, const std::vector<float> &nrm, const std::vector<uint16_t> &idx) {
            double s = 0;
            for (size_t i = 0; i + 2 < idx.size(); i += 3) {
                const float *a = &pos[idx[i] * 3], *b = &pos[idx[i + 1] * 3], *c = &pos[idx[i + 2] * 3];
                const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                const float g[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                for (int k = 0; k < 3; k++) {
                    const float *n = &nrm[idx[i + k] * 3];
                    s += g[0] * n[0] + g[1] * n[1] + g[2] * n[2];
                }
            }
            return s > 0 ? 1.0f : s < 0 ? -1.0f : 0.0f;
        }
        Quat Nlerp(Quat a, Quat b, float t) {
            if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = { -b.x, -b.y, -b.z, -b.w };
            Quat r{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
            const float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
            return n > 0 ? Quat{ r.x / n, r.y / n, r.z / n, r.w / n } : Quat{ 0, 0, 0, 1 };
        }
    }

    bool Scene::Load(const char *shape, const char *tex, const char *body, const char *anims, const Parts &p) {
        m_parts = p;
        if (!LoadBody(body, p, m_body)) return false;
        Resources r;
        if (!r.Load(shape, tex)) return false;
        FaceTextures ft;
        BuildFaceTextures(r, p, ft);
        BuildMaskKit(r, p, m_kit);
        m_expr = Expression{};

        m_atlas.w = kAW; m_atlas.h = kAH;
        m_atlas.px.assign((size_t)kAW * kAH * 4, 0);
        Blit(m_atlas, ft.noseline, kNoseline);
        Blit(m_atlas, ft.mask, kMask);
        Blit(m_atlas, ft.faceline, kFaceline);
        Blit(m_atlas, ft.glasses, kGlasses);
        for (int y = 0; y < 8; y++) memset(&m_atlas.px[((size_t)(kWhite.y + y) * kAW + kWhite.x) * 4], 255, 8 * 4);
        m_white[0] = (kWhite.x + 4.0f) / kAW;
        m_white[1] = (kWhite.y + 4.0f) / kAH;

        for (Mesh &M : r.BuildHead(p)) {
            const Image *img = nullptr;
            Slot s{};
            switch (M.kind) {
            case ShapeKind::Mask:     img = &ft.mask;     s = kMask;     break;
            case ShapeKind::Faceline: img = &ft.faceline; s = kFaceline; break;
            case ShapeKind::NoseLine: img = &ft.noseline; s = kNoseline; break;
            case ShapeKind::Glasses:  img = &ft.glasses;  s = kGlasses;  break;
            default: break;
            }
            const bool textured = img && img->w && !M.uv.empty();
            Part pt;
            pt.layer = Layer(M.kind);
            const bool decal = M.kind == ShapeKind::Mask || M.kind == ShapeKind::NoseLine || M.kind == ShapeKind::Glasses;
            if (decal && !textured) continue;   // nothing to show: no glasses, no nose line
            pt.pos = std::move(M.pos);
            pt.nrm = std::move(M.nrm);
            pt.idx = std::move(M.idx);
            memcpy(pt.color, M.color, sizeof(pt.color));
            const size_t n = pt.pos.size() / 3;
            if (pt.nrm.size() != n * 3) pt.nrm.assign(n * 3, 0.0f);
            pt.uv.resize(n * 2);
            for (size_t v = 0; v < n; v++) {
                if (textured) {
                    const float u = std::fmin(2.0f, std::fmax(0.0f, M.uv[v * 2])), vv = std::fmin(1.0f, std::fmax(0.0f, M.uv[v * 2 + 1]));
                    pt.uv[v * 2] = (s.x + u * img->w) / kAW;
                    pt.uv[v * 2 + 1] = (s.y + vv * img->h) / kAH;
                } else {
                    pt.uv[v * 2] = m_white[0];
                    pt.uv[v * 2 + 1] = m_white[1];
                }
            }
            pt.winding = Winding(pt.pos, pt.nrm, pt.idx);
            pt.twoSided = M.kind == ShapeKind::Hair || M.kind == ShapeKind::Hat;
            if (textured) pt.color[0] = pt.color[1] = pt.color[2] = pt.color[3] = 1;   // the texture has the colour
            m_head.push_back(std::move(pt));
        }

        m_bodyWinding = Winding(m_body.pos, m_body.nrm, m_body.idx);
        m_haveAnims = anims && m_anims.Load(anims);
        if (m_haveAnims) {
            const char *mine = p.gender ? "female_idle_" : "male_idle_";
            const std::string sex = p.gender ? "female_" : "male_";
            for (int i = 0; i < m_anims.Count(); i++) {
                const std::string &nm = m_anims.Name(i);
                if (!nm.compare(0, 14, "generic_stand_") || !nm.compare(0, strlen(mine), mine)) m_idles.push_back(i);
            }
            // Now and then something livelier, as the 360's avatars did.
            for (const std::string &nm : { std::string("generic_wave"), std::string("generic_clap"), sex + "laugh",
                                           sex + "confused", sex + "shocked_surprised", sex + "yawn" })
                if (const int i = m_anims.Find(nm.c_str()); i >= 0) m_extras.push_back(i);
            if (m_idles.empty()) m_haveAnims = false;
        }
        // Each Mii its own sequence: from its looks and where it lives, so two
        // copies of one Mii do not move in step either.
        m_rng = 2166136261u;
        for (size_t i = 0; i < sizeof(p); i++) m_rng = (m_rng ^ ((const uint8_t *)&p)[i]) * 16777619u;
        m_rng ^= (uint32_t)(uintptr_t)this;
        if (!m_rng) m_rng = 0x2545F491u;
        for (int j = 0; j < kAnimJoints; j++) m_last[j] = m_from[j] = { 0, 0, 0, 1 };
        return !m_head.empty();
    }

    void Scene::PickAnim(double t) {
        m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
        // Mostly standing about, now and then one of the gender's idles.
        std::vector<int> stands, others;
        for (int i : m_idles) (m_anims.Name(i).compare(0, 14, "generic_stand_") ? others : stands).push_back(i);
        const unsigned roll = m_rng % 100;
        const std::vector<int> &pool = !m_extras.empty() && roll < 15 ? m_extras
                                     : (!others.empty() && roll < 45) || stands.empty() ? others : stands;
        int next = pool[(m_rng >> 8) % pool.size()];
        if (next == m_anim && pool.size() > 1) next = pool[((m_rng >> 8) + 1) % pool.size()];
        if (m_anim >= 0) {
            memcpy(m_from, m_last, sizeof(m_from));
            memcpy(m_fromRoot, m_lastRoot, sizeof(m_fromRoot));
            m_blend = true;
        }
        m_anim = next;
        m_start = t;
    }

    const Image *Scene::MaskUpdate(int res) {
        if (!m_maskDirty) return nullptr;
        m_maskDirty = false;
        // Drawn at 256 at most and scaled up: the face is never that big on
        // screen, and a 512 redraw costs a frame on the console.
        const int dr = std::min(res, 256), k = res / dr;
        DrawMask(m_kit, m_expr, m_maskImg, dr);
        if (m_maskImg.w != dr) return nullptr;
        m_maskDouble.w = 2 * res; m_maskDouble.h = res;
        m_maskDouble.px.resize((size_t)4 * res * 2 * res);
        for (int y = 0; y < res; y++)
            for (int x = 0; x < res; x++) {
                const uint8_t *p = &m_maskImg.px[((size_t)(y / k) * dr + x / k) * 4];
                memcpy(&m_maskDouble.px[((size_t)y * 2 * res + x) * 4], p, 4);
                memcpy(&m_maskDouble.px[((size_t)y * 2 * res + 2 * res - 1 - x) * 4], p, 4);
            }
        return &m_maskDouble;
    }

    void Scene::Build(double t, float x, float y, float size, float alpha, std::vector<Vertex> &out) {
        Quat rot[kAnimJoints];
        float root[3] = { 0, 0, 0 };
        if (m_haveAnims) {
            if (m_anim < 0 || t - m_start >= m_anims.Duration(m_anim)) PickAnim(t);
            const float at = (float)(t - m_start);
            m_anims.Sample(m_anim, at, rot, root);
            if (m_blend && at < kBlend) {
                const float k = at / kBlend, s = k * k * (3 - 2 * k);
                for (int j = 0; j < kAnimJoints; j++) rot[j] = Nlerp(m_from[j], rot[j], s);
                for (int c = 0; c < 3; c++) root[c] = m_fromRoot[c] + (root[c] - m_fromRoot[c]) * s;
            } else {
                m_blend = false;
            }
            // The face: the Xbox codes mapped onto the Mii's own face parts.
            Expression e;
            int mouth = 0, brow = 0, eye = 0;
            if (m_anims.Face(m_anim, at, mouth, brow, eye)) {
                switch (eye) {
                case 4: case 6: e.eyes = Expression::SmileEyes; break;
                case 5: e.eyes = Expression::WideEyes; break;
                case 7: case 8: case 13: e.eyes = Expression::ClosedEyes; break;
                default: break;
                }
                switch (mouth) {
                case 1: case 2: e.mouth = Expression::Frown; break;
                case 3: e.mouth = Expression::Wavy; break;
                case 4: e.mouth = Expression::Grin; break;
                case 5: case 7: e.mouth = Expression::Open; break;
                case 6: e.mouth = Expression::Smile; break;
                default: if (mouth >= 8 && mouth <= 13) e.mouth = Expression::Talk; break;
                }
                switch (brow) {
                case 1: e.brow_rot = -2; e.brow_y = -1; break;   // sad: inner ends up
                case 2: e.brow_rot = 2; e.brow_y = 1; break;     // angry: inner ends down
                case 3: e.brow_rot = -1; break;
                case 4: e.brow_y = -2; break;
                default: break;
                }
            }
            // A blink every few seconds while the eyes are the Mii's own.
            if (t >= m_blinkAt + 0.14) {
                m_rng = m_rng * 1664525u + 1013904223u;
                m_blinkAt = t + 2.5 + (m_rng >> 8) % 3000 / 1000.0;
            }
            if (e.eyes == Expression::OwnEyes && t >= m_blinkAt) e.eyes = Expression::ClosedEyes;
            if (e != m_expr) { m_expr = e; m_maskDirty = true; }
            memcpy(m_last, rot, sizeof(m_last));
            memcpy(m_lastRoot, root, sizeof(m_lastRoot));
            if (m_float > 0) {
                const int st = m_anims.Find("generic_stand_0");
                Quat hover[kAnimJoints];
                float hroot[3] = { 0, 0, 0 };
                if (st >= 0) m_anims.Sample(st, 0, hover, hroot);
                else for (Quat &q : hover) q = { 0, 0, 0, 1 };
                // The arms (clavicle, shoulder, elbow, hand on each side) part
                // way back to the T pose, rising and falling a little.
                const float arms = 0.42f + 0.06f * (float)std::sin(t * 1.8);
                for (int j = 5; j <= 12; j++) hover[j] = Nlerp(hover[j], Quat{ 0, 0, 0, 1 }, arms);
                for (int j = 0; j < kAnimJoints; j++) rot[j] = Nlerp(rot[j], hover[j], m_float);
                for (int c = 0; c < 3; c++) root[c] += (hroot[c] - root[c]) * m_float;
            }
        }
        Pose pose;
        ComputePose(m_body, m_parts, m_haveAnims ? rot : nullptr, m_haveAnims ? root : nullptr, pose);
        Skin(m_body, pose, m_bp, m_bn);

        const float cy = std::cos(kYaw), sy = std::sin(kYaw), sc = size / kFrameUnits;
        const float ox = x + size * 0.5f, foot = y + size * 0.975f;
        auto put = [&](Vertex &o, const float *p, const float *n, const float *col, float u, float v) {
            const float px = p[0] * cy + p[2] * sy;
            o.x = ox + px * sc;
            o.y = foot - p[1] * sc;
            const float l = Lit(n[0] * cy + n[2] * sy, n[1], -n[0] * sy + n[2] * cy);
            o.r = std::fmin(1.0f, col[0] * l); o.g = std::fmin(1.0f, col[1] * l); o.b = std::fmin(1.0f, col[2] * l);
            o.a = col[3] * alpha;
            o.u = u; o.v = v;
        };
        auto depth = [&](const float *p) { return -p[0] * sy + p[2] * cy; };
        // Facing away from the viewer, by the posed triangle's own face and
        // its mesh's winding (corner normals mislead on big triangles).
        auto away = [&](const float *a, const float *b, const float *c, float winding) {
            const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            const float gx = e1[1] * e2[2] - e1[2] * e2[1], gz = e1[0] * e2[1] - e1[1] * e2[0];
            return winding * (-gx * sy + gz * cy) < 0;
        };
        m_tris.clear();
        for (size_t i = 0; i + 2 < m_body.idx.size(); i += 3) {
            const uint16_t *ix = &m_body.idx[i];
            if (away(&m_bp[ix[0] * 3], &m_bp[ix[1] * 3], &m_bp[ix[2] * 3], m_bodyWinding)) continue;
            Tri tr;
            tr.layer = -1;
            tr.depth = 0;
            for (int k = 0; k < 3; k++) {
                const size_t v = m_body.idx[i + k];
                put(tr.v[k], &m_bp[v * 3], &m_bn[v * 3], &m_body.col[v * 4], m_white[0], m_white[1]);
                tr.depth += depth(&m_bp[v * 3]);
            }
            m_tris.push_back(tr);
        }
        const size_t nBody = m_tris.size();
        float headFront = -1e30f;
        for (const Part &pt : m_head) {
            for (size_t i = 0; i + 2 < pt.idx.size(); i += 3) {
                Tri tr;
                tr.layer = pt.layer;
                tr.depth = 0;
                float p[3][3];
                for (int k = 0; k < 3; k++) {
                    const size_t v = pt.idx[i + k];
                    float n[3];
                    MatPoint(pose.head, &pt.pos[v * 3], p[k]);
                    MatVector(pose.head, &pt.nrm[v * 3], n);
                    put(tr.v[k], p[k], n, pt.color, pt.uv[v * 2], pt.uv[v * 2 + 1]);
                    tr.z[k] = depth(p[k]);
                    tr.depth += tr.z[k];
                }
                if (away(p[0], p[1], p[2], pt.winding)) {
                    // Hair's inside (the lining under bangs and behind the
                    // face) is drawn too, but under everything on the head:
                    // it lies inside the shell, behind whatever covers it.
                    if (!pt.twoSided) continue;
                    tr.layer = -1;
                }
                headFront = std::fmax(headFront, tr.depth);
                m_tris.push_back(tr);
            }
        }
        // Hair against the face: which side of it a hair triangle lies on is
        // read from a coarse depth map of the faceline and forehead, drawn on
        // the CPU here each frame.
        {
            constexpr int G = 64;
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (size_t i = nBody; i < m_tris.size(); i++)
                for (const Vertex &v : m_tris[i].v) { x0 = std::fmin(x0, v.x); y0 = std::fmin(y0, v.y); x1 = std::fmax(x1, v.x); y1 = std::fmax(y1, v.y); }
            const float gw = (x1 - x0) / G + 1e-3f, gh = (y1 - y0) / G + 1e-3f;
            m_faceZ.assign(G * G, -1e30f);
            for (size_t i = nBody; i < m_tris.size(); i++) {
                const Tri &t = m_tris[i];
                if (t.layer != 0 && t.layer != 6) continue;
                float X[3], Y[3];
                for (int k = 0; k < 3; k++) { X[k] = (t.v[k].x - x0) / gw; Y[k] = (t.v[k].y - y0) / gh; }
                const float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
                if (std::fabs(area) < 1e-9f) continue;
                const int cx0 = std::max(0, (int)std::fmin(X[0], std::fmin(X[1], X[2]))), cx1 = std::min(G - 1, (int)std::fmax(X[0], std::fmax(X[1], X[2])));
                const int cy0 = std::max(0, (int)std::fmin(Y[0], std::fmin(Y[1], Y[2]))), cy1 = std::min(G - 1, (int)std::fmax(Y[0], std::fmax(Y[1], Y[2])));
                for (int gy = cy0; gy <= cy1; gy++)
                    for (int gx = cx0; gx <= cx1; gx++) {
                        const float fx = gx + 0.5f, fy = gy + 0.5f;
                        const float w0 = ((X[1] - fx) * (Y[2] - fy) - (X[2] - fx) * (Y[1] - fy)) / area;
                        const float w1 = ((X[2] - fx) * (Y[0] - fy) - (X[0] - fx) * (Y[2] - fy)) / area, w2 = 1 - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        float &z = m_faceZ[gy * G + gx];
                        z = std::fmax(z, w0 * t.z[0] + w1 * t.z[1] + w2 * t.z[2]);
                    }
            }
            // The face's own parts (mask, beard, nose, glasses) are painted
            // over the faceline, which is only right from the front: turned
            // away, or where the nose's root sinks into the head, they lie
            // behind it. The nose and glasses are split finely and each piece
            // tested; the flat decals only go under as a whole.
            auto behind = [&](float x, float y, float z, float margin) {
                const int gx = (int)((x - x0) / gw), gy = (int)((y - y0) / gh);
                return gx >= 0 && gy >= 0 && gx < G && gy < G && z < m_faceZ[gy * G + gx] - margin;
            };
            auto lerp = [](const Vertex &a, const Vertex &b) {
                return Vertex{ (a.x + b.x) / 2, (a.y + b.y) / 2, (a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2,
                               (a.a + b.a) / 2, (a.u + b.u) / 2, (a.v + b.v) / 2 };
            };
            std::vector<Tri> pieces;
            auto cut = [&](auto &&self, const Tri &t, int level) -> void {
                int n = 0;
                for (int k = 0; k < 3; k++) n += behind(t.v[k].x, t.v[k].y, t.z[k], 0.5f);
                const float cz = (t.z[0] + t.z[1] + t.z[2]) / 3;
                const bool cb = behind((t.v[0].x + t.v[1].x + t.v[2].x) / 3, (t.v[0].y + t.v[1].y + t.v[2].y) / 3, cz, 0.5f);
                if (n == 0 && !cb) { pieces.push_back(t); return; }
                if ((n == 3 && cb) || level == 3) { if (!cb) pieces.push_back(t); return; }
                const Vertex m01 = lerp(t.v[0], t.v[1]), m12 = lerp(t.v[1], t.v[2]), m20 = lerp(t.v[2], t.v[0]);
                const float z01 = (t.z[0] + t.z[1]) / 2, z12 = (t.z[1] + t.z[2]) / 2, z20 = (t.z[2] + t.z[0]) / 2;
                const Tri kids[4] = {
                    { t.layer, t.depth, { t.z[0], z01, z20 }, { t.v[0], m01, m20 } },
                    { t.layer, t.depth, { z01, t.z[1], z12 }, { m01, t.v[1], m12 } },
                    { t.layer, t.depth, { z20, z12, t.z[2] }, { m20, m12, t.v[2] } },
                    { t.layer, t.depth, { z01, z12, z20 }, { m01, m12, m20 } },
                };
                for (const Tri &k : kids) self(self, k, level + 1);
            };
            for (size_t i = nBody; i < m_tris.size(); i++) {
                Tri &t = m_tris[i];
                if (t.layer == 3 || t.layer == 8) {   // nose, glasses
                    cut(cut, t, 0);
                    t.layer = -2;                     // replaced by its pieces
                } else if (t.layer == 1 || t.layer == 2 || t.layer == 4) {
                    int n = 0;
                    for (int k = 0; k < 3; k++) n += behind(t.v[k].x, t.v[k].y, t.z[k], 3.0f);
                    if (n >= 2) t.layer = -1;
                }
            }
            m_tris.erase(std::remove_if(m_tris.begin() + nBody, m_tris.end(), [](const Tri &t) { return t.layer == -2; }), m_tris.end());
            m_tris.insert(m_tris.end(), pieces.begin(), pieces.end());
            // All of the hair goes under the face (the forehead's edge is the
            // hairline, as with a depth buffer); what lies (mostly) in front of
            // the face (a fringe, the sides) is drawn again on top.
            // Where there is no face behind it (long hair below the chin), in
            // front means ahead of the neck: locks over the chest, not hair
            // hanging down the back.
            const float origin[3] = { pose.head.m[12], pose.head.m[13], pose.head.m[14] }, neck = depth(origin);
            auto ahead = [&](float x, float y, float z) {
                const int gx = (int)((x - x0) / gw), gy = (int)((y - y0) / gh);
                const float face = gx < 0 || gy < 0 || gx >= G || gy >= G ? -1e30f : m_faceZ[gy * G + gx];
                return face < -1e29f ? z > neck : z > face + 0.5f;
            };
            // A triangle with corners on both sides of the face is split into
            // four, up to three times, so the hair's edge against the face
            // follows the face rather than the hair's big triangles.
            auto mid = [](const Vertex &a, const Vertex &b) {
                return Vertex{ (a.x + b.x) / 2, (a.y + b.y) / 2, (a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2,
                               (a.a + b.a) / 2, (a.u + b.u) / 2, (a.v + b.v) / 2 };
            };
            std::vector<Tri> over;
            auto place = [&](auto &&self, const Tri &t, int level) -> void {
                const int in = ahead(t.v[0].x, t.v[0].y, t.z[0]) + ahead(t.v[1].x, t.v[1].y, t.z[1]) + ahead(t.v[2].x, t.v[2].y, t.z[2]);
                if (in == 0 || in == 3 || level == 3) {
                    if (in >= 2) { over.push_back(t); over.back().layer = 7; }   // over the forehead too
                    return;
                }
                const Vertex m01 = mid(t.v[0], t.v[1]), m12 = mid(t.v[1], t.v[2]), m20 = mid(t.v[2], t.v[0]);
                const float z01 = (t.z[0] + t.z[1]) / 2, z12 = (t.z[1] + t.z[2]) / 2, z20 = (t.z[2] + t.z[0]) / 2;
                const Tri kids[4] = {
                    { t.layer, 0, { t.z[0], z01, z20 }, { t.v[0], m01, m20 } },
                    { t.layer, 0, { z01, t.z[1], z12 }, { m01, t.v[1], m12 } },
                    { t.layer, 0, { z20, z12, t.z[2] }, { m20, m12, t.v[2] } },
                    { t.layer, 0, { z01, z12, z20 }, { m01, m12, m20 } },
                };
                for (const Tri &k : kids) self(self, k, level + 1);
            };
            for (size_t i = nBody; i < m_tris.size(); i++) {
                if (m_tris[i].layer != 5 && m_tris[i].layer != 7) continue;
                place(place, m_tris[i], 0);
                m_tris[i].layer = -1;
            }
            m_tris.insert(m_tris.end(), over.begin(), over.end());
        }
        // ponytail: painter's order, no depth buffer; a deko3d pass with one if parts ever cross wrongly
        auto byDepth = [](const Tri &a, const Tri &b) { return a.depth < b.depth; };
        std::sort(m_tris.begin(), m_tris.begin() + nBody, byDepth);
        std::stable_sort(m_tris.begin() + nBody, m_tris.end(),
                         [](const Tri &a, const Tri &b) { return a.layer != b.layer ? a.layer < b.layer : a.depth < b.depth; });
        // The hair under the face (all of it, behind the body too: long hair
        // down the back), the body behind the face, the rest of the head,
        // then any of the body ahead of the face (a hand raised in front).
        const size_t split = std::partition_point(m_tris.begin(), m_tris.begin() + nBody,
                                                  [&](const Tri &t) { return t.depth <= headFront; }) - m_tris.begin();
        out.resize(m_tris.size() * 3);
        size_t o = 0;
        auto emit = [&](size_t a, size_t b) { for (size_t i = a; i < b; i++, o += 3) memcpy(&out[o], m_tris[i].v, sizeof(m_tris[i].v)); };
        const size_t under = std::partition_point(m_tris.begin() + nBody, m_tris.end(),
                                                  [](const Tri &t) { return t.layer < 0; }) - m_tris.begin();
        emit(nBody, under);
        emit(0, split);
        emit(under, m_tris.size());
        emit(split, nBody);
    }
}
