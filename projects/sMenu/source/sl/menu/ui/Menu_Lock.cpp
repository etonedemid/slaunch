// The lock screen: over every layout on waking from sleep (and at a cold
// boot) when it is on. A press of A opens it; with a passcode set it wants
// four presses instead, the way the Xbox 360's passcodes worked: the D-pad,
// X, Y, L and R. Settings > Lock screen / Lock passcode.
#include <sl/menu/ui/Menu.hpp>
#include <sl/menu/ui/Locale.hpp>
#include <SDL3_image/SDL_image.h>
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "Menu_Internal.hpp"

namespace sl::menu::ui {

    namespace {
        constexpr const char *kLockFile = "sdmc:/slaunch/config/lock.txt";
        constexpr int kCodeLen = 4;

        // ponytail: FNV-1a of a 4-press code, so the card holds no plain code;
        // 8^4 codes is no protection from someone with the card in a PC anyway
        uint64_t CodeHash(const std::string &code) {
            uint64_t h = 1469598103934665603ull;
            for (char c : std::string("slaunch-lock:") + code) { h ^= (uint8_t)c; h *= 1099511628211ull; }
            return h ? h : 1;
        }
        char CodeChar(Btn b) {
            switch (b) {
            case Btn::Up: return 'U'; case Btn::Down: return 'D'; case Btn::Left: return 'L'; case Btn::Right: return 'R';
            case Btn::X: return 'X'; case Btn::Y: return 'Y'; case Btn::L: return 'l'; case Btn::R: return 'r';
            default: return 0;
            }
        }
        float Since(u64 tick) { return (float)(armGetSystemTick() - tick) / (float)armGetSystemTickFreq(); }
    }

