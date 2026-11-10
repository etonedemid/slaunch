// Hold ZL+ZR for a second on the home screen and the layout blows up. Every
// text, icon and card the layout draws becomes its own rigid piece (gravity,
// spin, walls, a floor, pieces shoving each other into a heap), some text
// bursts into shrapnel, images and cards turn into 3D boxes that shatter
// (the closer to the blast, the more), an orange fireball goes off, and the wallpaper stays
// put behind it all. The list then keeps scrolling: rows ride in the way the
// layout moves them and break loose one by one. Any button puts the original
// screen back together.
#include <sl/menu/ui/Menu.hpp>
#include <sl/menu/ui/Locale.hpp>
#include <SDL3_image/SDL_image.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include "Menu_Internal.hpp"

namespace sl::menu::ui {

    namespace {
        constexpr float kGravity = 2400.0f;
        constexpr u64   kHoldMs = 1000;
        constexpr int   kMaxPieces = 420;        // sweep-and-prune keeps the pair test cheap
        constexpr float kScrollEveryS = 0.09f;   // layout step while things fall
        constexpr int   kMaxScroll = 150;
        constexpr float kAutoBackS = 12.0f;      // reassemble on its own
        constexpr float kBackS = 0.55f, kBackStagger = 0.25f;
        constexpr float kShardChance = 0.4f;     // text pieces that burst instead
        constexpr float kFireS = 1.1f;
        constexpr float kTau = 6.2831853f;
        constexpr float kRestitution = 0.15f, kFriction = 0.6f;
        constexpr int   kSubsteps = 6;
        constexpr float kCrackV = 1300.0f;       // closing speed that breaks a box
        constexpr float kMinShard = 26.0f;       // smallest side that still breaks
        constexpr float kPi = 3.14159265f;

        enum { Off, Flying, Back };

        float Rnd(float lo, float hi) { return lo + (hi - lo) * (float)rand() / (float)RAND_MAX; }
        float Secs(u64 from, u64 to) { return (float)(to - from) / (float)armGetSystemTickFreq(); }

        float Wrap(float r) { return r - kTau * std::round(r / kTau); }   // to -pi..pi

        // Cut p in two across its longer side, at a random point, and append
        // the halves. They keep p's motion, fly apart by `kick`, and go home
        // to their own part of p's spot.
        void Split(std::vector<Menu::BoomPiece> &out, const Menu::BoomPiece &p, float kick) {
            const bool across_x = p.src.w * Rnd(0.7f, 1.3f) >= p.src.h;
            const float f = Rnd(0.3f, 0.7f);
            const float ca = std::cos(p.a), sa = std::sin(p.a);
            for (int half = 0; half < 2; half++) {
                Menu::BoomPiece c = p;
                float ox = 0.0f, oy = 0.0f;              // child centre from p's, unturned
                if (across_x) {
                    c.src.w = half ? p.src.w * (1.0f - f) : p.src.w * f;
                    c.src.x = half ? p.src.x + p.src.w * f : p.src.x;
                    ox = c.src.x + c.src.w * 0.5f - (p.src.x + p.src.w * 0.5f);
                } else {
                    c.src.h = half ? p.src.h * (1.0f - f) : p.src.h * f;
                    c.src.y = half ? p.src.y + p.src.h * f : p.src.y;
                    oy = c.src.y + c.src.h * 0.5f - (p.src.y + p.src.h * 0.5f);
                }
                c.hx = p.hx + ox; c.hy = p.hy + oy;
                const float lx = ox * std::cos(p.ry), ly = oy * std::cos(p.rx);   // tumble squashes
                const float wx = lx * ca - ly * sa, wy = lx * sa + ly * ca;
                const float d = std::sqrt(wx * wx + wy * wy) + 1.0f;
                c.x = p.x + wx; c.y = p.y + wy;
                c.vx = p.vx - p.va * wy + wx / d * kick + Rnd(-kick, kick) * 0.3f;
                c.vy = p.vy + p.va * wx + wy / d * kick + Rnd(-kick, kick) * 0.3f;
                c.va = p.va + Rnd(-4.0f, 4.0f);
                c.vrx = p.vrx + Rnd(-6.0f, 6.0f); c.vry = p.vry + Rnd(-6.0f, 6.0f);
                c.depth = std::fmin(p.depth, std::fmin(c.src.w, c.src.h) * 0.5f);
                c.nocrack = 0.12f; c.crack = false;
                out.push_back(c);
            }
        }

