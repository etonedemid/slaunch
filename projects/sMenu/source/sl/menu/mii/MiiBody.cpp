#include <sl/menu/mii/MiiBody.hpp>
#include "MiiColors.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sl::menu::mii {

    // ---- math ---------------------------------------------------------------
    Mat4 MatIdentity() { Mat4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
    Mat4 MatMul(const Mat4 &a, const Mat4 &b) {
        Mat4 r{};
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
    void MatPoint(const Mat4 &m, const float i[3], float o[3]) {
        for (int r = 0; r < 3; r++) o[r] = m.m[r] * i[0] + m.m[4 + r] * i[1] + m.m[8 + r] * i[2] + m.m[12 + r];
    }
    void MatVector(const Mat4 &m, const float i[3], float o[3]) {
        for (int r = 0; r < 3; r++) o[r] = m.m[r] * i[0] + m.m[4 + r] * i[1] + m.m[8 + r] * i[2];
    }
    namespace {
        Mat4 Scale(float x, float y, float z) { Mat4 r = MatIdentity(); r.m[0] = x; r.m[5] = y; r.m[10] = z; return r; }
        Mat4 Rotate(const Quat &q) {
            const float x = q.x, y = q.y, z = q.z, w = q.w;
            Mat4 r = MatIdentity();
            r.m[0] = 1 - 2 * (y * y + z * z); r.m[1] = 2 * (x * y + z * w);     r.m[2] = 2 * (x * z - y * w);
            r.m[4] = 2 * (x * y - z * w);     r.m[5] = 1 - 2 * (x * x + z * z); r.m[6] = 2 * (y * z + x * w);
            r.m[8] = 2 * (x * z + y * w);     r.m[9] = 2 * (y * z - x * w);     r.m[10] = 1 - 2 * (x * x + y * y);
            return r;
        }
        Quat Nlerp(Quat a, Quat b, float t) {
            if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = { -b.x, -b.y, -b.z, -b.w };
            Quat r{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
            const float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
            if (n > 0) { r.x /= n; r.y /= n; r.z /= n; r.w /= n; } else r = { 0, 0, 0, 1 };
            return r;
        }
    }

    // ---- body file ------------------------------------------------------------
    // MiiBody.dat, as read off firmware 23.0.1: two bodies back to back (male,
    // female). Each, little endian:
    //   0x000 "NFVB" u32 1 u32 size_of_this_body u32 22
    //   0x010 u32 palette[25]     vertex bone byte -> bone (22 used)
    //   0x074 u32 parent[25]      0xffff = none
    //   0x0d8 u32 (offset, size) x 8: shirt pos, nrm, bone, idx, trousers pos, nrm, bone, idx
    //         then (0x120, 0x640) and two more words
    //   0x120 bones[25] x 64 B: row-major 3x4 local matrix, then f32 scale[4]
    //   pos half4 (xyz, 1) in the bone's space, nrm 10:10:10:2 snorm, bone u8, idx u16 list
    namespace {
        float Half(const uint8_t *p) { uint16_t h; memcpy(&h, p, 2); return HalfToFloat(h); }
        uint32_t U32(const std::vector<uint8_t> &d, size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; }
        float Snorm10(uint32_t v) { v &= 1023; return (v >= 512 ? (float)v - 1024 : (float)v) / 511.0f; }
    }

    bool LoadBody(const char *path, const Parts &p, BodyMesh &out) {
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        std::vector<uint8_t> d;
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        d.resize(sz > 0 ? (size_t)sz : 0);
        const bool rd = sz > 0 && fread(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        if (!rd) return false;
        size_t base = 0;
        for (int g = 0;; g++) {
            if (base + 0x760 > d.size() || memcmp(&d[base], "NFVB", 4) || U32(d, base + 4) != 1) return false;
            if (g == (p.gender & 1)) break;
            base += U32(d, base + 8);
        }
        const size_t end = base + U32(d, base + 8);
        if (end > d.size()) return false;
        out = BodyMesh{};
        uint32_t pal[kBodyBones];
        for (int b = 0; b < kBodyBones; b++) {
            pal[b] = U32(d, base + 0x10 + 4 * b);
            const uint32_t par = U32(d, base + 0x74 + 4 * b);
            out.parent[b] = par == 0xffff ? -1 : (int)par;
            if (out.parent[b] >= b) return false;   // parents come first
            float m[12];
            memcpy(m, &d[base + 0x120 + 64 * b], sizeof(m));
            Mat4 &M = out.bind[b];
            M = MatIdentity();
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 4; c++) M.m[c * 4 + r] = m[r * 4 + c];
        }
        static const float kPants[4] = { 0.25f, 0.27f, 0.31f, 1 };   // the console's default grey
        for (int k = 0; k < 2; k++) {
            uint32_t s[8];
            for (int i = 0; i < 8; i++) s[i] = U32(d, base + 0xd8 + 4 * (k * 8 + i));
            const uint32_t n = s[1] / 8;
            for (int i = 0; i < 8; i += 2) if (base + s[i] + s[i + 1] > end) return false;
            if (s[3] < n * 4 || s[5] < n) return false;
            const float *col = k ? kPants : kFavoriteColor[p.favorite_color % 12];
            const uint16_t first = (uint16_t)(out.pos.size() / 3);
            for (uint32_t v = 0; v < n; v++) {
                const uint8_t *pp = &d[base + s[0] + 8 * v];
                out.pos.insert(out.pos.end(), { Half(pp), Half(pp + 2), Half(pp + 4) });
                const uint32_t nn = U32(d, base + s[2] + 4 * v);
                out.nrm.insert(out.nrm.end(), { Snorm10(nn), Snorm10(nn >> 10), Snorm10(nn >> 20) });
                out.col.insert(out.col.end(), { col[0], col[1], col[2], 1 });
                const uint8_t j = d[base + s[4] + v];
                if (j >= kBodyBones || pal[j] >= kBodyBones) return false;
                out.bone.push_back((uint8_t)pal[j]);
            }
            for (uint32_t i = 0; i < s[7] / 2; i++) {
                uint16_t ix;
                memcpy(&ix, &d[base + s[6] + 2 * i], 2);
                if (ix >= n) return false;
                out.idx.push_back((uint16_t)(first + ix));
            }
        }
        return !out.idx.empty();
    }

    // ---- skeleton -------------------------------------------------------------
    namespace {
        // nn::mii VariableIconBodyBoneKind, the bone order of MiiBody.dat.
        enum B { AllRoot, Body, SklRoot, Chest, ArmL1, ArmL2, WristL, ElbowL, ShoulderL, ArmR1, ArmR2, WristR, ElbowR,
                 ShoulderR, Head, Chest2, Hip, FootL1, FootL2, AnkleL, KneeL, FootR1, FootR2, AnkleR, KneeR };
        // How each bone takes the body scale (UpdateScale, as FFL-Testing documents it).
        enum Kind { K_None, K_XYZ, K_YXZ, K_XXX, K_Head };
        const Kind kKind[kBodyBones] = {
            K_None, K_None, K_None, K_XYZ,                 // all root, body, skl root, chest
            K_YXZ, K_YXZ, K_XXX, K_YXZ, K_XXX,             // arm L1, L2, wrist, elbow, shoulder
            K_YXZ, K_YXZ, K_XXX, K_YXZ, K_XXX,             // right
            K_Head, K_XYZ, K_XYZ,                          // head (the neck), chest2, hip
            K_XYZ, K_XYZ, K_XXX, K_XXX,                    // foot L1, L2, ankle, knee
            K_XYZ, K_XYZ, K_XXX, K_XXX,                    // right
        };
        void KindScale(Kind k, float sx, float sy, float o[3]) {
            switch (k) {
            case K_None: o[0] = o[1] = o[2] = 1; break;
            case K_XYZ:  o[0] = sx; o[1] = sy; o[2] = sx; break;
            case K_YXZ:  o[0] = sy; o[1] = sx; o[2] = sx; break;   // arms run along x
            case K_XXX:  o[0] = o[1] = o[2] = sx; break;
            case K_Head: o[0] = sx; o[1] = sy < 1 ? 1 : sy; o[2] = sx; break;
            }
        }
        // The Xbox joints (avatar-anims.py order) and their parents in that list.
        enum X { XPelvis, XSpine, XChest, XNeck, XHead, XClavL, XShoulderL, XElbowL, XHandL, XClavR, XShoulderR, XElbowR,
                 XHandR, XHipL, XKneeL, XAnkleL, XToeL, XHipR, XKneeR, XAnkleR, XToeR };
        const int kXParent[kAnimJoints] = { -1, XPelvis, XSpine, XChest, XNeck, XChest, XClavL, XShoulderL, XElbowL,
                                            XChest, XClavR, XShoulderR, XElbowR, XPelvis, XHipL, XKneeL, XAnkleL,
                                            XPelvis, XHipR, XKneeR, XAnkleR };
        // Which Xbox joint drives each Nintendo bone (-1: follows its parent).
        // Both rigs face +z with the Mii's left on +x.
        const int kDrive[kBodyBones] = {
            -1, -1, XPelvis, XChest,
            XShoulderL, XElbowL, XHandL, -1, -1,
            XShoulderR, XElbowR, XHandR, -1, -1,
            XHead, -1, -1,
            XHipL, XKneeL, XAnkleL, -1,
            XHipR, XKneeR, XAnkleR, -1,
        };
        // Xbox pelvis bind height 0.777 (avatar export) against Nintendo's 47.36.
        constexpr float kXboxToMii = 47.36f / 0.777f;
        // nn::mii's cBodyScaleFactor: lifts the root as the shoes grow by sx.
        constexpr float kBodyScaleFactor = 7.0f;

        Mat4 RotOnly(Mat4 m) { m.m[12] = m.m[13] = m.m[14] = 0; return m; }
        Mat4 Transpose3(const Mat4 &m) {
            Mat4 r = MatIdentity();
            for (int a = 0; a < 3; a++) for (int c = 0; c < 3; c++) r.m[c * 4 + a] = m.m[a * 4 + c];
            return r;
        }
        // Shortest rotation taking unit vector a onto unit vector b.
        Mat4 Swing(const float a[3], const float b[3]) {
            const float c[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
            const float w = 1 + a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            Quat q{ c[0], c[1], c[2], w };
            const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (n < 1e-6f) return MatIdentity();
            q = { q.x / n, q.y / n, q.z / n, q.w / n };
            return Rotate(q);
        }
    }

    void ComputePose(const BodyMesh &bd, const Parts &p, const Quat *rot, const float *root, Pose &out) {
        // Nintendo's GetBodyScale.
        const float h = p.height, bu = p.build;
        const float sx = (bu * (h * 0.003671875f + 0.4f)) / 128.0f + h * 0.001796875f + 0.4f;
        const float sy = h * 0.006015625f + 0.5f;

        // Local rotations: the console's bind, or the animation retargeted.
        // The Xbox bind is a T pose with every rotation identity, so a joint's
        // animated world rotation applies to the Nintendo bone as it sits in a
        // T pose: its bind, with the arms swung out level first.
        Mat4 local[kBodyBones];
        for (int b = 0; b < kBodyBones; b++) local[b] = RotOnly(bd.bind[b]);
        if (rot) {
            Mat4 xw[kAnimJoints];
            for (int j = 0; j < kAnimJoints; j++) xw[j] = kXParent[j] < 0 ? Rotate(rot[j]) : MatMul(xw[kXParent[j]], Rotate(rot[j]));
            Mat4 tpose[kBodyBones], world[kBodyBones];
            for (int b = 0; b < kBodyBones; b++) {
                const int pa = bd.parent[b];
                tpose[b] = pa < 0 ? local[b] : MatMul(tpose[pa], local[b]);
                if (b == ArmL1 || b == ArmR1) {
                    const float sg = b == ArmL1 ? 1.0f : -1.0f, along[3] = { sg, 0, 0 };
                    float dir[3];
                    MatVector(tpose[b], along, dir);
                    tpose[b] = MatMul(Swing(dir, along), tpose[b]);
                }
            }
            for (int b = 0; b < kBodyBones; b++) {
                const int pa = bd.parent[b];
                world[b] = kDrive[b] >= 0 ? MatMul(xw[kDrive[b]], tpose[b]) : pa < 0 ? local[b] : MatMul(world[pa], local[b]);
                if (pa >= 0) local[b] = MatMul(Transpose3(world[pa]), world[b]);
            }
        }

        // VariableIconBodyImpl::CalculateWorldMatrix: translations scaled by
        // the parent's kind (the root's by YYX, lifted for the shoes), world
        // matrices unscaled, each bone's own kind applied last.
        Mat4 world[kBodyBones];
        for (int b = 0; b < kBodyBones; b++) {
            const int pa = bd.parent[b];
            float t[3] = { bd.bind[b].m[12], bd.bind[b].m[13], bd.bind[b].m[14] };
            if (b == SklRoot) {
                const float r[3] = { root ? root[0] * kXboxToMii : 0, root ? root[1] * kXboxToMii : 0, root ? root[2] * kXboxToMii : 0 };
                t[0] = (t[0] + r[0]) * sy;
                t[1] = (t[1] + r[1]) * sy + (sx - sy) * kBodyScaleFactor;
                t[2] = (t[2] + r[2]) * sx;
            }
            float ps[3] = { 1, 1, 1 };
            if (pa >= 0) KindScale(kKind[pa], sx, sy, ps);
            Mat4 L = local[b];
            L.m[12] = t[0] * ps[0]; L.m[13] = t[1] * ps[1]; L.m[14] = t[2] * ps[2];
            world[b] = pa < 0 ? L : MatMul(world[pa], L);
        }
        for (int b = 0; b < kBodyBones; b++) {
            float s[3];
            KindScale(kKind[b], sx, sy, s);
            out.skin[b] = MatMul(world[b], Scale(s[0], s[1], s[2]));
        }
        // Nintendo leaves the head unscaled; ours follows the body's size.
        // Knobs: kHeadBase is the head at an average Mii (64, 64), 1 being
        // Nintendo's size; kHeadFollow 0 keeps it fixed, 1 scales it as much
        // as the body.
        constexpr float kHeadBase = 0.85f, kHeadFollow = 0.6f, kAvg = (0.8325f + 0.885f) * 0.5f;
        const float hs = kHeadBase * std::pow((sx + sy) * 0.5f / kAvg, kHeadFollow);
        out.head = MatMul(world[Head], Scale(hs, hs, hs));
    }

    void Skin(const BodyMesh &b, const Pose &pose, std::vector<float> &pos, std::vector<float> &nrm) {
        const size_t n = b.pos.size() / 3;
        pos.resize(n * 3);
        nrm.resize(n * 3);
        for (size_t v = 0; v < n; v++) {
            const Mat4 &M = pose.skin[b.bone[v]];
            float q[3];
            MatPoint(M, &b.pos[v * 3], &pos[v * 3]);
            MatVector(M, &b.nrm[v * 3], q);   // ponytail: scaled normals renormalised, not inverse-transposed; scales are near 1
            const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) + 1e-9f;
            for (int c = 0; c < 3; c++) nrm[v * 3 + c] = q[c] / l;
        }
    }

    // ---- animations -----------------------------------------------------------
    bool Anims::Load(const char *path) {
        m_anims.clear();
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        std::vector<uint8_t> d;
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        d.resize(sz > 0 ? (size_t)sz : 0);
        const bool rd = sz > 0 && fread(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        size_t o = 0;
        auto need = [&](size_t n) { return rd && o + n <= d.size(); };
        if (!need(12) || memcmp(d.data(), "SLMA", 4)) return false;
        uint32_t ver; uint16_t nj, na;
        memcpy(&ver, &d[4], 4); memcpy(&nj, &d[8], 2); memcpy(&na, &d[10], 2);
        o = 12 + (size_t)nj * 2;
        if (ver != 1 || nj != kAnimJoints || !need(0)) return false;
        for (int a = 0; a < na; a++) {
            if (!need(40)) return false;
            Anim an;
            an.name.assign((const char *)&d[o], strnlen((const char *)&d[o], 32));
            uint16_t frames, keys;
            memcpy(&frames, &d[o + 32], 2); memcpy(&keys, &d[o + 34], 2); memcpy(&an.fps, &d[o + 36], 4);
            o += 40;
            const size_t rootB = (size_t)frames * 12, rotB = (size_t)frames * nj * 8, faceB = (size_t)keys * 5;
            if (!frames || !(an.fps > 0) || !need(rootB + rotB + faceB + ((4 - faceB % 4) % 4))) return false;
            an.frames = frames;
            an.root.resize(frames * 3);  memcpy(an.root.data(), &d[o], rootB); o += rootB;
            an.rot.resize((size_t)frames * nj * 4); memcpy(an.rot.data(), &d[o], rotB); o += rotB;
            an.face.assign(&d[o], &d[o] + faceB); o += faceB + (4 - faceB % 4) % 4;
            m_anims.push_back(std::move(an));
        }
        return !m_anims.empty();
    }

    int Anims::Find(const char *name) const {
        for (size_t i = 0; i < m_anims.size(); i++) if (m_anims[i].name == name) return (int)i;
        return -1;
    }
    float Anims::Duration(int i) const { return (m_anims[i].frames - 1) / m_anims[i].fps; }

    void Anims::Sample(int i, float t, Quat rot[kAnimJoints], float root[3]) const {
        const Anim &a = m_anims[i];
        const float dur = Duration(i);
        if (dur > 0) t = std::fmod(t, dur);
        if (t < 0) t += dur;
        const float f = t * a.fps;
        const int f0 = std::min(a.frames - 1, (int)f), f1 = std::min(a.frames - 1, f0 + 1);
        const float k = f - f0;
        for (int j = 0; j < kAnimJoints; j++) {
            const int16_t *q0 = &a.rot[((size_t)f0 * kAnimJoints + j) * 4], *q1 = &a.rot[((size_t)f1 * kAnimJoints + j) * 4];
            rot[j] = Nlerp({ q0[0] / 32767.0f, q0[1] / 32767.0f, q0[2] / 32767.0f, q0[3] / 32767.0f },
                           { q1[0] / 32767.0f, q1[1] / 32767.0f, q1[2] / 32767.0f, q1[3] / 32767.0f }, k);
        }
        for (int c = 0; c < 3; c++) root[c] = a.root[f0 * 3 + c] + (a.root[f1 * 3 + c] - a.root[f0 * 3 + c]) * k;
    }

    bool Anims::Face(int i, float t, int &mouth, int &brow, int &eye) const {
        const Anim &a = m_anims[i];
        const size_t n = a.face.size() / 5;
        if (!n) return false;
        const float dur = Duration(i);
        if (dur > 0) t = std::fmod(t, dur);
        const int frame = (int)(t * 30.0f);   // face keys count source frames at 30 fps
        size_t hit = 0;
        for (size_t k = 0; k < n; k++) {
            const int fr = a.face[k * 5] | (a.face[k * 5 + 1] << 8);
            if (fr <= frame) hit = k; else break;
        }
        mouth = a.face[hit * 5 + 2]; brow = a.face[hit * 5 + 3]; eye = a.face[hit * 5 + 4];
        return true;
    }
}