    void Menu::LockLoad() {
        m_lock_on = false;
        m_lock_hash = 0;
        if (FILE *f = fopen(kLockFile, "r")) {
            char line[64];
            int v;
            unsigned long long h;
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "on=%d", &v) == 1) m_lock_on = v != 0;
                else if (sscanf(line, "code=%llx", &h) == 1) m_lock_hash = h;
            }
            fclose(f);
        }
        // A cold boot locks too; a menu reload (after the browser, a crash) does not.
        if (m_lock_on && armGetSystemTick() < 90 * armGetSystemTickFreq()) Lock(false);
    }

    void Menu::LockSave() {
        if (FILE *f = fopen(kLockFile, "w")) {
            fprintf(f, "on=%d\ncode=%llx\n", m_lock_on ? 1 : 0, (unsigned long long)m_lock_hash);
            fclose(f);
        }
    }

    bool Menu::Lock(bool resume) {
        if (!m_lock_on) return false;
        m_locked = true;
        m_lock_resume = resume;
        m_lock_mode = 0;
        m_lock_in.clear();
        m_lock_since = armGetSystemTick();
        return true;
    }

    void Menu::LockSetPasscode() {
        m_lock_mode = 1;
        m_lock_in.clear();
        m_lock_first.clear();
        m_lock_since = armGetSystemTick();
    }

    Menu::Action Menu::OnButtonLock(Btn b) {
        if (m_lock_mode != 0) {   // choosing a passcode
            if (b == Btn::B) { m_lock_mode = 0; m_lock_in.clear(); return Action::None; }
            if (b == Btn::Minus && m_lock_hash) {   // remove it
                m_lock_hash = 0;
                m_lock_mode = 0;
                LockSave();
                SetStatus(T("Passcode removed"));
                return Action::None;
            }
            const char c = CodeChar(b);
            if (!c) return Action::None;
            m_lock_in += c;
            if ((int)m_lock_in.size() < kCodeLen) return Action::None;
            if (m_lock_mode == 1) {
                m_lock_first = m_lock_in;
                m_lock_in.clear();
                m_lock_mode = 2;
            } else if (m_lock_in == m_lock_first) {
                m_lock_hash = CodeHash(m_lock_in);
                m_lock_on = true;
                m_lock_mode = 0;
                m_lock_in.clear();
                LockSave();
                SetStatus(T("Passcode set"));
            } else {   // did not match: start over
                m_lock_in.clear();
                m_lock_first.clear();
                m_lock_mode = 1;
                m_lock_shake = armGetSystemTick();
            }
            return Action::None;
        }
        if (!m_locked) return Action::None;
        bool open = false;
        if (!m_lock_hash) {
            open = b == Btn::A;
        } else if (b == Btn::B) {
            m_lock_in.clear();
        } else if (const char c = CodeChar(b)) {
            m_lock_in += c;
            if ((int)m_lock_in.size() >= kCodeLen) {
                open = CodeHash(m_lock_in) == m_lock_hash;
                if (!open) m_lock_shake = armGetSystemTick();
                m_lock_in.clear();
            }
        }
        if (!open) return Action::None;
        m_locked = false;
        if (m_lock_resume && m_suspended != 0) return Action::ResumeApp;
        return Action::None;
    }

    // Each layout locks in its own look: its own backdrop (never the menu on
    // it, which stays private) under a veil, and the clock, type, colours and
    // passcode marks of the console it is after.
    void Menu::DrawLock() {
        if (!Locked()) return;
        const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
        const float in = std::min(1.0f, Since(m_lock_since) / 0.25f);
        const Theme &t = m_theme.Current();

        enum Align { Center, LeftLow, TopRight };
        enum Mark { Square, Dot, Bar };
        struct Look {
            bool metro;                  // Selawik / the skin's font, else the console's
            SDL_Color veil, fg, dim, accent;
            Align align;
            Mark mark;
        } L{ false, { 0, 0, 0, 110 }, t.fg, t.dim, t.accent, Center, Dot };
        switch (m_ui_mode) {
        case UiMode::Metro:  L = { true, { 0, 0, 0, 120 }, { 255, 255, 255, 255 }, { 205, 208, 212, 255 }, { 14, 150, 14, 255 }, LeftLow, Square }; break;
        case UiMode::Blades: L = { true, { 0, 0, 0, 150 }, { 255, 255, 255, 255 }, { 196, 214, 176, 255 }, { 104, 178, 32, 255 }, Center, Square }; break;
        case UiMode::NXE:    if (m_nxe_themed) break;
                             L = { true, { 250, 250, 252, 150 }, { 40, 40, 42, 255 }, { 92, 96, 104, 255 }, { 104, 178, 32, 255 }, Center, Square }; break;
        case UiMode::XMB:    L = { false, { 0, 0, 0, 70 }, { 255, 255, 255, 255 }, { 222, 226, 236, 255 }, { 255, 255, 255, 255 }, TopRight, Dot }; break;
        case UiMode::Deck:   L = { false, { 14, 20, 28, 190 }, { 255, 255, 255, 255 }, { 160, 170, 184, 255 }, { 26, 159, 255, 255 }, Center, Bar }; break;
        case UiMode::Shelf:
            if (m_shelf_ps) L = { false, { 0, 24, 70, 120 }, { 255, 255, 255, 255 }, { 200, 214, 236, 255 }, { 0, 112, 209, 255 }, Center, Dot };
            break;
        case UiMode::List: case UiMode::Line: L.align = LeftLow; break;
        default: break;
        }

        // The layout's own backdrop, then the veil, fading in together over
        // whatever the menu drew this frame.
        if (in < 1.0f) m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, (Uint8)(255 * in) });
        if (in >= 1.0f) {
            if (BladesUi()) DrawBladesFrame();
            else            DrawBackground();
        }
        const Uint8 A = (Uint8)(255 * in);
        auto fade = [&](SDL_Color c) { c.a = (Uint8)(c.a * A / 255); return c; };
        m_gfx->FillRect(0, 0, W, H, fade(L.veil));

        auto width = [&](int pt, const char *s) {
            if (L.metro) return MWidth(0, pt, s);
            if (TTF_Font *f = m_gfx->Face("@sys", pt)) return m_gfx->TextWidth(f, s);
            return m_gfx->TextWidth(FontSize::Title, s);
        };
        auto text = [&](int pt, int x, int y, SDL_Color c, const char *s) {
            c = fade(c);
            if (L.metro) { MText(0, pt, x, y, c, s); return; }
            if (TTF_Font *f = m_gfx->Face("@sys", pt)) m_gfx->Text(f, x, y, c, s);
            else m_gfx->Text(FontSize::Title, x, y, c, s);
        };
        auto center = [&](int pt, int y, SDL_Color c, const char *s) { text(pt, (W - width(pt, s)) / 2, y, c, s); };

        const time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char clock[16], date[64], batt[24];
        strftime(clock, sizeof(clock), "%H:%M", &tm_now);
        strftime(date, sizeof(date), "%A, %d %B", &tm_now);
        snprintf(batt, sizeof(batt), sys::Charging() ? "+%lu%%" : "%lu%%", (unsigned long)sys::BatteryPercent());

        int markY;   // the passcode marks' row
        if (m_lock_mode != 0) {
            center(48, H / 2 - 170, L.fg, m_lock_mode == 1 ? T("Choose a passcode") : T("Enter it again"));
            center(26, H / 2 - 100, L.dim, T("Four presses: the D-pad, X, Y, L or R"));
            markY = H / 2 - 10;
        } else if (L.align == LeftLow) {   // phone-style: big time low on the left
            text(170, 80, H - 400, L.fg, clock);
            text(40, 88, H - 190, L.dim, date);
            text(28, W - 60 - width(28, batt), 40, L.dim, batt);
            markY = H - 110;
        } else if (L.align == TopRight) {  // the XMB's clock corner, grown
            text(30, W - 70 - width(30, date), 60, L.dim, date);
            text(120, W - 70 - width(120, clock), 96, L.fg, clock);
            text(26, 70, 60, L.dim, batt);
            markY = H / 2 + 120;
        } else {
            center(200, H / 2 - 230, L.fg, clock);
            center(40, H / 2 + 10, L.dim, date);
            text(28, W - 60 - width(28, batt), 40, L.dim, batt);
            markY = H / 2 + 110;
        }

        // The passcode's four marks, shaken when a code was wrong.
        const bool marks = m_lock_mode != 0 || m_lock_hash != 0;
        if (marks) {
            const float sh = Since(m_lock_shake);
            const int dx = m_lock_shake && sh < 0.4f ? (int)(18 * std::sin(sh * 60.0f) * (1 - sh / 0.4f)) : 0;
            const int d = L.mark == Bar ? 56 : 30, gap = L.mark == Bar ? 20 : 34;
            int x0 = (L.align == LeftLow && m_lock_mode == 0 ? 88 : (W - (kCodeLen * d + (kCodeLen - 1) * gap)) / 2) + dx;
            for (int i = 0; i < kCodeLen; i++) {
                const bool on = i < (int)m_lock_in.size();
                const int x = x0 + i * (d + gap);
                SDL_Color c = on ? L.accent : L.dim;
                if (!on) c.a = 70;
                c = fade(c);
                if (L.mark == Bar) {
                    m_gfx->FillRect(x, markY + d / 2 - 3, d, 6, c);
                } else if (L.mark == Square) {
                    m_gfx->FillRect(x, markY, d, d, c);
                } else {   // a dot: rows of a circle
                    const float r = d * 0.5f;
                    for (int yy = 0; yy < d; yy++) {
                        const float fy = yy + 0.5f - r, hw = std::sqrt(std::max(0.0f, r * r - fy * fy));
                        m_gfx->FillRect(x + (int)(r - hw), markY + yy, std::max(1, (int)(2 * hw)), 1, c);
                    }
                }
            }
        }
        const char *hint = m_lock_mode != 0 ? (m_lock_hash ? T("B  Cancel      -  Remove passcode") : T("B  Cancel"))
                         : m_lock_hash ? T("Enter your passcode") : T("Press A to unlock");
        const int hy = marks ? markY + 70 : (L.align == LeftLow ? H - 110 : H / 2 + 110);
        if (L.align == LeftLow && m_lock_mode == 0) text(30, 88, std::min(hy, H - 60), L.dim, hint);
        else center(30, hy, L.dim, hint);
    }
}