        float Median(std::vector<float> &v) {
            if (v.empty()) return 0.0f;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            return v[v.size() / 2];
        }
    }

    // Draw the layout onto a clear page and turn whatever is new on it into
    // pieces. `blast`: the first capture, which flies outward; later ones
    // (rows the scroll brought in) ride along with the scroll first.
    bool Menu::BoomCapture(bool blast) {
        if (!m_gfx->BeginCapture()) return false;
        DrawMain();
        m_gfx->EndCapture();

        std::vector<gfx::Gfx::Drawn> all = m_gfx->Captured();
        std::unordered_map<u64, int> seen_here;      // "H" twice is two pieces
        std::vector<u64> keys(all.size());
        for (size_t i = 0; i < all.size(); i++)
            keys[i] = all[i].key * 31 + (u64)seen_here[all[i].key]++;

        // How far the layout moved since the last capture, from everything
        // drawn in both: the speed and direction new rows travel in.
        std::unordered_map<u64, SDL_FPoint> pos;
        std::vector<float> dxs, dys;
        for (size_t i = 0; i < all.size(); i++) {
            const SDL_FPoint c { all[i].r.x + all[i].r.w * 0.5f, all[i].r.y + all[i].r.h * 0.5f };
            pos[keys[i]] = c;
            auto it = m_boom_prev.find(keys[i]);
            if (it != m_boom_prev.end()) { dxs.push_back(c.x - it->second.x); dys.push_back(c.y - it->second.y); }
        }
        m_boom_prev.swap(pos);
        if (!blast && !dxs.empty()) {
            m_boom_vx = Median(dxs) / kScrollEveryS;
            m_boom_vy = Median(dys) / kScrollEveryS;
        }

        // Biggest first, so an icon on a card goes with the card instead of
        // becoming a piece of its own cut out of it.
        std::vector<size_t> order(all.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return all[a].r.w * all[a].r.h > all[b].r.w * all[b].r.h; });
        std::vector<SDL_FRect> kept;
        for (size_t i : order) {
            const SDL_FRect &r = all[i].r;
            // Later captures only take the content: the header and the footer
            // (clock, counter, hints) are already flying, and the counter
            // reads differently at every step.
            if (!blast && (r.y < 110.0f || r.y + r.h > gfx::Gfx::Height - 50.0f)) continue;
            // ...and only what came in at the edge the scroll feeds from. The
            // selection's own details (play time and so on) appear mid-screen
            // at every step and are not rows arriving.
            if (!blast && (m_boom_dir == Btn::Down ? r.y + r.h * 0.5f < gfx::Gfx::Height * 0.62f
                                                   : r.x + r.w * 0.5f < gfx::Gfx::Width * 0.62f))
                continue;
            if (!m_boom_keys.insert(keys[i]).second) continue;   // already flying
            bool inside = false;
            for (const SDL_FRect &k : kept) {
                const float ix = std::fmax(0.0f, std::fmin(r.x + r.w, k.x + k.w) - std::fmax(r.x, k.x));
                const float iy = std::fmax(0.0f, std::fmin(r.y + r.h, k.y + k.h) - std::fmax(r.y, k.y));
                if (ix * iy > r.w * r.h * 0.6f) { inside = true; break; }
            }
            if (inside || (int)m_boom.size() >= kMaxPieces) continue;
            BoomPiece p{};
            if (!m_gfx->Keep(r, p.src)) continue;        // off screen, or atlas full
            kept.push_back(r);
            const float left = std::fmax(0.0f, r.x), top = std::fmax(0.0f, r.y);
            p.x = p.hx = left + p.src.w * 0.5f;
            p.y = p.hy = top + p.src.h * 0.5f;
            p.home_ok = blast;
            if (!blast) {                                // rides the scroll, then lets go
                p.ride = Rnd(0.2f, 1.0f);
                p.va = Rnd(-5.0f, 5.0f);
                m_boom.push_back(p);
                continue;
            }
            float dx = p.x - m_boom_ox, dy = p.y - m_boom_oy;
            const float dist = std::sqrt(dx * dx + dy * dy) + 1.0f;
            dx /= dist; dy /= dist;
            const float speed = (700.0f + 1900.0f * std::fmax(0.0f, 1.0f - dist / 800.0f))
                              * Rnd(0.6f, 1.2f);
            p.vx = dx * speed;
            p.vy = dy * speed - Rnd(300.0f, 800.0f);          // up and out
            p.va = Rnd(-12.0f, 12.0f);                       // rad/s

            // Images and cards: boxes, broken up the more the closer they sat.
            if (all[i].solid) {
                p.depth = std::fmax(6.0f, std::fmin(22.0f, std::fmin(p.src.w, p.src.h) * 0.12f));
                p.vrx = Rnd(-8.0f, 8.0f); p.vry = Rnd(-8.0f, 8.0f);
                const float near = std::fmax(0.0f, 1.0f - dist / 700.0f);
                int cuts = (int)(near * near * 9.0f + Rnd(0.0f, 1.5f));
                std::vector<BoomPiece> bits { p };
                while (cuts-- > 0 && (int)(m_boom.size() + bits.size()) < kMaxPieces) {
                    auto big = std::max_element(bits.begin(), bits.end(), [](const BoomPiece &a, const BoomPiece &b) {
                        return a.src.w * a.src.h < b.src.w * b.src.h; });
                    if (std::fmin(big->src.w, big->src.h) < kMinShard) break;
                    const BoomPiece whole = *big;
                    bits.erase(big);
                    Split(bits, whole, 150.0f + 500.0f * near);
                }
                m_boom.insert(m_boom.end(), bits.begin(), bits.end());
                continue;
            }

            // Long text never flies as one bar: it breaks into a few parts,
            // and some of it all the way to shrapnel, thrown harder.
            const float w = p.src.w, h = p.src.h;
            if (w > h * 3.0f && h >= 10.0f) {
                const bool shrapnel = Rnd(0.0f, 1.0f) < kShardChance;
                const int rows = shrapnel && h > 34.0f ? 2 : 1;
                const float fh = h / rows;
                for (float x = 0.0f; x < w - 0.5f; ) {
                    float fw = h * (shrapnel ? Rnd(0.5f, 1.2f) : Rnd(1.3f, 3.0f));
                    if (w - x - fw < h * 0.5f) fw = w - x;     // no sliver at the end
                    for (int row = 0; row < rows && (int)m_boom.size() < kMaxPieces; row++) {
                        BoomPiece s = p;
                        s.src = SDL_FRect{ p.src.x + x, p.src.y + row * fh, fw, fh };
                        s.x = s.hx = left + x + fw * 0.5f;
                        s.y = s.hy = top + row * fh + fh * 0.5f;
                        const float kick = shrapnel ? 700.0f : 250.0f;
                        s.vx = p.vx + Rnd(-kick, kick);
                        s.vy = p.vy + Rnd(-kick * 1.3f, kick * 0.4f);
                        s.va = Rnd(-1.0f, 1.0f) * (shrapnel ? 25.0f : 10.0f);
                        m_boom.push_back(s);
                    }
                    x += fw;
                }
                continue;
            }
            m_boom.push_back(p);
        }
        return true;
    }

    void Menu::PollBoom() {
        const u64 now = armGetSystemTick();
        if (!m_trig_held) { m_trig_tick = 0; m_trig_spent = false; }
        else if (!m_trig_tick) m_trig_tick = now;

        if (m_boom_state == Off) {
            const bool idle = m_screen == Screen::Main && m_dialog == Dialog::None &&
                              !m_options_open && m_launch_tick == 0;
            if (!idle || !m_trig_tick || m_trig_spent ||
                (now - m_trig_tick) * 1000 / armGetSystemTickFreq() < kHoldMs) return;
            m_trig_spent = true;                         // one boom per hold
            m_boom.clear();
            m_boom_keys.clear();
            m_boom_prev.clear();
            m_boom_ox = gfx::Gfx::Width * 0.5f;
            m_boom_oy = gfx::Gfx::Height * 0.55f;
            m_boom_vx = m_boom_vy = 0.0f;
            if (!BoomCapture(true) || m_boom.empty()) { m_gfx->FreeCapture(); return; }
            m_sfx.Play(audio::Sfx::Explode);
            m_boom_state = Flying;
            m_boom_tick = m_boom_last = m_boom_step = now;
            m_boom_scrolled = 0;
            // Only layouts whose rows run down the screen keep scrolling;
            // the horizontal ones just blow up what is on screen.
            m_boom_dir = Btn::Down;
            m_boom_scroll_done = !(m_ui_mode == UiMode::List || m_ui_mode == UiMode::Grid ||
                                   m_ui_mode == UiMode::XMB || m_ui_mode == UiMode::Blades);
            return;
        }
        if (m_boom_state != Flying) return;
        // Walk the layout on so what was off screen comes in and drops.
        // OnButtonMain, not OnButton: the layout's own step, without the click
        // every step would make.
        if (!m_boom_scroll_done && Secs(m_boom_step, now) >= kScrollEveryS &&
            (int)m_boom.size() < kMaxPieces) {
            m_boom_step = now;
            const int before = m_cursor;
            u64 id = 0;
            OnButtonMain(m_boom_dir, id);
            if (m_cursor != before) m_boom_scrolled++;   // undone on the way back
            if (m_cursor <= before || m_boom_scrolled >= kMaxScroll) m_boom_scroll_done = true;
            else BoomCapture(false);
        }
        if (Secs(m_boom_tick, now) > kAutoBackS) BoomReassemble();
    }

    void Menu::BoomReassemble() {
        if (m_boom_state != Flying) return;
        for (BoomPiece &p : m_boom) {
            if (p.ride > 0.0f) { p.ride = 0.0f; p.vx = m_boom_vx; p.vy = m_boom_vy; }
            p.x0 = p.x; p.y0 = p.y;
            p.a0 = Wrap(p.a);                                // shortest way upright
            p.rx0 = Wrap(p.rx); p.ry0 = Wrap(p.ry);
            p.delay = Rnd(0.0f, kBackStagger);
        }
        // Step back as many times as the scroll stepped forward, so every
        // layout's own state (scroll, rows, categories) is where it was.
        const Btn back = m_boom_dir == Btn::Down ? Btn::Up : Btn::Left;
        for (u64 id = 0; m_boom_scrolled > 0; m_boom_scrolled--) OnButtonMain(back, id);
        m_boom_state = Back;
        m_boom_tick = armGetSystemTick();
    }

    // Rigid boxes. Every piece is a rotated rectangle with a mass (its area)
    // and the matching moment of inertia; overlaps are found with the
    // separating axis test and resolved with an impulse at the contact point,
    // friction included, so pieces tip, slide and stack the way boxes do. The
    // floor and the walls work per corner, which is what topples a piece that
    // lands on its end.
    namespace {
        struct Body {
            float cx, cy, ux, uy, vx, vy, hw, hh;   // centre, axes, half extents
            float im, ii;                           // inverse mass, inverse inertia
        };

        float Cross(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

        bool Inside(const Body &b, float px, float py) {
            const float dx = px - b.cx, dy = py - b.cy;
            return std::fabs(dx * b.ux + dy * b.uy) <= b.hw + 0.01f &&
                   std::fabs(dx * b.vx + dy * b.vy) <= b.hh + 0.01f;
        }

        void Corners(const Body &b, float out[4][2]) {
            const float ax = b.ux * b.hw, ay = b.uy * b.hw, bx = b.vx * b.hh, by = b.vy * b.hh;
            out[0][0] = b.cx - ax - bx; out[0][1] = b.cy - ay - by;
            out[1][0] = b.cx + ax - bx; out[1][1] = b.cy + ay - by;
            out[2][0] = b.cx + ax + bx; out[2][1] = b.cy + ay + by;
            out[3][0] = b.cx - ax + bx; out[3][1] = b.cy - ay + by;
        }

        // Impulse between piece p (at contact point x,y) and piece q (or a
        // wall when q is null), along normal n pointing from p towards q.
        float Impulse(Menu::BoomPiece &p, const Body &bp, Menu::BoomPiece *q, const Body *bq,
                     float x, float y, float nx, float ny) {
            const float rpx = x - bp.cx, rpy = y - bp.cy;
            float vx = -(p.vx - p.va * rpy), vy = -(p.vy + p.va * rpx);   // q's velocity minus p's
            float rqx = 0, rqy = 0;
            if (q) {
                rqx = x - bq->cx; rqy = y - bq->cy;
                vx += q->vx - q->va * rqy; vy += q->vy + q->va * rqx;
            }
            const float vn = vx * nx + vy * ny;
            if (vn > 0.0f) return vn;                               // already separating
            const float iq = q ? bq->im : 0.0f, iiq = q ? bq->ii : 0.0f;
            const float rpn = Cross(rpx, rpy, nx, ny), rqn = Cross(rqx, rqy, nx, ny);
            const float e = vn < -80.0f ? kRestitution : 0.0f;      // no bouncing at rest
            const float j = -(1.0f + e) * vn / (bp.im + iq + rpn * rpn * bp.ii + rqn * rqn * iiq);
            // Friction along the surface, capped by the normal impulse.
            const float tx = -ny, ty = nx;
            const float vt = vx * tx + vy * ty;
            const float rpt = Cross(rpx, rpy, tx, ty), rqt = Cross(rqx, rqy, tx, ty);
            float jt = -vt / (bp.im + iq + rpt * rpt * bp.ii + rqt * rqt * iiq);
            jt = std::fmax(-kFriction * j, std::fmin(kFriction * j, jt));
            const float ix = nx * j + tx * jt, iy = ny * j + ty * jt;
            p.vx -= ix * bp.im; p.vy -= iy * bp.im; p.va -= Cross(rpx, rpy, ix, iy) * bp.ii;
            if (q) { q->vx += ix * iq; q->vy += iy * iq; q->va += Cross(rqx, rqy, ix, iy) * iiq; }
            return vn;
        }

        // A box that hits hard enough breaks (once the dust of its last break settles).
        void Hit(Menu::BoomPiece &p, float vn) {
            if (vn < -kCrackV && p.depth > 0.0f && p.nocrack <= 0.0f &&
                std::fmin(p.src.w, p.src.h) >= kMinShard) p.crack = true;
        }

        Body MakeBody(const Menu::BoomPiece &p) {
            Body b;
            b.cx = p.x; b.cy = p.y;
            b.ux = std::cos(p.a); b.uy = std::sin(p.a);
            b.vx = -b.uy; b.vy = b.ux;
            // A tumbled box shows its thickness instead of part of its face.
            b.hw = 0.5f * (p.src.w * std::fabs(std::cos(p.ry)) + p.depth * std::fabs(std::sin(p.ry)));
            b.hh = 0.5f * (p.src.h * std::fabs(std::cos(p.rx)) + p.depth * std::fabs(std::sin(p.rx)));
            b.hw = std::fmax(b.hw, 1.0f); b.hh = std::fmax(b.hh, 1.0f);
            const float m = p.src.w * p.src.h * 0.001f;
            b.im = 1.0f / m;
            b.ii = 12.0f / (m * (p.src.w * p.src.w + p.src.h * p.src.h));
            return b;
        }

        // Separating axis test of two boxes; on overlap, the push-out normal
        // (from a towards b), its depth and a contact point.
        bool Collide(const Body &a, const Body &b, float &nx, float &ny, float &depth,
                     float &px, float &py) {
            const float axes[4][2] = { { a.ux, a.uy }, { a.vx, a.vy }, { b.ux, b.uy }, { b.vx, b.vy } };
            const float dx = b.cx - a.cx, dy = b.cy - a.cy;
            depth = 1e9f;
            for (const auto &u : axes) {
                const float ra = a.hw * std::fabs(a.ux * u[0] + a.uy * u[1]) + a.hh * std::fabs(a.vx * u[0] + a.vy * u[1]);
                const float rb = b.hw * std::fabs(b.ux * u[0] + b.uy * u[1]) + b.hh * std::fabs(b.vx * u[0] + b.vy * u[1]);
                const float d = dx * u[0] + dy * u[1];
                const float o = ra + rb - std::fabs(d);
                if (o <= 0.0f) return false;
                if (o < depth) { depth = o; nx = d < 0 ? -u[0] : u[0]; ny = d < 0 ? -u[1] : u[1]; }
            }
            // Contact: the corners of each box that sit inside the other.
            float ca[4][2], cb[4][2], sx = 0, sy = 0;
            int n = 0;
            Corners(a, ca); Corners(b, cb);
            for (int i = 0; i < 4; i++) {
                if (Inside(b, ca[i][0], ca[i][1])) { sx += ca[i][0]; sy += ca[i][1]; n++; }
                if (Inside(a, cb[i][0], cb[i][1])) { sx += cb[i][0]; sy += cb[i][1]; n++; }
            }
            if (n) { px = sx / n; py = sy / n; }
            else   { px = (a.cx + b.cx) * 0.5f; py = (a.cy + b.cy) * 0.5f; }   // edges crossing
            return true;
        }
    }

    void Menu::StepBoom(float dt) {
        const float W = (float)gfx::Gfx::Width, H = (float)gfx::Gfx::Height;
        for (BoomPiece &p : m_boom) {
            if (p.ride > 0.0f) {                         // still on the scrolling list
                p.x += m_boom_vx * dt; p.y += m_boom_vy * dt;
                p.ride -= dt;
                if (p.ride <= 0.0f) {                    // breaks loose
                    p.vx = m_boom_vx * 0.5f + Rnd(-150.0f, 150.0f);
                    p.vy = m_boom_vy * 0.5f - Rnd(0.0f, 250.0f);
                }
                continue;
            }
            p.vy += kGravity * dt;
            p.x += p.vx * dt; p.y += p.vy * dt; p.a += p.va * dt;
            p.rx += p.vrx * dt; p.ry += p.vry * dt;
            p.va *= 0.999f;
            p.nocrack -= dt;
        }

        // Broad phase: sweep along x, so only pieces whose spans overlap are tested.
        const int n = (int)m_boom.size();
        std::vector<Body> body(n);
        std::vector<char> touched(n, 0);
        std::vector<std::pair<float, int>> span;
        span.reserve(n);
        for (int i = 0; i < n; i++) {
            body[i] = MakeBody(m_boom[i]);
            if (m_boom[i].ride > 0.0f) continue;         // riders pass through
            span.push_back({ m_boom[i].x - std::fmax(body[i].hw, body[i].hh) * 1.42f, i });
        }
        std::sort(span.begin(), span.end());
        for (size_t s = 0; s < span.size(); s++) {
            const int i = span[s].second;
            const float reach = m_boom[i].x + std::fmax(body[i].hw, body[i].hh) * 1.42f;
            for (size_t t = s + 1; t < span.size() && span[t].first <= reach; t++) {
                const int j = span[t].second;
                float nx, ny, depth, px, py;
                if (!Collide(body[i], body[j], nx, ny, depth, px, py)) continue;
                // Push apart by inverse mass (a little short, to stay stable),
                // then exchange momentum.
                const float share = body[i].im / (body[i].im + body[j].im);
                const float k = std::fmax(0.0f, depth - 0.3f) * 0.8f;
                m_boom[i].x -= nx * k * share;          m_boom[i].y -= ny * k * share;
                m_boom[j].x += nx * k * (1.0f - share); m_boom[j].y += ny * k * (1.0f - share);
                body[i].cx = m_boom[i].x; body[i].cy = m_boom[i].y;
                body[j].cx = m_boom[j].x; body[j].cy = m_boom[j].y;
                const float vn = Impulse(m_boom[i], body[i], &m_boom[j], &body[j], px, py, nx, ny);
                Hit(m_boom[i], vn); Hit(m_boom[j], vn);
                touched[i] = touched[j] = 1;
            }
        }

        // Floor and walls, corner by corner.
        for (int i = 0; i < n; i++) {
            BoomPiece &p = m_boom[i];
            if (p.ride > 0.0f) continue;
            Body &b = body[i];
            b.cx = p.x; b.cy = p.y;
            float c[4][2];
            Corners(b, c);
            float push_x = 0.0f, push_y = 0.0f;
            for (const auto &k : c) {
                if (k[1] > H)       { push_y = std::fmin(push_y, H - k[1]); touched[i] = 1;
                                      Hit(p, Impulse(p, b, nullptr, nullptr, k[0], k[1], 0.0f, 1.0f)); }
                if (k[0] < 0.0f)    { push_x = std::fmax(push_x, -k[0]);
                                      Hit(p, Impulse(p, b, nullptr, nullptr, k[0], k[1], -1.0f, 0.0f)); }
                if (k[0] > W)       { push_x = std::fmin(push_x, W - k[0]);
                                      Hit(p, Impulse(p, b, nullptr, nullptr, k[0], k[1], 1.0f, 0.0f)); }
            }
            p.x += push_x; p.y += push_y;
            // Lying on something, a tumbling box rolls onto its nearest face.
            if (p.depth > 0.0f) {
                const float k = touched[i] ? 1.0f : 0.0f;
                const float tx = kPi * std::round(p.rx / kPi), ty = kPi * std::round(p.ry / kPi);
                p.vrx += ((tx - p.rx) * 250.0f * k - p.vrx * (0.3f + 18.0f * k)) * dt;
                p.vry += ((ty - p.ry) * 250.0f * k - p.vry * (0.3f + 18.0f * k)) * dt;
            }
        }

        // Break what hit hard: each cracked box becomes two, in its place.
        for (int i = n - 1; i >= 0; i--) {
            if (!m_boom[i].crack) continue;
            if ((int)m_boom.size() >= kMaxPieces) { m_boom[i].crack = false; continue; }
            const BoomPiece whole = m_boom[i];
            m_boom[i] = m_boom.back();
            m_boom.pop_back();
            Split(m_boom, whole, 120.0f);
        }
    }

    void Menu::DrawBoomFire(float t) {
        if (t >= kFireS || !m_gfx->FxBegin(m_gfx->FxProgram(kBoomVs, kBoomFs))) return;
        m_gfx->FxSet("uBoom", m_boom_ox, m_boom_oy, t, 4.0f);
        m_gfx->FxStrip(2);
        m_gfx->FxEnd();
    }

    bool Menu::DrawBoom() {
        if (m_boom_state == Off) return false;
        const u64 now = armGetSystemTick();
        const float t = Secs(m_boom_tick, now);
        const float dt = std::fmin(Secs(m_boom_last, now), 1.0f / 30.0f);
        m_boom_last = now;
        if (m_boom_state == Flying)
            for (int s = 0; s < kSubsteps; s++) StepBoom(dt / kSubsteps);

        DrawBackground();
        float shake_x = 0.0f, shake_y = 0.0f;
        if (m_boom_state == Flying && t < 0.4f) {
            const float k = (1.0f - t / 0.4f) * 14.0f;
            shake_x = Rnd(-k, k); shake_y = Rnd(-k, k);
        }
        bool done = true;
        for (BoomPiece &p : m_boom) {
            float x = p.x, y = p.y, a = p.a, rx = p.rx, ry = p.ry;
            if (m_boom_state == Back) {
                if (!p.home_ok) {                          // rows from later: fall away
                    p.vy += kGravity * dt; p.x += p.vx * dt; p.y += p.vy * dt; p.a += p.va * dt;
                    p.rx += p.vrx * dt; p.ry += p.vry * dt;
                    if (p.y < gfx::Gfx::Height + 400.0f) done = false;
                    x = p.x; y = p.y; a = p.a; rx = p.rx; ry = p.ry;
                } else {
                    float u = (t - p.delay) / kBackS;
                    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
                    if (u < 1.0f) done = false;
                    const float e = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);   // ease-out
                    x = p.x0 + (p.hx - p.x0) * e; y = p.y0 + (p.hy - p.y0) * e; a = p.a0 * (1.0f - e);
                    rx = p.rx0 * (1.0f - e); ry = p.ry0 * (1.0f - e);
                }
            }
            if (p.depth > 0.0f) m_gfx->DrawPieceBox(p.src, x + shake_x, y + shake_y, a, rx, ry, p.depth);
            else                m_gfx->DrawPiece(p.src, x + shake_x, y + shake_y, a * 57.29578f);
        }
        if (m_boom_state == Flying) {
            DrawBoomFire(t);
            if (t < 0.12f)                                 // the flash
                m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height,
                                SDL_Color{255, 240, 200, (Uint8)(200.0f * (1.0f - t / 0.12f))});
        }
        if (m_boom_state == Back && (done || t > 3.0f)) {
            m_boom_state = Off;
            m_boom.clear();
            m_boom_keys.clear();
            m_boom_prev.clear();
            m_gfx->FreeCapture();
        }
        return true;
    }

} // namespace sl::menu::ui
