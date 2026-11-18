// A Mii's head as meshes, built from the console's own MiiModel data
// (system data 0100000000000802: ShapeMid.dat, NXTextureMidSRGB.dat).
// Nothing of Nintendo's is shipped: the files are read from the console.
//
// File layout and placement rules follow vee (MIT/Apache-2.0,
// github.com/j0lol/vee) and FFL-Testing (public domain changes).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sl::menu::mii {

    // The CharInfo fields the model needs, under vee's names. Filled from
    // libnx's MiiCharInfo on the console and from studio data in tests.
    struct Parts {
        uint8_t faceline_type = 0, faceline_color = 0, faceline_wrinkle = 0, faceline_make = 0;
        uint8_t hair_type = 0, hair_color = 0, hair_flip = 0;
        uint8_t eye_type = 0, eye_color = 0, eye_scale = 0, eye_aspect = 0, eye_rotate = 0, eye_x = 0, eye_y = 0;
        uint8_t eyebrow_type = 0, eyebrow_color = 0, eyebrow_scale = 0, eyebrow_aspect = 0, eyebrow_rotate = 0,
                eyebrow_x = 0, eyebrow_y = 0;
        uint8_t nose_type = 0, nose_scale = 0, nose_y = 0;
        uint8_t mouth_type = 0, mouth_color = 0, mouth_scale = 0, mouth_aspect = 0, mouth_y = 0;
        uint8_t beard_color = 0, beard_type = 0, mustache_type = 0, mustache_scale = 0, mustache_y = 0;
        uint8_t glass_type = 0, glass_color = 0, glass_scale = 0, glass_y = 0;
        uint8_t mole_type = 0, mole_scale = 0, mole_x = 0, mole_y = 0;
        uint8_t height = 64, build = 64, favorite_color = 0, gender = 0;
    };

    // Studio data (the 46 bytes, de-obfuscated) to Parts. false if malformed.
    bool PartsFromStudio(const uint8_t studio[46], Parts &out);
    // The obfuscated hex string studio URLs carry ("00" seed + 46 bytes).
    bool PartsFromStudioHex(const std::string &hex, Parts &out);

    enum class ShapeKind : uint8_t { Beard, Faceline, Mask, Forehead, Hair, Glasses, Nose, NoseLine, Hat };

    struct Mesh {
        ShapeKind kind;
        std::vector<float> pos;      // xyz per vertex, already offset and scaled
        std::vector<float> nrm;      // xyz per vertex (empty if none)
        std::vector<float> uv;       // uv per vertex (empty if none)
        std::vector<uint16_t> idx;   // triangle list
        float color[4];              // flat colour; textured kinds take their texture instead
    };

    // A texture as stored: Tegra block-linear, mip 0 first. Decode() gives RGBA8.
    struct Texture {
        int width = 0, height = 0, format = -1, mips = 0;   // format: 0 R, 1 RG, 2 RGBA, 3 BC4, 4 BC5, 5 BC7, 6 ASTC4x4
        std::vector<uint8_t> data;                           // inflated, still swizzled
    };

    enum class TexKind : uint8_t { Hat, Eye, Eyebrow, Beard, Wrinkle, Makeup, Glass, Mole, Mouth, Mustache, NoseLine };

    class Resources {
    public:
        // Reads both files whole (about 1.6 MB). false if either is missing or malformed.
        bool Load(const char *shape_path, const char *texture_path);
        bool Loaded() const { return !m_shape.empty() && !m_tex.empty(); }

        // The head's meshes, placed and coloured for this Mii (Mii units,
        // y up, the faceline about 50 tall).
        std::vector<Mesh> BuildHead(const Parts &p) const;

        bool GetTexture(TexKind kind, int index, Texture &out) const;

    private:
        bool ShapeMesh(int table, int index, Mesh &out) const;
        std::vector<uint8_t> m_shape, m_tex;
    };

    // A texture's mip 0 as RGBA8 holding its raw channels (R, RG, BCn, ASTC
    // all land here; single-channel formats fill R and leave A at 255).
    bool Decode(const Texture &t, std::vector<uint8_t> &rgba);

    // A straight-alpha RGBA8 image.
    struct Image {
        int w = 0, h = 0;
        std::vector<uint8_t> px;
    };

    // The textures the head's meshes are drawn with, composited the way
    // FFL's shaders would (see MiiModel.cpp). Empty images where a Mii has
    // nothing (no glasses, plain faceline).
    struct FaceTextures {
        Image mask;       // 512x512: eyes, brows, mouth, mustache, mole
        Image faceline;   // 512x512: skin with wrinkles, makeup, beard stubble
        Image noseline;   // 256x256
        Image glasses;    // 512x512
    };
    bool BuildFaceTextures(const Resources &r, const Parts &p, FaceTextures &out);

    // A face expression, as the mask draws it: the console's special eye and
    // mouth textures (the ^ arc, the round eye, the O) and a few of the
    // regular mouths stand in for the Mii's own, and the brows tilt or lift.
    struct Expression {
        enum Eyes : uint8_t { OwnEyes, SmileEyes, ClosedEyes, WideEyes, kEyes };
        enum Mouth : uint8_t { OwnMouth, Smile, Grin, Open, Talk, Frown, Wavy, kMouths };
        uint8_t eyes = OwnEyes, mouth = OwnMouth;
        int8_t brow_rot = 0, brow_y = 0;   // in the Mii's own rotation / position steps
        bool operator==(const Expression &o) const {
            return eyes == o.eyes && mouth == o.mouth && brow_rot == o.brow_rot && brow_y == o.brow_y;
        }
        bool operator!=(const Expression &o) const { return !(*this == o); }
    };
    // The mask's parts for one Mii, decoded and coloured once, so an
    // expression is only a redraw (a few ms) and not a decode.
    struct MaskKit {
        Parts p;
        Image eye[Expression::kEyes], mouth[Expression::kMouths], brow, mustache, mole;
    };
    bool BuildMaskKit(const Resources &r, const Parts &p, MaskKit &out);
    // The mask (res x res, 512 being the console's) with that expression.
    void DrawMask(const MaskKit &k, const Expression &e, Image &mask, int res = 512);

    // Half float bits to float.
    float HalfToFloat(uint16_t h);
}
