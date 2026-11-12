// The in-menu updater: the newest release's changed files, fetched by
// net/Ota.cpp on a worker, and a pop-up over the layout while it runs. The
// pop-up borrows each layout's own look - a list line, a Switch-style card, a
// tile, a cover, a box on the shelf, the PS3's update bar, an iTunes-style
// readout under the coverflow, a Steam dialog, a Metro band, a Blades card.
#include <sl/menu/ui/Menu.hpp>
#include <sl/menu/ui/Locale.hpp>
#include <SDL3_image/SDL_image.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include "Menu_Internal.hpp"

namespace sl::menu::ui {

    namespace {
        using gfx::FontSize;
        constexpr int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
        enum { kLight, kSemi, kBold };     // the faces MText knows (Metro's Selawik, a Blades skin's)
        constexpr int kMtGX = 231;          // Metro's left margin (Menu_Metro.cpp)
        constexpr int kBpHead = 48;         // a Blades pop-up's title bar (Menu_Xbox.cpp)
        constexpr SDL_Color kWhite{ 255, 255, 255, 255 };
        // Enough that the layout's own words and hint row fade out behind.
        constexpr Uint8 kDim = 236;

        SDL_Color Scale(SDL_Color c, float f) {
            auto ch = [f](Uint8 v) { return (Uint8)std::clamp((float)v * f, 0.0f, 255.0f); };
            return SDL_Color{ ch(c.r), ch(c.g), ch(c.b), c.a };
        }
        SDL_Color Avg(SDL_Color a, SDL_Color b, Uint8 alpha) {
            return SDL_Color{ (Uint8)((a.r + b.r) / 2), (Uint8)((a.g + b.g) / 2), (Uint8)((a.b + b.b) / 2), alpha };
        }
    }

    bool Menu::CanOta() {
#if defined(SL_QEXT)
        return true;          // the release zip is this build's layout
#elif defined(SL_SIMULATOR)
        return getenv("SL_OTA_DEMO") != nullptr;
#else
        return false;         // the old daemon build: the installer moves it over
#endif
    }

