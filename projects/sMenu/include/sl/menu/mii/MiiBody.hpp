// The 3D Mii's body: the console's own Mii body (MiiBody.dat, "NFVB", from
// the cabinet applet 0100000000001002's RomFS, read from the SD at run time,
// never shipped), sized by height and build the way nn::mii's
// VariableIconBody does, and posed by animations converted from an Xbox 360
// avatar export when the user has one (scripts/mii/avatar-anims.py ->
// sdmc:/slaunch/mii/anims.bin), retargeted onto Nintendo's bones.
#pragma once
#include <sl/menu/mii/MiiModel.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace sl::menu::mii {

    constexpr int kAnimJoints = 21;   // the order avatar-anims.py writes
    constexpr int kBodyBones = 25;    // nn::mii VariableIconBody bones

    struct Mat4 { float m[16]; };     // column major
    struct Quat { float x, y, z, w; };

    struct BodyMesh {
        std::vector<float> pos, nrm, col;   // xyz (in its bone's space), xyz, rgba per vertex
        std::vector<uint8_t> bone;           // one bone per vertex
        std::vector<uint16_t> idx;
        Mat4 bind[kBodyBones];               // local bind matrices
        int parent[kBodyBones];
    };
    // The body for the Mii's gender from MiiBody.dat: shirt (hands included)
    // in the favourite colour, trousers grey, as the console draws it.
    bool LoadBody(const char *path, const Parts &p, BodyMesh &out);

    class Anims {
    public:
        bool Load(const char *path);
        int Count() const { return (int)m_anims.size(); }
        int Find(const char *name) const;                 // -1 if absent
        const std::string &Name(int i) const { return m_anims[i].name; }
        float Duration(int i) const;
        // Local joint rotations and the root's offset at time t (looped).
        void Sample(int i, float t, Quat rot[kAnimJoints], float root[3]) const;
        // The face key in force at time t: Xbox mouth/eyebrow/eye layer indices.
        bool Face(int i, float t, int &mouth, int &brow, int &eye) const;
    private:
        struct Anim {
            std::string name;
            float fps = 30;
            int frames = 0;
            std::vector<float> root;     // 3 per frame
            std::vector<int16_t> rot;    // 4 per joint per frame
            std::vector<uint8_t> face;   // 5 per key: frame lo, hi, mouth, brow, eye
        };
        std::vector<Anim> m_anims;
    };

    struct Pose {
        Mat4 skin[kBodyBones];    // bone space -> posed, body scale included
        Mat4 head;                // head mesh (Mii units) -> body space, unscaled
    };
    // rot/root from Anims::Sample; nullptr for the console's bind pose.
    // Body space is Mii units, y up, +z forward, feet on y = 0.
    void ComputePose(const BodyMesh &b, const Parts &p, const Quat *rot, const float *root, Pose &out);

    // Posed copies of the body's positions and normals.
    void Skin(const BodyMesh &b, const Pose &pose, std::vector<float> &pos, std::vector<float> &nrm);

    // Small helpers the renderers share.
    Mat4 MatIdentity();
    Mat4 MatMul(const Mat4 &a, const Mat4 &b);
    void MatPoint(const Mat4 &m, const float in[3], float out[3]);
    void MatVector(const Mat4 &m, const float in[3], float out[3]);
}
