// The 3D Mii in the menu's scene: the console's head and body, idling through
// the Xbox avatar animations, handed to the renderer as screen-space
// triangles (SDL_RenderGeometry) with no depth buffer: back faces culled, the
// body sorted back to front, the head painted in a fixed layer order. Every
// texture the head uses sits in one atlas, so the whole Mii is one draw call.
#pragma once
#include <sl/menu/mii/MiiBody.hpp>
#include <sl/menu/mii/MiiModel.hpp>
#include <cstdint>
#include <vector>

namespace sl::menu::mii {

    // Same layout as SDL3's SDL_Vertex (position, float colour, uv).
    struct Vertex { float x, y, r, g, b, a, u, v; };

    class Scene {
    public:
        // shape/tex: ShapeHigh.dat (or Mid) and NXTextureMidSRGB.dat; body:
        // MiiBody.dat; anims: anims.bin (optional: the bind pose without it).
        // Heavy (inflates ~3 MB): run it off the main thread.
        bool Load(const char *shape, const char *tex, const char *body, const char *anims, const Parts &p);
        // RGBA8 straight alpha; upload once as the texture Build's uvs refer to.
        const Image &Atlas() const { return m_atlas; }
        void DropAtlas() { m_atlas = Image{}; }   // once it is on the GPU
        // Triangles (3 vertices each) for the pose at time t (seconds, a clock
        // that only goes up), framed like the studio all_body render: a size
        // square at (x, y), feet near its foot. alpha fades the whole Mii.
        void Build(double t, float x, float y, float size, float alpha, std::vector<Vertex> &out);
        // 0..1: how far into the hovering pose of the Xbox's avatar picker -
        // arms half raised from the stand, swaying - instead of idling. The
        // pack has no such animation, so it is made from the stand and the
        // bind (T) pose.
        void SetFloat(float k) { m_float = k; }
        // The face follows the animations' face keys (and blinks on its own),
        // which redraws the mask. After a Build that changed it this returns
        // the new mask, doubled like the atlas (2res x res), to upload over
        // the atlas rect (0, res, 2res, res) of an atlas scaled to res/512;
        // nullptr when it is unchanged.
        const Image *MaskUpdate(int res = 512);

    private:
        struct Part {
            std::vector<float> pos, nrm, uv;   // uv already in the atlas
            std::vector<uint16_t> idx;
            float color[4];
            int layer = 0;                     // head draw order (see Load)
            float winding = 0;                 // +-1: which way its triangles face; 0 never culled
            bool twoSided = false;             // hair: its inside shows too
        };
        void PickAnim(double t);

        Parts m_parts;
        BodyMesh m_body;
        float m_bodyWinding = 0;
        std::vector<Part> m_head;
        Image m_atlas;
        float m_white[2] = {};                 // atlas uv of a white texel
        Anims m_anims;
        bool m_haveAnims = false;
        std::vector<int> m_idles;              // animations to idle through
        std::vector<int> m_extras;             // and now and then one of these
        int m_anim = -1;
        double m_start = 0;
        Quat m_from[kAnimJoints];              // the pose a change blends out of
        float m_fromRoot[3] = {};
        Quat m_last[kAnimJoints];
        float m_lastRoot[3] = {};
        bool m_blend = false;
        float m_float = 0;
        MaskKit m_kit;
        Expression m_expr;
        bool m_maskDirty = false;
        double m_blinkAt = 0;
        Image m_maskImg, m_maskDouble;
        uint32_t m_rng = 0x2545F491u;
        // Scratch kept between frames.
        struct Tri { int layer; float depth, z[3]; Vertex v[3]; };
        std::vector<Tri> m_tris;
        std::vector<float> m_bp, m_bn, m_faceZ;
    };
}