    void Menu::OtaTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        std::string err;
        // The update check may have worked it out already, to ask with the size.
        bool ok = !m->m_ota_files.empty() || ota::Plan(m->m_upd_zip, "sdmc:", m->m_ota_files, m->m_ota_pr, err);
        // What it changed, for when an update goes wrong in the field.
        if (FILE *fp = fopen("sdmc:/slaunch/boot.log", "a")) {
            fprintf(fp, "update to %s: %s, %zu files\n", m->m_upd_latest.c_str(), ok ? "planned" : err.c_str(),
                    m->m_ota_files.size());
            for (const ota::Entry &e : m->m_ota_files) fprintf(fp, "  %s (%u bytes)\n", e.name.c_str(), e.csize);
            fclose(fp);
        }
        if (ok && !m->m_ota_files.empty()) {
            ok = ota::Apply(m->m_upd_zip, "sdmc:", m->m_ota_files, m->m_ota_pr, err);
            if (FILE *fp = fopen("sdmc:/slaunch/boot.log", "a")) {
                fprintf(fp, "update to %s: %s\n", m->m_upd_latest.c_str(), ok ? "installed" : err.c_str());
                fclose(fp);
            }
        }
        m->m_ota_err = err;
        m->m_ota_ok.store(ok, std::memory_order_release);
        m->m_ota_finished.store(true, std::memory_order_release);
    }

    void Menu::StartOta() {
        if (m_ota_running || m_upd_running || m_upd_zip.empty()) return;
        m_ota = OtaStep::Busy;
        m_ota_tick = armGetSystemTick();
        m_ota_err.clear();
        m_ota_files.swap(m_upd_plan);   // used once: a retry looks again
        m_upd_plan.clear();
        m_ota_pr.stage = 0;
        m_ota_pr.done = m_ota_pr.total = 0;
        m_ota_pr.file = m_ota_pr.files = 0;
        m_ota_pr.cancel = false;
        m_ota_finished.store(false);
        m_ota_ok.store(false);
        // Bigger than the check's: TLS handshakes and inflate run on it.
        if (R_SUCCEEDED(threadCreate(&m_ota_thread, &Menu::OtaTrampoline, this, nullptr, 0x10000, 0x3B, -2))) {
            threadStart(&m_ota_thread);
            m_ota_running = true;
        } else {
            m_ota = OtaStep::Failed;
            m_ota_err = T("Could not start the download.");
        }
    }

    void Menu::PollOta() {
#ifdef SL_SIMULATOR
        // SL_OTA_DEMO=live|ask|check|get|install|done|same|fail opens the pop-up in that
        // state over the home screen, for screenshots: there is no card to update.
        static bool demo_done = false;
        if (const char *d = getenv("SL_OTA_DEMO"); d && !demo_done && m_screen == Screen::Main) {
            demo_done = true;
            const std::string s = d;
            if (s == "live") return;   // only CanOta: the real check and plan, against the sim's card
            m_upd_latest = "v9.9.9";
            if (s == "ask") {   // the question instead, as PollUpdateCheck asks it
                m_upd_zip = "x";
                m_upd_bytes = 4456705;
                m_upd_checked = m_upd_available = true;
                return;
            }
            m_ota_tick = armGetSystemTick();
            m_ota = s == "done" ? OtaStep::Done : s == "same" ? OtaStep::Same
                  : s == "fail" ? OtaStep::Failed : OtaStep::Busy;
            m_ota_pr.stage = s == "check" ? 0 : s == "install" ? 2 : 1;
            m_ota_pr.files = s == "check" ? 161 : 3;
            m_ota_pr.file = s == "check" ? 97 : 2;
            m_ota_pr.total = 3156659;
            m_ota_pr.done = m_ota == OtaStep::Busy ? 1294230 : 3156659;
            if (m_ota == OtaStep::Failed) m_ota_err = "Network error: Couldn't resolve host name";
        }
#endif
        if (!m_ota_running || !m_ota_finished.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_ota_thread);
        threadClose(&m_ota_thread);
        m_ota_running = false;
        if (m_ota_ok.load(std::memory_order_acquire)) {
            m_ota = m_ota_files.empty() ? OtaStep::Same : OtaStep::Done;
        } else if (m_ota_pr.cancel.load()) {
            m_ota = OtaStep::Off;
            SetStatus("Update cancelled");
        } else {
            m_ota = OtaStep::Failed;
        }
        m_ota_tick = armGetSystemTick();
    }

    Menu::Action Menu::OnButtonUpdate(Btn b) {
        switch (m_ota) {
            case OtaStep::Busy:
                // Installing is a handful of renames; stopping halfway is worse.
                if (b == Btn::B && m_ota_pr.stage.load() < 2) m_ota_pr.cancel = true;
                break;
            case OtaStep::Done:
            case OtaStep::Same:
                if (b == Btn::A) { m_ota = OtaStep::Off; return Action::PowerReboot; }
                if (b == Btn::B) { m_ota = OtaStep::Off; SetStatus("Restart to finish the update"); }
                break;
            case OtaStep::Failed:
                if (b == Btn::A) StartOta();
                if (b == Btn::B) m_ota = OtaStep::Off;
                break;
            default: break;
        }
        return Action::None;
    }

    void Menu::DrawUpdate() {
        const Theme &t = m_theme.Current();
        const float secs = (float)(armGetSystemTick() - m_ota_tick) / (float)armGetSystemTickFreq();
        const int stage = m_ota_pr.stage.load();
        const bool busy = m_ota == OtaStep::Busy, cancelling = busy && m_ota_pr.cancel.load();

        // What it says, the same in every layout.
        char buf[160];
        const std::string title = m_ota == OtaStep::Failed ? std::string(T("Update failed"))
                                                           : "sLaunch " + m_upd_latest;
        std::string line, sub;
        float frac = -1.0f;   // below 0: working, no amount to show
        const double mb_done = m_ota_pr.done.load() / 1048576.0, mb_all = m_ota_pr.total.load() / 1048576.0;
        switch (m_ota) {
            case OtaStep::Busy:
                if (cancelling) { line = T("Cancelling..."); break; }
                if (stage == 0) {
                    line = T("Checking what changed...");
                    const int n = m_ota_pr.files.load();
                    if (n > 0) frac = (float)m_ota_pr.file.load() / (float)n;
                } else if (stage == 1) {
                    snprintf(buf, sizeof(buf), "%s  %d / %d", T("Downloading"), m_ota_pr.file.load(), m_ota_pr.files.load());
                    line = buf;
                    snprintf(buf, sizeof(buf), "%.1f / %.1f MB", mb_done, mb_all);
                    sub = buf;
                    if (m_ota_pr.total.load()) frac = (float)(mb_done / mb_all);
                } else {
                    line = T("Installing...");
                    sub = T("Keep the console on.");
                }
                break;
            case OtaStep::Done:
                line = T("Installed. Restart to finish.");
                snprintf(buf, sizeof(buf), "%.1f MB %s", mb_all, T("downloaded"));
                sub = buf;
                frac = 1.0f;
                break;
            case OtaStep::Same:
                line = T("The card already has this version.");
                sub = T("Restart to start it.");
                frac = 1.0f;
                break;
            default:
                line = m_ota_err.empty() ? std::string(T("Something went wrong.")) : m_ota_err;
                sub = T("Nothing was changed.");
                break;
        }
        const int pct = frac < 0 ? -1 : (int)std::lround(std::clamp(frac, 0.0f, 1.0f) * 100);
        char pcts[16] = "";
        if (pct >= 0) snprintf(pcts, sizeof(pcts), "%d%%", pct);

        // A bar: the amount, or a segment sliding along while there is none.
        auto bar = [&](int x, int y, int w, int h, SDL_Color track, SDL_Color fill, float r) {
            if (track.a) XbCard((float)x, (float)y, (float)w, (float)h, r, 0.0f, track);
            float x0 = (float)x, x1;
            if (frac >= 0) {
                x1 = x + w * std::clamp(frac, 0.0f, 1.0f);
            } else {
                const float seg = w * 0.3f, u = std::fmod(secs / 1.4f, 1.0f);
                x0 = std::max((float)x, x - seg + (w + seg) * u);
                x1 = std::min((float)(x + w), x - seg + (w + seg) * u + seg);
            }
            if (x1 - x0 >= 1.0f)
                XbCard(x0, (float)y, x1 - x0, (float)h, std::min(r, (x1 - x0) / 2), 0.0f, fill);
        };
        auto hints = [&] {
            if (busy) { if (stage < 2 && !cancelling) DrawHint({ {{"b"}, "Cancel"} }); }
            else if (m_ota == OtaStep::Failed) DrawHint({ {{"a"}, "Try again"}, {{"b"}, "Back"} });
            else DrawHint({ {{"a"}, "Restart"}, {{"b"}, "Later"} });
        };
        auto card = [&](float x, float y, float w, float h, float r, float sh, SDL_Color fill,
                        float glow = 0.0f, SDL_Color glow_col = kWhite) {
            gfx::Gfx::CardStyle cs;
            cs.radius = r; cs.shadow = sh; cs.fill = fill; cs.glow = glow; cs.glow_col = glow_col;
            if (!m_gfx->Card(nullptr, x, y, w, h, cs, 255))
                m_gfx->FillRect((int)x, (int)y, (int)w, (int)h, fill);
        };
        const char *c_title = title.c_str(), *c_line = line.c_str(), *c_sub = sub.c_str();
        const SDL_Color on_accent = (t.accent.r * 3 + t.accent.g * 6 + t.accent.b) / 10 > 165
                                  ? SDL_Color{ 24, 24, 26, 255 } : kWhite;

        // Metro: a band across the screen, as its dialogs are, with the
        // Windows Phone dots running along it while there is no amount.
        if (m_ui_mode == UiMode::Metro) {
            MetroPalette();
            const int bh = 230, by = (H - bh) / 2, bw = W - 2 * kMtGX;
            m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, 200 });
            m_gfx->FillRect(0, by, W, bh, SDL_Color{ 34, 35, 38, 245 });
            MText(kLight, 32, kMtGX, by + 22, kWhite, c_title);
            MText(kSemi, 20, kMtGX, by + 82, SDL_Color{ 255, 255, 255, 210 }, c_line);
            const int ly = by + 136;
            if (frac >= 0) {
                m_gfx->FillRect(kMtGX, ly, bw, 4, SDL_Color{ 255, 255, 255, 40 });
                m_gfx->FillRect(kMtGX, ly, (int)(bw * frac), 4, m_mt_accent);
            } else {
                for (int i = 0; i < 5; i++) {   // fast at the edges, slow through the middle
                    const float u = std::fmod(secs / 2.2f - i * 0.07f + 1.0f, 1.0f);
                    if (u > 0.8f) continue;
                    const float v = u / 0.8f, e = v + 0.85f * std::sin(v * 6.2831853f) / 6.2831853f;
                    m_gfx->FillRect(kMtGX + (int)(bw * std::clamp(e, 0.0f, 1.0f)), ly - 1, 5, 5, m_mt_accent);
                }
            }
            if (!sub.empty()) MText(kSemi, 18, kMtGX, by + 156, SDL_Color{ 255, 255, 255, 150 }, c_sub);
            if (pct >= 0 && busy) MText(kLight, 32, kMtGX + bw - MWidth(kLight, 32, pcts), by + 22, kWhite, pcts);
            hints();
            return;
        }

        // Blades and NXE: the pale pop-up card in the open blade's colour,
        // with the 360's white bar filling green.
        if (XboxOld()) {
            const SDL_Color base = BladesColorOf(m_xmb_col);
            constexpr int cw = 560, ch = kBpHead + 140;
            m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, 110 });   // on top of the pop-up's own dimming
            const int x = BladesPopup(cw, ch, title), y = std::max(12, (H - ch) / 2);
            MText(kSemi, 20, x + 24, y + kBpHead + 8, Scale(base, 0.24f), c_line);
            const int bx = x + 24, by = y + kBpHead + 48, bw = cw - 48;
            XbCard((float)bx, (float)by, (float)bw, 20.0f, 10.0f, 4.0f, SDL_Color{ 250, 250, 250, 255 });
            bar(bx + 2, by + 2, bw - 4, 16, SDL_Color{ 0, 0, 0, 0 }, SDL_Color{ 104, 176, 38, 255 }, 8.0f);
            XbCard(bx + 4.0f, by + 3.0f, bw - 8.0f, 6.0f, 3.0f, 0.0f, SDL_Color{ 255, 255, 255, 70 });   // gloss
            MText(kSemi, 17, bx, by + 32, Scale(base, 0.36f), c_sub);
            if (pct >= 0) MText(kSemi, 17, bx + bw - MWidth(kSemi, 17, pcts), by + 32, Scale(base, 0.36f), pcts);
            hints();
            return;
        }

        switch (m_ui_mode) {
            case UiMode::List: {
                // A line of the list: the cursor, the name, and a rule for a bar.
                m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, kDim });
                const int x = 60, y = 270;
                m_gfx->Text(FontSize::Normal, x - 22, y, t.accent, ">");
                m_gfx->Text(FontSize::Normal, x, y, t.accent, c_title);
                m_gfx->Text(FontSize::Small, x, y + 50, t.fg, c_line);
                bar(x, y + 92, 560, 3, WithAlpha(t.dim, 70), t.accent, 0.0f);
                if (!sub.empty()) m_gfx->Text(FontSize::Small, x, y + 108, t.dim, c_sub);
                if (pct >= 0) m_gfx->Text(FontSize::Small, x + 560 - m_gfx->TextWidth(FontSize::Small, pcts), y + 108, t.dim, pcts);
                break;
            }
            case UiMode::Grid: {
                // A wide tile: the name over it as the grid's header, the bar
                // filling the tile, the caption in its corner.
                m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, kDim });
                m_gfx->Text(FontSize::Large, 44, 64, t.title, c_title);
                const int tw = 420, th = 210, tx = (W - tw) / 2, ty = (H - th) / 2;
                m_gfx->FillRect(tx, ty, tw, th, Scale(t.accent, 0.45f));
                if (frac >= 0) m_gfx->FillRect(tx, ty, (int)(tw * frac), th, t.accent);
                else bar(tx, ty, tw, th, SDL_Color{ 0, 0, 0, 0 }, t.accent, 0.0f);
                if (pct >= 0) m_gfx->TextCentered(FontSize::Title, tx + tw / 2, ty + 54, on_accent, pcts);
                m_gfx->FillRect(tx, ty + th - 40, tw, 40, SDL_Color{ 0, 0, 0, 150 });
                m_gfx->Text(FontSize::Small, tx + 10, ty + th - 34, kWhite, c_line);
                if (!sub.empty()) m_gfx->TextCentered(FontSize::Small, W / 2, ty + th + 20, t.dim, c_sub);
                break;
            }
            case UiMode::Cover: {
                // The one big cover: the amount on it, the name and a counter under it.
                m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, kDim });
                const int s = 300, x = (W - s) / 2, y = 112;
                card((float)x, (float)y, (float)s, (float)s, 26.0f, 24.0f, Scale(t.accent, 0.5f));
                m_gfx->TextCentered(FontSize::Title, W / 2, y + s / 2 - 34, kWhite, pct >= 0 ? pcts : "...");
                bar(x + 40, y + s - 50, s - 80, 6, SDL_Color{ 255, 255, 255, 60 }, kWhite, 3.0f);
                m_gfx->TextCentered(FontSize::Large, W / 2, y + s + 28, t.accent, c_title);
                m_gfx->TextCentered(FontSize::Small, W / 2, y + s + 78, t.dim, c_line);
                if (!sub.empty()) m_gfx->TextCentered(FontSize::Small, W / 2, y + s + 108, t.dim, c_sub);
                break;
            }
            case UiMode::Shelf: {
                // The box at the front of the shelf filling up, its reflection,
                // and the name in the corner as the shelf shows a game's.
                m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, kDim });
                const int bw = 225, bh = 320, bx = 165, by = 150;
                m_gfx->FillRect(bx, by, bw, bh, Scale(t.accent, 0.35f));
                const float f = frac >= 0 ? frac : 0.5f + 0.5f * std::sin(secs * 3.0f);
                const int fh = (int)(bh * std::clamp(f, 0.0f, 1.0f));
                m_gfx->FillRect(bx, by + bh - fh, bw, fh, t.accent);
                m_gfx->FillRect(bx - 3, by - 3, bw + 6, 3, t.accent);   // the frame
                m_gfx->FillRect(bx - 3, by + bh, bw + 6, 3, t.accent);
                m_gfx->FillRect(bx - 3, by, 3, bh, t.accent);
                m_gfx->FillRect(bx + bw, by, 3, bh, t.accent);
                for (int r = 0; r < 70; r += 2)   // the reflection, fading down
                    m_gfx->FillRect(bx, by + bh + 6 + r, bw, 2,
                                    WithAlpha(r < fh ? t.accent : Scale(t.accent, 0.35f), (Uint8)(70 - r)));
                if (pct >= 0) m_gfx->TextCentered(FontSize::Title, bx + bw / 2, by + bh / 2 - 34, on_accent, pcts);
                m_gfx->Text(FontSize::Large, bx, 560, t.accent, c_title);
                m_gfx->Text(FontSize::Small, bx, 612, t.dim, (line + (sub.empty() ? "" : "   " + sub)).c_str());
                break;
            }
            case UiMode::XMB: {
                // The PS3's update screen: a line of text over a thin bar, the
                // waves still moving behind.
                m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, kDim });
                const int x = 340, w = 600, y = 290;
                m_gfx->Text(FontSize::Small, x, y - 36, SDL_Color{ 200, 200, 200, 255 }, c_title);
                m_gfx->Text(FontSize::Normal, x, y, kWhite, c_line);
                bar(x, y + 52, w, 4, SDL_Color{ 255, 255, 255, 55 }, kWhite, 0.0f);
                if (pct >= 0) m_gfx->Text(FontSize::Small, x + w - m_gfx->TextWidth(FontSize::Small, pcts), y + 66, kWhite, pcts);
                if (!sub.empty()) m_gfx->Text(FontSize::Small, x, y + 66, SDL_Color{ 170, 170, 170, 255 }, c_sub);
                break;
            }
            case UiMode::Flow: {
                // Under the coverflow, a readout like the old iTunes window's:
                // a pale panel with the name, the line and a capsule bar.
                m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, kDim });
                const int pw = 600, ph = 110, px = (W - pw) / 2, py = 290;
                card((float)px, (float)py, (float)pw, (float)ph, 12.0f, 18.0f, SDL_Color{ 228, 231, 222, 250 });
                m_gfx->FillRect(px + 12, py + 1, pw - 24, 1, SDL_Color{ 255, 255, 255, 200 });
                const SDL_Color ink{ 44, 46, 48, 255 };
                m_gfx->TextCentered(FontSize::Normal, W / 2, py + 10, ink, c_title);
                m_gfx->TextCentered(FontSize::Small, W / 2, py + 46, SDL_Color{ 90, 92, 94, 255 }, c_line);
                bar(px + 70, py + 84, pw - 140, 10, SDL_Color{ 186, 190, 182, 255 }, SDL_Color{ 82, 86, 92, 255 }, 5.0f);
                for (int r = 0; r < 40; r += 2)   // its reflection on the floor
                    m_gfx->FillRect(px + 12, py + ph + 4 + r, pw - 24, 2, SDL_Color{ 228, 231, 222, (Uint8)(36 - r * 36 / 40) });
                if (!sub.empty()) m_gfx->TextCentered(FontSize::Small, W / 2, py + ph + 50, t.dim, c_sub);
                break;
            }
            case UiMode::Deck: {
                // A Steam dialog: dark slate, white heading, the blue bar.
                m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, kDim });
                const int pw = 640, ph = 220, px = (W - pw) / 2, py = (H - ph) / 2;
                card((float)px, (float)py, (float)pw, (float)ph, 6.0f, 24.0f, SDL_Color{ 35, 38, 46, 252 });
                m_gfx->Text(FontSize::Normal, px + 32, py + 26, kWhite, c_title);
                m_gfx->Text(FontSize::Small, px + 32, py + 80, SDL_Color{ 178, 184, 194, 255 }, c_line);
                bar(px + 32, py + 130, pw - 64, 8, SDL_Color{ 61, 66, 77, 255 }, SDL_Color{ 26, 159, 255, 255 }, 4.0f);
                if (!sub.empty()) m_gfx->Text(FontSize::Small, px + 32, py + 152, SDL_Color{ 130, 138, 150, 255 }, c_sub);
                if (pct >= 0) m_gfx->Text(FontSize::Small, px + pw - 32 - m_gfx->TextWidth(FontSize::Small, pcts), py + 152,
                                          SDL_Color{ 130, 138, 150, 255 }, pcts);
                break;
            }
            default: {
                // Line, and anything new: the HOME menu's pop-up, a rounded card
                // with the selection's glow around it and a thin rounded bar.
                m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, kDim });
                const int cw = 620, ch = 250, cx = (W - cw) / 2, cy = (H - ch) / 2;
                card((float)cx, (float)cy, (float)cw, (float)ch, 18.0f, 24.0f, Avg(t.bg_top, t.bg_bottom, 250),
                     0.55f + 0.25f * std::sin(secs * 2.6f), t.accent);
                m_gfx->TextCentered(FontSize::Large, W / 2, cy + 30, t.title, c_title);
                m_gfx->TextCentered(FontSize::Small, W / 2, cy + 98, t.fg, c_line);
                bar(cx + 60, cy + 150, cw - 120, 8, WithAlpha(t.fg, 40), t.accent, 4.0f);
                if (!sub.empty()) m_gfx->TextCentered(FontSize::Small, W / 2, cy + 176, t.dim, c_sub);
                break;
            }
        }
        hints();
    }

}
