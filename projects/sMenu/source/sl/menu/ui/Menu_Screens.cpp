#include <sl/menu/ui/Menu.hpp>
#include <unordered_set>
#include <sl/menu/ui/Locale.hpp>
#include <sl/menu/net/Http.hpp>
#include <sl/menu/net/ContentFilter.hpp>
#include <sl/smi/Protocol.hpp>
#include <SDL2/SDL_image.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include "Menu_Internal.hpp"

namespace sl::menu::ui {

    Menu::Action Menu::OnButtonKeyboard(Btn b) {
        auto commit = [&]() {
            switch (m_kb_purpose) {
                case sl::smi::Kb_FileRename: FmKeyboardDone(true,  m_kb_text); break;
                case sl::smi::Kb_NewFolder:  FmKeyboardDone(false, m_kb_text); break;
                case sl::smi::Kb_WidgetOption:
                    if (widgets::IWidget *w = m_widgets.At((int)m_kb_app))
                        w->SetOption(m_kb_opt, m_kb_text);
                    SetStatus("Saved");
                    m_screen = Screen::WidgetOptions;
                    break;
                case sl::smi::Kb_ThemeName:
                    if (m_theme.IsCustom((int)m_kb_app) && !m_kb_text.empty()) {
                        Theme &c = m_theme.CustomAt((int)m_kb_app);
                        strncpy(c.name, m_kb_text.c_str(), sizeof(c.name) - 1);
                        c.name[sizeof(c.name) - 1] = '\0';
                        m_theme.Save();
                    }
                    SetStatus("Theme renamed");
                    m_screen = Screen::ThemeEditor;
                    break;
                // This fell through to the rename case, so typing the API key
                // into Theming renamed whatever game the cursor happened to be
                // on and never saved the key at all.
                case sl::smi::Kb_SteamGridKey: {
                    constexpr const char *path = "sdmc:/slaunch/config/steamgriddb.txt";
                    mkdir("sdmc:/slaunch", 0777);
                    mkdir("sdmc:/slaunch/config", 0777);
                    if (m_kb_text.empty()) {
                        remove(path);
                    } else if (FILE *fp = fopen(path, "w")) {
                        fprintf(fp, "%s\n", m_kb_text.c_str());
                        fclose(fp);
                    }
                    m_sgdb_key = m_kb_text;
                    m_sgdb_key_loaded = true;
                    SetStatus(m_kb_text.empty() ? T("Key cleared") : T("Key saved"));
                    m_screen = Screen::Theming;
                    break;
                }
                case sl::smi::Kb_Search: {
                    m_search = m_kb_text;
                    RebuildItems();
                    // Land on the first match rather than wherever the cursor
                    // happened to be in the unfiltered list.
                    m_cursor = 0;
                    m_scroll_pos = 0.0f;
                    m_xmb_col = -1; m_xmb_item = 0; m_xmb_placed = false;
                    XmbRebuild();
                    if (m_search.empty())        SetStatus(T("Search cleared"));
                    else if (m_items.empty())    SetStatus(T("No matches"));
                    m_screen = Screen::Main;
                    break;
                }
                default: // Kb_RenameGame
                    SetCustomName(m_kb_app, m_kb_text.c_str());
                    RebuildItems();
                    SelectApp(m_kb_app);
                    SetStatus("Renamed");
                    m_screen = Screen::Main;
                    break;
            }
        };
        auto backspace = [&]() {
            if (!m_kb_text.empty()) m_kb_text.pop_back();
        };

        if (b == Btn::Up)   { m_kb_row = (m_kb_row + 4) % 5; m_kb_col = std::min(m_kb_col, KbRowLen(m_kb_row) - 1); }
        if (b == Btn::Down) { m_kb_row = (m_kb_row + 1) % 5; m_kb_col = std::min(m_kb_col, KbRowLen(m_kb_row) - 1); }
        if (b == Btn::Left)  { int n = KbRowLen(m_kb_row); m_kb_col = (m_kb_col + n - 1) % n; }
        if (b == Btn::Right) { int n = KbRowLen(m_kb_row); m_kb_col = (m_kb_col + 1) % n; }

        if (b == Btn::A) {
            if (m_kb_row < 4) {
                char c = kKbRows[m_kb_row][m_kb_col];
                if (m_kb_upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                if (m_kb_text.size() < 60) m_kb_text.push_back(c);
            } else {
                switch (m_kb_col) {
                    case 0: m_kb_upper = !m_kb_upper; break;         // Shift
                    case 1: if (m_kb_text.size() < 60) m_kb_text.push_back(' '); break; // Space
                    case 2: backspace(); break;                     // Back
                    case 3: m_kb_text.clear(); break;               // Clear
                    case 4: commit(); break;                        // Done
                }
            }
        }
        if (b == Btn::Y)    backspace();          // quick backspace
        if (b == Btn::X)    m_kb_upper = !m_kb_upper;
        if (b == Btn::Plus) commit();
        if (b == Btn::B) {
            m_screen = (m_kb_purpose == sl::smi::Kb_FileRename ||
                        m_kb_purpose == sl::smi::Kb_NewFolder)
                       ? Screen::Files :
                       (m_kb_purpose == sl::smi::Kb_WidgetOption)
                       ? Screen::WidgetOptions :
                       (m_kb_purpose == sl::smi::Kb_ThemeName)
                       ? Screen::ThemeEditor : Screen::Main;
        }
        return Action::None;
    }
    // Setup: Welcome, Layout, Theme, Good to know, Done.
    Menu::Action Menu::OnButtonOobe(Btn b) {
        constexpr int LastStep = kOobeSteps - 1;
        const bool prev = (b == Btn::Left || b == Btn::Up);
        const bool next = (b == Btn::Right || b == Btn::Down);
        if (m_oobe_step == 1 && (prev || next)) {          // layout gallery
            const int n = (int)UiMode::Count;
            m_ui_mode = (UiMode)(((int)m_ui_mode + (next ? 1 : n - 1)) % n);
            RebuildItems();   // the list contents depend on the mode
        }
        if (m_oobe_step == 2 && (prev || next)) {          // theme, applied live
            const int n = m_theme.Count();
            m_theme_cursor = (m_theme_cursor + (next ? 1 : n - 1)) % n;
            m_theme.Select(m_theme_cursor);
        }
        if (m_oobe_step == LastStep && (b == Btn::Left || b == Btn::Right))
            m_check_updates = !m_check_updates;

        if (b == Btn::A) {
            if (m_oobe_step < LastStep) {
                m_oobe_step++;
                if (m_oobe_step == 1) m_oobe_gallery = (float)(int)m_ui_mode;
                if (m_oobe_step == 2) m_oobe_list = (float)m_theme_cursor;
            } else {
                m_theme.Save();
                SaveSettings();   // persist the chosen layout (ui_mode)
                FreeOobePreviews();
                // Hand off to the welcome screen (opening jingle + the user's name)
                // instead of dropping straight into the menu.
                if (m_welcome_enabled) { EnterWelcome(); m_sfx.Play(audio::Sfx::Startup); }
                else                     m_screen = Screen::Main;
                return Action::FinishSetup;
            }
        }
        if (b == Btn::B && m_oobe_step > 0) m_oobe_step--;
        return Action::None;
    }
    bool Menu::IsSysHidden(ItemKind k) const {
        if (k == ItemKind::Theming) return false;      // never hideable
        return (m_sys_hidden & (1u << (u32)k)) != 0;
    }
    void Menu::ToggleSysHidden(ItemKind k) {
        if (k == ItemKind::Theming) return;
        m_sys_hidden ^= (1u << (u32)k);
        SaveSysEntries();
        RebuildItems();
    }
    void Menu::LoadSysEntries() {
        m_sys_hidden = 0;
        FILE *fp = fopen(GetUserConfigPath("sysentries.txt").c_str(), "r");
        if (!fp) return;
        unsigned v = 0;
        if (fscanf(fp, "%u", &v) == 1) m_sys_hidden = (u32)v;
        fclose(fp);
    }
    void Menu::SaveSysEntries() {
        EnsureUserConfigDir();
        const std::string path = GetUserConfigPath("sysentries.txt");
        FILE *fp = fopen(path.c_str(), "w");
        if (!fp) return;
        fprintf(fp, "%u\n", (unsigned)m_sys_hidden);
        fclose(fp);
    }
    Menu::Action Menu::OnButtonSysEntries(Btn b) {
        if (b == Btn::B) { m_screen = Screen::Theming; m_sub_scroll = 0; return Action::None; }
        if (b == Btn::Down) m_sys_cursor = (m_sys_cursor + 1) % kSysEntryN;
        if (b == Btn::Up)   m_sys_cursor = (m_sys_cursor + kSysEntryN - 1) % kSysEntryN;
        if (b == Btn::A || b == Btn::Left || b == Btn::Right)
            ToggleSysHidden(kSysEntries[m_sys_cursor].kind);
        return Action::None;
    }
    void Menu::DrawSysEntries() {
        DrawTopBar(T("Menu entries"));
        std::vector<std::string> labels, values;
        for (int i = 0; i < kSysEntryN; i++) {
            labels.push_back(T(kSysEntries[i].name));
            values.push_back(IsSysHidden(kSysEntries[i].kind) ? T("Hidden") : T("Shown"));
        }
        DrawCarousel(labels, values, m_sys_cursor, m_sub_scroll);
        DrawHint({ {{"up","down"}, "Select"}, {{"b"}, "Back"} });
    }
    void Menu::CoverPickTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        net::GlobalInit();

        // Second phase: the chosen grid is downloaded over the real cover.
        if (m->m_pick_apply) {
            m->m_pick_state.store((int)PickState::Applying, std::memory_order_release);
            bool ok = false;
            const int idx = m->m_pick_choice;
            if (idx >= 0 && idx < (int)m->m_pick_urls.size()) {
                mkdir("sdmc:/slaunch/covers", 0777);
                char dst[96];
                snprintf(dst, sizeof(dst), "sdmc:/slaunch/covers/%016llX.jpg",
                         (unsigned long long)m->m_pick_id);
                ok = net::Download(m->m_pick_urls[idx].c_str(), dst, 25);
            }
            m->m_pick_state.store((int)(ok ? PickState::Applied : PickState::Failed),
                                  std::memory_order_release);
            m->m_pick_done.store(true, std::memory_order_release);
            return;
        }

        const std::string auth = "Bearer " + m->m_sgdb_key;
        PickState end = PickState::Failed;

        do {
            std::string q;
            for (unsigned char c : m->m_pick_name) {
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') q += (char)c;
                else if (c == ' ') q += "%20";
                else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); q += b; }
            }
            if (q.empty()) break;

            // Check if this game should be filtered due to adult content
            if (net::ContentFilter::ShouldFilterGameByName(m->m_pick_name)) {
                end = PickState::Failed;  // Treat filtered as failed lookup
                break;
            }

            m->m_pick_state.store((int)PickState::Searching, std::memory_order_release);
            std::string body;
            long http = 0; int rc = 0;
            const std::string url =
                "https://www.steamgriddb.com/api/v2/search/autocomplete/" + q;
            if (!net::Get(url.c_str(), body, 12, auth.c_str(), &http, &rc)) {
                end = (http == 401 || http == 403) ? PickState::BadKey : PickState::Failed;
                break;
            }

            end = PickState::NoMatch;
            const size_t d = body.find("\"data\"");
            if (d == std::string::npos) break;
            const size_t idp = body.find("\"id\"", d);
            if (idp == std::string::npos) break;
            const size_t colon = body.find(':', idp);
            if (colon == std::string::npos) break;
            const long long gid = strtoll(body.c_str() + colon + 1, nullptr, 10);
            if (gid <= 0) break;

            m->m_pick_state.store((int)PickState::Listing, std::memory_order_release);
            char grid[176];
            snprintf(grid, sizeof(grid),
                     "https://www.steamgriddb.com/api/v2/grids/game/%lld"
                     "?dimensions=600x900&types=static&limit=%d", gid, kPickMax);
            body.clear();
            end = PickState::NoArt;
            if (!net::Get(grid, body, 15, auth.c_str(), &http, &rc)) break;

            // "url" is the full-size grid, "thumb" the preview beside it; one of
            // each per entry, in the same order, so the two lists stay aligned.
            m->m_pick_urls   = JsonStrAll(body, "url",   kPickMax);
            m->m_pick_thumbs = JsonStrAll(body, "thumb", kPickMax);
            if (m->m_pick_urls.empty()) break;
            if (m->m_pick_thumbs.size() < m->m_pick_urls.size())
                m->m_pick_urls.resize(m->m_pick_thumbs.size());
            if (m->m_pick_urls.empty()) break;

            // Both vectors are complete from here on and never resized again,
            // which is what makes them safe to read on the main thread behind
            // the m_pick_have counter.
            m->m_pick_total.store((int)m->m_pick_urls.size(), std::memory_order_release);

            mkdir("sdmc:/slaunch/cache", 0777);
            mkdir(kPickDir, 0777);
            for (size_t i = 0; i < m->m_pick_urls.size(); i++) {
                // A choice has been made; whatever is already on screen is
                // enough and the rest of the downloads are wasted work.
                if (m->m_pick_abort.load(std::memory_order_acquire)) break;
                char dst[96];
                snprintf(dst, sizeof(dst), "%s/%02u.jpg", kPickDir, (unsigned)i);
                remove(dst);
                if (!net::Download(m->m_pick_thumbs[i].c_str(), dst, 15)) continue;
                // Released one at a time so the grid fills in as they land
                // rather than sitting empty until the last one arrives.
                m->m_pick_have.store((int)i + 1, std::memory_order_release);
            }
            end = (m->m_pick_have.load(std::memory_order_acquire) > 0)
                      ? PickState::Ready : PickState::Failed;
        } while (false);

        m->m_pick_state.store((int)end, std::memory_order_release);
        m->m_pick_done.store(true, std::memory_order_release);
    }
    void Menu::EnterCoverPicker() {
        if (m_pick_running) return;                 // a fetch is already in flight
        const MenuItem &it = m_items[m_cursor];
        if (it.kind != ItemKind::Game || it.app_id == 0) return;

        LeaveCoverPicker();                          // free any previous session
        m_pick_id     = it.app_id;
        m_pick_name   = it.name;
        m_pick_cursor = 0;
        m_pick_scroll = 0.0f;
        m_pick_apply  = false;
        m_pick_want_apply = false;
        m_pick_abort.store(false, std::memory_order_release);
        m_pick_have.store(0, std::memory_order_release);
        m_pick_total.store(0, std::memory_order_release);
        m_pick_done.store(false, std::memory_order_release);
        m_screen = Screen::CoverPicker;

        if (!SgdbKeyPresent()) {
            m_pick_state.store((int)PickState::NoKey, std::memory_order_release);
            return;
        }
        m_pick_state.store((int)PickState::Searching, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_pick_thread, &Menu::CoverPickTrampoline,
                                     this, nullptr, 0x20000, 0x3B, -2))) {
            threadStart(&m_pick_thread);
            m_pick_running = true;
        } else {
            m_pick_state.store((int)PickState::Failed, std::memory_order_release);
        }
    }
    // Frees the textures and forgets the listing. The worker is joined first:
    // it writes the url vectors, so tearing them down under it would be a race.
    void Menu::LeaveCoverPicker() {
        // Cancelling should be immediate too: without this, backing out waited
        // for every remaining thumbnail before the screen would close.
        m_pick_abort.store(true, std::memory_order_release);
        if (m_pick_running) {
            threadWaitForExit(&m_pick_thread);
            threadClose(&m_pick_thread);
            m_pick_running = false;
        }
        for (SDL_Texture *t : m_pick_tex) if (t) m_gfx->FreeImage(t);
        m_pick_tex.clear();
        m_pick_urls.clear();
        m_pick_thumbs.clear();
        m_pick_have.store(0, std::memory_order_release);
        m_pick_total.store(0, std::memory_order_release);
        m_pick_abort.store(false, std::memory_order_release);
        m_pick_want_apply = false;
        m_pick_state.store((int)PickState::Idle, std::memory_order_release);
    }
    void Menu::StartPickApply() {
        m_pick_apply = true;
        m_pick_done.store(false, std::memory_order_release);
        m_pick_state.store((int)PickState::Applying, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_pick_thread, &Menu::CoverPickTrampoline,
                                     this, nullptr, 0x20000, 0x3B, -2))) {
            threadStart(&m_pick_thread);
            m_pick_running = true;
        } else {
            m_pick_apply = false;
            m_pick_state.store((int)PickState::Failed, std::memory_order_release);
            SetStatus(T("Could not download that cover"));
        }
    }
    void Menu::PollCoverPicker() {
        if (!m_pick_running || !m_pick_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_pick_thread);
        threadClose(&m_pick_thread);
        m_pick_running = false;

        // A was pressed while the listing was still running.
        if (m_pick_want_apply && !m_pick_apply) {
            m_pick_want_apply = false;
            m_pick_abort.store(false, std::memory_order_release);
            StartPickApply();
            return;
        }
        if (!m_pick_apply) return;
        m_pick_apply = false;

        const PickState st = (PickState)m_pick_state.load(std::memory_order_acquire);
        if (st != PickState::Applied) { SetStatus(T("Could not download that cover")); return; }

        // Drop what Flow and the shelf have cached for this title so the new
        // file is picked up on the next frame, exactly as a fetch does.
        m_art_epoch++;
        auto f = m_covers.find(m_pick_id);
        if (f != m_covers.end()) {
            if (f->second) m_gfx->FreeImage(f->second);
            m_covers.erase(f);
        }
        auto g = m_shots.find(m_pick_id);
        if (g != m_shots.end()) {
            if (g->second.a) m_gfx->FreeImage(g->second.a);
            if (g->second.b) m_gfx->FreeImage(g->second.b);
            m_shots.erase(g);
        }
        m_cover_tried[m_pick_id] = true;   // do not let the auto-fetch undo this

        SetStatus(T("Cover updated"));
        m_screen = BackTarget();
        LeaveCoverPicker();
    }
    Menu::Action Menu::OnButtonCoverPicker(Btn b) {
        if (b == Btn::B) {
            m_screen = BackTarget();
            LeaveCoverPicker();
            return Action::None;
        }

        const int have = m_pick_have.load(std::memory_order_acquire);
        if (have <= 0) return Action::None;          // nothing to move around yet

        const int rows = (have + kPickCols - 1) / kPickCols;
        if (b == Btn::Left)  m_pick_cursor = (m_pick_cursor + have - 1) % have;
        if (b == Btn::Right) m_pick_cursor = (m_pick_cursor + 1) % have;
        if (b == Btn::Up) {
            if (m_pick_cursor >= kPickCols) m_pick_cursor -= kPickCols;
        }
        if (b == Btn::Down) {
            if (m_pick_cursor + kPickCols < have) m_pick_cursor += kPickCols;
        }

        // Keep the cursor's row on screen.
        const int row = m_pick_cursor / kPickCols;
        const float first = m_pick_scroll;
        if ((float)row < first)                    m_pick_scroll = (float)row;
        else if (row > (int)first + kPickRows - 1) m_pick_scroll = (float)(row - kPickRows + 1);
        if (m_pick_scroll > (float)(rows - kPickRows))
            m_pick_scroll = (float)((rows > kPickRows) ? rows - kPickRows : 0);
        if (m_pick_scroll < 0.0f) m_pick_scroll = 0.0f;

        if (b == Btn::A) {
            m_pick_choice = m_pick_cursor;
            m_sfx_confirm = true;
            if (m_pick_running) {
                // Still listing: cut the remaining thumbnail downloads short and
                // let PollCoverPicker start the real download once it has joined.
                m_pick_want_apply = true;
                m_pick_abort.store(true, std::memory_order_release);
                m_pick_state.store((int)PickState::Applying, std::memory_order_release);
            } else {
                StartPickApply();
            }
        }
        return Action::None;
    }
    void Menu::DrawCoverPicker() {
        const Theme &t = m_theme.Current();
        const int    W = gfx::Gfx::Width;
        DrawTopBar(T("Choose cover"));

        const PickState st = (PickState)m_pick_state.load(std::memory_order_acquire);
        const int have  = m_pick_have.load(std::memory_order_acquire);
        const int total = m_pick_total.load(std::memory_order_acquire);

        m_gfx->TextCentered(FontSize::Small, W / 2, 96, t.dim,
                            Ellipsize(m_pick_name, W - 120, FontSize::Small).c_str());

        const char *msg = nullptr;
        switch (st) {
            case PickState::NoKey:    msg = T("No SteamGridDB key set"); break;
            case PickState::BadKey:   msg = T("SteamGridDB rejected the key"); break;
            case PickState::Searching:msg = T("Searching..."); break;
            case PickState::Listing:  msg = T("Loading covers..."); break;
            case PickState::NoMatch:  msg = T("No match on SteamGridDB"); break;
            case PickState::NoArt:    msg = T("No covers available"); break;
            case PickState::Failed:   msg = T("Fetch failed"); break;
            case PickState::Applying: msg = T("Downloading..."); break;
            default: break;
        }
        if (have <= 0 && msg) {
            m_gfx->TextCentered(FontSize::Normal, W / 2, 330, t.fg, msg);
            DrawHint({ {{"b"}, "Back"} });
            return;
        }

        // Thumbnails are decoded a couple per frame, like every other image in
        // the menu, so a screen filling up never costs a frame.
        int budget = 2;
        if ((int)m_pick_tex.size() < have) m_pick_tex.resize(have, nullptr);

        const int top = kPickTop - (int)(m_pick_scroll * (kPickCellH + kPickGapY));

        for (int i = 0; i < have; i++) {
            const int cx = PickCellX(i % kPickCols);
            const int cy = top + (i / kPickCols) * (kPickCellH + kPickGapY);
            if (cy + kPickCellH < 110 || cy > gfx::Gfx::Height - 40) continue;

            if (!m_pick_tex[i] && budget > 0) {
                char path[96];
                snprintf(path, sizeof(path), "%s/%02d.jpg", kPickDir, i);
                m_pick_tex[i] = m_gfx->LoadImageScaled(path, kPickCellW, kPickCellH);
                budget--;
            }

            const bool sel = (i == m_pick_cursor);
            if (m_pick_tex[i]) m_gfx->DrawImage(m_pick_tex[i], cx, cy, kPickCellW, kPickCellH);
            else               m_gfx->FillRect(cx, cy, kPickCellW, kPickCellH,
                                               WithAlpha(t.fg, 26));
            if (sel) {
                const SDL_Color a = t.accent;
                m_gfx->FillRect(cx - 4, cy - 4, kPickCellW + 8, 4, a);
                m_gfx->FillRect(cx - 4, cy + kPickCellH, kPickCellW + 8, 4, a);
                m_gfx->FillRect(cx - 4, cy - 4, 4, kPickCellH + 8, a);
                m_gfx->FillRect(cx + kPickCellW, cy - 4, 4, kPickCellH + 8, a);
            }
        }

        char count[48];
        snprintf(count, sizeof(count), "%d / %d", m_pick_cursor + 1,
                 total > 0 ? total : have);
        m_gfx->TextCentered(FontSize::Small, W / 2, gfx::Gfx::Height - 64, t.dim, count);

        if (st == PickState::Applying) DrawHint("Downloading...");
        else                           DrawHint({ {{"dpad"}, "Choose"}, {{"a"}, "Use this"}, {{"b"}, "Cancel"} });
    }
    // ---- Network screen -----------------------------------------------------
    // The status the console already knows, plus the one setting that can be
    // changed without leaving the menu. Full configuration - scanning for a
    // network, entering a passphrase - stays with the system NetConnect applet,
    // which the last row hands off to. That applet owns the keyboard and the
    // scan UI, and there is no reason to reimplement either.
    // nifm answers over IPC, so this is polled on a timer rather than per frame.
    // Four service calls every frame would show up in the frame time, for numbers
    // that change every few seconds at most.
    void Menu::RefreshNetwork(bool force) {
        const u64 now = armGetSystemTick();
        if (!force && m_net_tick && (now - m_net_tick) < armGetSystemTickFreq())
            return;
        m_net_tick = now;

        NetStatus s;

        bool wl = true;
        if (R_SUCCEEDED(nifmIsWirelessCommunicationEnabled(&wl))) s.wifi_on = wl;

        NifmInternetConnectionType   type = (NifmInternetConnectionType)0;
        NifmInternetConnectionStatus st   = (NifmInternetConnectionStatus)0;
        u32 strength = 0;
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &st))) {
            s.connected = (st == NifmInternetConnectionStatus_Connected);
            s.ethernet  = (type == NifmInternetConnectionType_Ethernet);
            s.strength  = (int)strength;
        }

        u32 ip = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip != 0) {
            // nifm returns the address low byte first.
            char buf[24];
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                     (unsigned)( ip        & 0xFF), (unsigned)((ip >>  8) & 0xFF),
                     (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
            s.ip = buf;
        }

        NifmNetworkProfileData prof = {};
        if (R_SUCCEEDED(nifmGetCurrentNetworkProfile(&prof))) {
            // The SSID when there is one, otherwise the profile name: a wired
            // profile has no SSID at all, and an empty row for it reads as a bug
            // rather than as "not applicable".
            char nm[0x41] = {};
            memcpy(nm, prof.wireless_setting_data.ssid,
                   sizeof(prof.wireless_setting_data.ssid) - 1);
            if (nm[0] == 0)
                memcpy(nm, prof.network_name, sizeof(prof.network_name));
            nm[sizeof(nm) - 1] = 0;
            s.name = nm;
        }

        m_net = s;
    }
    void Menu::DrawNetwork() {
        RefreshNetwork(false);
        DrawTopBar(T("Network"));

        const std::string dash = "-";
        std::vector<std::string> labels, values;

        labels.push_back(T("Status"));
        values.push_back(!m_net.connected ? T("Not connected")
                         : m_net.ethernet ? T("Connected (wired)")
                                          : T("Connected"));

        labels.push_back(T("Network name"));
        values.push_back(m_net.name.empty() ? dash : m_net.name);

        labels.push_back(T("Signal"));
        if (!m_net.connected || m_net.ethernet) {
            values.push_back(dash);
        } else {
            char b[16];
            snprintf(b, sizeof(b), "%d/3", m_net.strength);
            values.push_back(b);
        }

        labels.push_back(T("IP address"));
        values.push_back(m_net.ip.empty() ? dash : m_net.ip);

        labels.push_back(T("Wi-Fi"));
        values.push_back(m_net.wifi_on ? T("On") : T("Off"));

        labels.push_back(T("Open network settings"));
        values.emplace_back();

        DrawCarousel(labels, values, m_net_cursor, m_sub_scroll);
        DrawHint({ {{"up","down"}, "Select"}, {{"a"}, "Change"}, {{"b"}, "Back"} });
    }
    Menu::Action Menu::OnButtonNetwork(Btn b) {
        if (b == Btn::B) { m_screen = BackTarget(); m_sub_scroll = 0; return Action::None; }
        if (b == Btn::Down) m_net_cursor = (m_net_cursor + 1) % NET_Count;
        if (b == Btn::Up)   m_net_cursor = (m_net_cursor + NET_Count - 1) % NET_Count;

        const bool a = (b == Btn::A);
        if (!a && b != Btn::Left && b != Btn::Right) return Action::None;

        if (m_net_cursor == NET_Wifi) {
            const bool want = !m_net.wifi_on;
            if (R_SUCCEEDED(nifmSetWirelessCommunicationEnabled(want))) {
                m_net.wifi_on = want;
                SetStatus(want ? T("Wi-Fi on") : T("Wi-Fi off"));
                // The radio takes a moment to associate or drop. Clearing the
                // timer re-reads on the next frame instead of leaving a stale
                // status sitting there for the rest of the poll interval.
                m_net_tick = 0;
                m_sfx_confirm = true;
            } else {
                SetStatus(T("Could not change Wi-Fi"));
            }
            return Action::None;
        }
        if (m_net_cursor == NET_Open && a) {
            m_sfx_confirm = true;
            return Action::OpenNetConnect;
        }
        // A on a read-only row re-reads instead of doing nothing, so there is an
        // obvious way to refresh the screen by hand.
        if (a) RefreshNetwork(true);
        return Action::None;
    }
    void Menu::ScanPayloads() {
        if (m_payloads_scanned) return;   // cheap, but the dirs cannot change under us
        m_payloads_scanned = true;
        m_payloads.clear();
        m_payload_names.clear();

        std::vector<std::pair<std::string, std::string>> found;   // name, path
        auto add = [&](std::string path, std::string name) {
            for (auto &f : found) if (f.second == path) return;   // listed twice
            found.emplace_back(std::move(name), std::move(path));
        };

        for (const char *dir : kPayloadDirs) {
            DIR *d = opendir(dir);
            if (!d) continue;
            while (dirent *e = readdir(d)) {
                const size_t len = strlen(e->d_name);
                if (len < 5 || strcasecmp(e->d_name + len - 4, ".bin") != 0) continue;
                add(std::string(dir) + "/" + e->d_name, std::string(e->d_name, len - 4));
            }
            closedir(d);
        }
        // The payload Atmosphere itself reboots into, if one is set up.
        struct stat st;
        if (stat(kDefaultPayload, &st) == 0 && st.st_size > 0)
            add(kDefaultPayload, T("Default payload"));

        std::sort(found.begin(), found.end(),
                  [](const auto &a, const auto &b) {
                      return strcasecmp(a.first.c_str(), b.first.c_str()) < 0;
                  });
        for (auto &f : found) {
            m_payload_names.push_back(f.first);
            m_payloads.push_back(f.second);
        }
        if (m_payload_cursor >= (int)m_payloads.size()) m_payload_cursor = 0;
    }
    void Menu::BuildPowerRows() {
        m_power_rows.clear();
        m_power_rows.push_back(PW_Sleep);
        m_power_rows.push_back(PW_Restart);
        m_power_rows.push_back(PW_Shutdown);
        if (!m_payloads.empty()) m_power_rows.push_back(PW_Payload);  // nothing to
        m_power_rows.push_back(PW_Back);                              // chainload -> hide
    }
    void Menu::EnterPower() {
        ScanPayloads();
        BuildPowerRows();
        m_power_cursor = 0;
        m_sub_scroll   = 0;
        m_screen       = Screen::Power;
    }
    void Menu::AskPower(Action act, const char *prompt) {
        m_power_confirm = act;
        m_dialog_title  = T(prompt);
        m_dialog_note   = (m_suspended != 0) ? T("The suspended game will be closed.") : "";
        m_dialog_cursor = 1;      // default to No: none of this is undoable
        m_dialog        = Dialog::ConfirmPower;
    }
    Menu::Action Menu::OnButtonPower(Btn b) {
        if (b == Btn::B) { m_screen = Screen::Main; return Action::None; }
        const int n = (int)m_power_rows.size();
        if (n == 0) return Action::None;
        if (b == Btn::Down) m_power_cursor = (m_power_cursor + 1) % n;
        if (b == Btn::Up)   m_power_cursor = (m_power_cursor + n - 1) % n;
        if (b != Btn::A) return Action::None;

        switch (m_power_rows[m_power_cursor]) {
            case PW_Sleep:
                // The console wakes back into the menu, so leave the main screen up.
                m_screen = Screen::Main;
                return Action::PowerSleep;
            case PW_Restart:  AskPower(Action::PowerReboot,   "Restart the console?");  break;
            case PW_Shutdown: AskPower(Action::PowerShutdown, "Turn the console off?"); break;
            case PW_Payload:
                m_screen         = Screen::Payloads;
                m_payload_cursor = 0;
                m_sub_scroll     = 0;
                break;
            default: m_screen = Screen::Main; break;   // PW_Back
        }
        return Action::None;
    }
    void Menu::DrawPower() {
        DrawTopBar("Power");
        std::vector<std::string> labels, values;
        for (int id : m_power_rows) {
            switch (id) {
                case PW_Sleep:    labels.push_back(T("Sleep"));     values.push_back(""); break;
                case PW_Restart:  labels.push_back(T("Restart"));   values.push_back(""); break;
                case PW_Shutdown: labels.push_back(T("Power off")); values.push_back(""); break;
                case PW_Payload: {
                    char c[32];
                    snprintf(c, sizeof(c), "%d %s", (int)m_payloads.size(), T("found"));
                    labels.push_back(T("Reboot to payload"));
                    values.push_back(c);
                    break;
                }
                default: labels.push_back(T("Back")); values.push_back(""); break;
            }
        }
        DrawCarousel(labels, values, m_power_cursor, m_sub_scroll);
        DrawHint({ {{"up","down"}, "Select"}, {{"a"}, "Confirm"}, {{"b"}, "Back"} });
    }
    Menu::Action Menu::OnButtonPayloads(Btn b) {
        if (b == Btn::B) { m_screen = Screen::Power; m_sub_scroll = (float)m_power_cursor; return Action::None; }
        const int n = (int)m_payloads.size();
        if (n == 0) return Action::None;
        if (b == Btn::Down) m_payload_cursor = (m_payload_cursor + 1) % n;
        if (b == Btn::Up)   m_payload_cursor = (m_payload_cursor + n - 1) % n;
        if (b == Btn::A) {
            m_payload_path = m_payloads[m_payload_cursor];
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "%s %s?", T("Reboot to"),
                     m_payload_names[m_payload_cursor].c_str());
            AskPower(Action::PowerPayload, prompt);
        }
        return Action::None;
    }
    void Menu::DrawPayloads() {
        DrawTopBar("Reboot to payload");
        if (m_payloads.empty()) {
            const Theme &t = m_theme.Current();
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 340, t.dim,
                                T("No payloads found in sdmc:/payloads"));
            DrawHint({ {{"b"}, "Back"} });
            return;
        }
        std::vector<std::string> values(m_payload_names.size());
        DrawCarousel(m_payload_names, values, m_payload_cursor, m_sub_scroll);
        DrawHint({ {{"up","down"}, "Select"}, {{"a"}, "Reboot into it"}, {{"b"}, "Back"} });
    }
    // ---- Random game roll ---------------------------------------------------
    void Menu::DrawRollFrame(const std::vector<const MenuItem *> &pool, int idx, bool settled) {
        const Theme &t = m_theme.Current();
        const int cx = gfx::Gfx::Width / 2;
        DrawBackground();

        m_gfx->TextCentered(FontSize::Small, cx, 120, t.dim,
                            settled ? T("Let's play") : T("Picking something..."));

        // Icon card.
        const int size = 220, ix = cx - size / 2, iy = 190;
        m_icons.SetScale(gfx::IconCache::GridScale);
        SDL_Texture *tex = pool[idx]->app_id ? m_icons.Get(pool[idx]->app_id) : nullptr;
        if (tex) m_gfx->DrawImage(tex, ix, iy, size, size, 255);
        else     m_gfx->FillRect(ix, iy, size, size, WithAlpha(t.bg_top, 220));

        const SDL_Color edge = settled ? t.accent : WithAlpha(t.accent, 130);
        m_gfx->FillRect(ix - 4, iy - 4, size + 8, 4, edge);
        m_gfx->FillRect(ix - 4, iy + size, size + 8, 4, edge);
        m_gfx->FillRect(ix - 4, iy, 4, size, edge);
        m_gfx->FillRect(ix + size, iy, 4, size, edge);

        m_gfx->TextCentered(FontSize::Normal, cx, iy + size + 34,
                            settled ? t.accent : t.fg, pool[idx]->name.c_str());
        m_gfx->Present();
    }
    // Rolls through the library with a decelerating tick and returns whatever it
    // lands on (the landing slot is what makes it random). 0 if there are no games.
    u64 Menu::RollRandomGame() {
        std::vector<const MenuItem *> pool;
        for (const auto &i : m_items)
            if (i.kind == ItemKind::Game) pool.push_back(&i);
        if (pool.empty()) return 0;
        const int n = (int)pool.size();

        const u64 freq  = armGetSystemTickFreq();
        const u64 t0    = armGetSystemTick();
        constexpr u64 kRollMs = 2000;
        int  idx      = (int)(randomGet64() % n);
        u64  nextStep = 0;

        for (;;) {
            const u64 ms = (armGetSystemTick() - t0) * 1000 / freq;
            if (ms >= kRollMs) break;
            if (ms >= nextStep) {
                idx = (idx + 1) % n;
                const float p = (float)ms / (float)kRollMs;      // 0..1
                nextStep = ms + (u64)(40.0f + p * p * 300.0f);   // ease out
            }
            DrawRollFrame(pool, idx, false);
        }

        // Hold on the result so it reads before the game launches.
        const u64 t1 = armGetSystemTick();
        while ((armGetSystemTick() - t1) * 1000 / freq < 800)
            DrawRollFrame(pool, idx, true);

        return pool[idx]->app_id;
    }
    Menu::Action Menu::OnButtonWelcome(Btn b) {
        if (b == Btn::A || b == Btn::B || b == Btn::Plus) m_screen = Screen::Main;   // skip
        return Action::None;
    }
    // Two lines and a rule, centred on the screen: a greeting, the user's name,
    // and a hairline that draws itself outward underneath. Everything shares one
    // fade so the screen arrives and leaves as a single object instead of a
    // stack of separately animated parts.
    void Menu::DrawWelcome() {
        const Theme &t = m_theme.Current();
        const int cx = gfx::Gfx::Width / 2;
        const int cy = gfx::Gfx::Height / 2;

        const u64 freq = armGetSystemTickFreq();
        const u64 ms   = (armGetSystemTick() - m_welcome_start) * 1000 / freq;

        // Fade in, hold, fade out. Without the tail the screen used to cut
        // straight to the menu the instant the timer expired.
        float k = 1.0f;
        if (ms < kWelcomeFade)                    k = (float)ms / (float)kWelcomeFade;
        else if (ms > kWelcomeMs - kWelcomeFade)  k = (float)(kWelcomeMs - ms) / (float)kWelcomeFade;
        if (k < 0.0f) k = 0.0f;
        if (k > 1.0f) k = 1.0f;
        const Uint8 a = (Uint8)(k * 255.0f);

        // Ease the rise so the text settles rather than sliding linearly.
        const float ease = 1.0f - (1.0f - k) * (1.0f - k);
        const int   rise = (int)((1.0f - ease) * 16.0f);

        const char *greet = T(kWelcomeMsgs[m_welcome_msg % kWelcomeMsgN]);
        const bool  named = (m_nickname[0] != '\0');

        if (named) {
            m_gfx->TextCentered(FontSize::Small, cx, cy - 74 + rise, WithAlpha(t.dim, a), greet);
            m_gfx->TextCentered(FontSize::Title, cx, cy - 38 + rise, WithAlpha(t.title, a), m_nickname);
        } else {
            // No account name to show, so the greeting carries the screen alone.
            m_gfx->TextCentered(FontSize::Title, cx, cy - 38 + rise, WithAlpha(t.title, a), greet);
        }

        const int lw = (int)(200.0f * ease);
        m_gfx->FillRect(cx - lw / 2, cy + 40, lw, 2, WithAlpha(t.accent, a));
    }
    Menu::Action Menu::OnButtonAbout(Btn b) {
        const int maxScroll = std::max(0, kChangelogN - kAboutVisible);
        if (b == Btn::Down) m_about_scroll = std::min(maxScroll, m_about_scroll + 1);
        if (b == Btn::Up)   m_about_scroll = std::max(0, m_about_scroll - 1);
        if (b == Btn::B) { m_screen = Screen::Theming; m_sub_scroll = TH_About; }
        return Action::None;
    }
    void Menu::DrawAbout() {
        const Theme &t = m_theme.Current();
        DrawTopBar("About");
        const int cx = gfx::Gfx::Width / 2;

        m_gfx->TextCentered(FontSize::Large, cx, 92, t.title, "sLaunch");

        char ver[80];
        snprintf(ver, sizeof(ver), "%s v%s", T("Version"), SL_VERSION);
        if (m_upd_available) {
            char up[140];
            snprintf(up, sizeof(up), "%s  -  %s %s", ver, T("update available:"), m_upd_latest.c_str());
            m_gfx->TextCentered(FontSize::Small, cx, 158, t.accent, up);
        } else {
            char line[140];
            snprintf(line, sizeof(line), "%s  -  %s", ver,
                     m_check_updates ? T("up to date") : T("update check off"));
            m_gfx->TextCentered(FontSize::Small, cx, 158, t.dim, line);
        }
        m_gfx->TextCentered(FontSize::Small, cx, 190, t.dim, "github.com/etonedemid/slaunch");

        // "What's new" changelog (scrollable).
        const int lx = cx - 360;
        m_gfx->Text(FontSize::Normal, lx, 246, t.accent, T("What's new"));
        m_gfx->FillRect(lx, 284, 720, 2, WithAlpha(t.dim, 90));

        for (int i = 0; i < kAboutVisible; i++) {
            const int idx = m_about_scroll + i;
            if (idx >= kChangelogN) break;
            const LogLine &l = kChangelog[idx];
            const int y = kAboutTop + i * kAboutRowH;
            if (l.head) m_gfx->Text(FontSize::Normal, lx, y, t.fg, l.text);
            else        m_gfx->Text(FontSize::Small, lx + 24, y + 4, t.dim, l.text);
        }

        // Scroll affordances.
        if (m_about_scroll > 0)
            m_gfx->TextCentered(FontSize::Small, cx, kAboutTop - 26, t.dim, "^");
        if (m_about_scroll + kAboutVisible < kChangelogN)
            m_gfx->TextCentered(FontSize::Small, cx, kAboutTop + kAboutVisible * kAboutRowH, t.dim, "v");

        DrawHint({ {{"up","down"}, "Scroll"}, {{"b"}, "Back"} });
    }
    Menu::Action Menu::OnButtonTheming(Btn b) {
        auto openWidgets = [&]() {
            m_screen = Screen::Widgets;
            m_widget_cursor = 0;
            m_sub_scroll = 0;
        };
        {
            const std::vector<int> rows = ThemingRows();
            const int rn = (int)rows.size();
            int at = 0;
            for (int i = 0; i < rn; i++) if (rows[i] == m_theming_cursor) { at = i; break; }
            if (b == Btn::Down) m_theming_cursor = rows[(at + 1) % rn];
            if (b == Btn::Up)   m_theming_cursor = rows[(at + rn - 1) % rn];
        }
        auto cycleUiMode = [&]() {
            const int dir = (b == Btn::Right) ? +1 : -1;
            const int n = (int)UiMode::Count;
            m_ui_mode = (UiMode)(((int)m_ui_mode + dir + n) % n);
            // The item list itself depends on the mode now: XMB carries the full
            // scanned-homebrew category, the other layouts only pinned homebrew.
            // Rebuild before syncing the bar, so the bar points into the list it
            // is actually going to draw.
            RebuildItems();
            // A deliberate switch into XMB keeps the entry you were on, rather
            // than resetting the bar to its Games default.
            XmbSyncFromCursor(); m_xmb_placed = true;
            SaveSettings();
        };
        auto cycleAlign = [&]() {
            const int dir = (b == Btn::Right) ? +1 : -1;
            m_align = (TextAlign)(((int)m_align + dir + 3) % 3);
            SaveSettings();
        };
        auto toggleListIcons = [&]() { m_list_icons = !m_list_icons; SaveSettings(); };
        // Index 0 is the built-in set; 1.. are the folders found under
        // sdmc:/slaunch/icon_packs. Switching drops the cached textures so the
        // new art shows up on the next frame.
        auto cycleIconPack = [&](int dir) {
            const int n = (int)m_icon_packs.size() + 1;
            m_icon_pack_idx = (m_icon_pack_idx + dir + n) % n;
            InvalidateSysIcons();
            SaveIconPackSetting();
        };
        // Language. LocaleInit rebuilds the string table in place and every
        // label is fetched through T() as it is drawn, so the whole menu is in
        // the new language on the very next frame - no restart, no reload of the
        // item list.
        auto cycleLanguage = [&](int dir) {
            m_lang_idx = (m_lang_idx + dir + kLangN) % kLangN;
            strncpy(m_lang, kLangs[m_lang_idx].code, sizeof(m_lang) - 1);
            m_lang[sizeof(m_lang) - 1] = '\0';
            LocaleInit(m_lang);
            SaveSettings();
        };
        auto toggleShelfVert = [&]() {
            m_shelf_vertical = !m_shelf_vertical;
            SaveSettings();
        };
        auto toggleWrap    = [&]() { m_wrap_nav     = !m_wrap_nav;     SaveSettings(); };
        auto toggleHints   = [&]() { m_show_hints   = !m_show_hints;   SaveSettings(); };
        auto toggleCounter = [&]() { m_show_counter = !m_show_counter; SaveSettings(); };
        // Rebuild rather than just save: this decides whether the shortcuts are
        // in m_items at all, so the list it names has to be rebuilt for the
        // change to show up anywhere but XMB.
        // Off -> on asks first: a big ROM library is still able to take the
        // system down (see the IconCache pool), so this is opt-in with a warning.
        auto toggleRetroArch = [&]() {
            if (m_retroarch) {
                m_retroarch = false;
                SaveSettings();
                RebuildItems();
            } else {
                m_dialog        = Dialog::ConfirmRetroArch;
                m_dialog_cursor = 1;   // "No" first: this is a warning
                m_dialog_title  = T("RetroArch games (beta)");
                m_dialog_note   = T("Heya! this feature is beta and really unstable! If you have over a thousand ROMs in your library be aware that the system may crash sometimes!");
            }
        };
        if (m_theming_cursor == TH_RetroArch && (b == Btn::Left || b == Btn::Right))
            toggleRetroArch();
        // SteamGridDB: a second source, off unless asked for. Turning it on
        // clears what was tried this session so its art is looked for.
        if (m_theming_cursor == TH_Sgdb &&
            (b == Btn::Left || b == Btn::Right || b == Btn::A)) {
            m_sgdb_enabled = !m_sgdb_enabled;
            if (m_sgdb_enabled) m_cover_tried.clear();
            SaveSettings();
        }
        // Which country's box GameTDB is asked for first (others follow).
        if (m_theming_cursor == TH_TdbRegion &&
            (b == Btn::Left || b == Btn::Right || b == Btn::A)) {
            m_tdb_region = (m_tdb_region + (b == Btn::Left ? kTdbRegionCount - 1 : 1)) % kTdbRegionCount;
            SaveSettings();
        }
        auto toggleShortcuts = [&]() {
            m_shortcuts_everywhere = !m_shortcuts_everywhere;
            SaveSettings();
            RebuildItems();
        };
        if (m_theming_cursor == TH_Shortcuts && (b == Btn::Left || b == Btn::Right))
            toggleShortcuts();
        auto toggleUpdates = [&]() { m_check_updates = !m_check_updates; SaveSettings(); };
        if (m_theming_cursor == TH_Updates && (b == Btn::Left || b == Btn::Right))
            toggleUpdates();
        auto toggleWelcome = [&]() { m_welcome_enabled = !m_welcome_enabled; SaveSettings(); };
        if (m_theming_cursor == TH_Welcome && (b == Btn::Left || b == Btn::Right))
            toggleWelcome();
        if (m_theming_cursor == TH_UiMode && (b == Btn::Left || b == Btn::Right))
            cycleUiMode();
        if (m_theming_cursor == TH_TextPos && (b == Btn::Left || b == Btn::Right))
            cycleAlign();
        if (m_theming_cursor == TH_ListIcons && (b == Btn::Left || b == Btn::Right))
            toggleListIcons();
        if (m_theming_cursor == TH_IconPack && (b == Btn::Left || b == Btn::Right))
            cycleIconPack(b == Btn::Right ? +1 : -1);
        // Applies when the menu is next opened: the renderer is built around it
        // at start-up, and rebuilding it here would throw away every texture the
        // menu is currently holding.
        auto toggleAA = [&]() {
            m_antialias = !m_antialias;
            SaveSettings();
            SetStatus(m_antialias ? "Anti-aliasing on next time the menu opens"
                                  : "Anti-aliasing off next time the menu opens");
        };
        if (m_theming_cursor == TH_Antialias && (b == Btn::Left || b == Btn::Right))
            toggleAA();
        if (m_theming_cursor == TH_ShelfVert && (b == Btn::Left || b == Btn::Right))
            toggleShelfVert();
        // Left/Right step the wall shape, which is what every other numeric row
        // here does; A wraps forward like the cycling rows do.
        auto stepWall = [&](int dir) {
            if (m_theming_cursor == TH_TileCols) {
                int n = TileCols() + dir;
                if (n > kTileColsMax) n = kTileColsMin;
                if (n < kTileColsMin) n = kTileColsMax;
                SetTileCols(n);
            } else {
                int n = TileRowsVis() + dir;
                if (n > kTileRowsMax) n = kTileRowsMin;
                if (n < kTileRowsMin) n = kTileRowsMax;
                SetTileRows(n);
            }
        };
        if ((m_theming_cursor == TH_TileCols || m_theming_cursor == TH_TileRows) &&
            (b == Btn::Left || b == Btn::Right))
            stepWall(b == Btn::Right ? +1 : -1);
        if (m_theming_cursor == TH_Wrap && (b == Btn::Left || b == Btn::Right))
            toggleWrap();
        if (m_theming_cursor == TH_Hints && (b == Btn::Left || b == Btn::Right))
            toggleHints();
        if (m_theming_cursor == TH_Counter && (b == Btn::Left || b == Btn::Right))
            toggleCounter();
        if (m_theming_cursor == TH_Language && (b == Btn::Left || b == Btn::Right))
            cycleLanguage(b == Btn::Right ? +1 : -1);
        if (b == Btn::A) {
            switch (m_theming_cursor) {
                case TH_Themes:      m_screen = Screen::Themes;       m_theme_cursor = m_theme.CurrentIndex(); m_sub_scroll = m_theme_cursor; break;
                case TH_Fonts:       m_screen = Screen::Fonts;        m_font_cursor = m_font_applied; m_sub_scroll = m_font_cursor; break;
                case TH_Music:       OpenMusicPlayer(true); break;
                case TH_Widgets:     openWidgets(); break;
                case TH_Entries:     m_screen = Screen::SysEntries;   m_sys_cursor = 0; m_sub_scroll = 0; break;
                case TH_IconPack:    cycleIconPack(+1); break;
                case TH_Language:    cycleLanguage(+1); break;
                case TH_Antialias:   toggleAA(); break;
                case TH_ShelfVert:   toggleShelfVert(); break;
                case TH_TileCols:
                case TH_TileRows:    stepWall(+1); break;
                case TH_Wrap:        toggleWrap(); break;
                case TH_Hints:       toggleHints(); break;
                case TH_Counter:     toggleCounter(); break;
                case TH_Shortcuts:   toggleShortcuts(); break;
                case TH_RetroArch:   toggleRetroArch(); break;
                case TH_FlowSet:
                    m_screen = Screen::FlowSettings;
                    m_flowset_cursor = 0;
                    break;
                case TH_SgdbKey:
                    SgdbKeyPresent();          // make sure it is loaded to edit
                    m_kb_purpose = sl::smi::Kb_SteamGridKey;
                    m_kb_text    = m_sgdb_key;
                    m_kb_row = m_kb_col = 0;
                    m_kb_upper = false;
                    m_screen = Screen::Keyboard;
                    break;

                case TH_Welcome:     toggleWelcome(); break;
                case TH_Updates:     toggleUpdates(); break;
                case TH_About:       m_screen = Screen::About;        m_about_scroll = 0; break;
                case TH_Back:        m_screen = Screen::Main; break;
                default: break; // UI mode / text pos / list icons: left/right already handles them
            }
        }
        if (b == Btn::B) m_screen = Screen::Main;
        return Action::None;
    }
    // ---- Widgets submenu: list detected Lua widgets ------------------------
    Menu::Action Menu::OnButtonWidgets(Btn b) {
        const int n = m_widgets.Count();
        if (b == Btn::B) { m_screen = Screen::Theming; m_sub_scroll = TH_Widgets; return Action::None; }
        if (n == 0) return Action::None;
        if (b == Btn::Down) m_widget_cursor = (m_widget_cursor + 1) % n;
        if (b == Btn::Up)   m_widget_cursor = (m_widget_cursor + n - 1) % n;
        // Left/Right toggles whether the menu loads/shows this widget.
        if (b == Btn::Left || b == Btn::Right)
            m_widgets.SetEnabled(m_widget_cursor, !m_widgets.IsEnabled(m_widget_cursor));
        if (b == Btn::A) {   // A opens the widget's own options
            m_widget_sel = m_widget_cursor;
            m_widgetopt_cursor = 0;
            m_sub_scroll = 0;
            m_screen = Screen::WidgetOptions;
        }
        return Action::None;
    }
    // ---- Widget options: edit one widget's exposed variables ---------------
    Menu::Action Menu::OnButtonWidgetOptions(Btn b) {
        widgets::IWidget *w = m_widgets.At(m_widget_sel);
        const int n = w ? w->OptionCount() : 0;
        if (b == Btn::B) {
            m_screen = Screen::Widgets;
            m_sub_scroll = m_widget_cursor;
            return Action::None;
        }
        if (!w || n == 0) return Action::None;
        if (b == Btn::Down) m_widgetopt_cursor = (m_widgetopt_cursor + 1) % n;
        if (b == Btn::Up)   m_widgetopt_cursor = (m_widgetopt_cursor + n - 1) % n;

        const int oi = m_widgetopt_cursor;
        const bool isBool = (w->OptionType(oi) == "bool");

        auto toggleBool = [&]() {
            w->SetOption(oi, w->OptionValue(oi) == "1" ? "0" : "1");
        };

        if (isBool && (b == Btn::Left || b == Btn::Right)) toggleBool();
        if (b == Btn::A) {
            if (isBool) {
                toggleBool();
            } else {
                // Edit a string/int value on the software keyboard.
                m_kb_purpose = sl::smi::Kb_WidgetOption;
                m_kb_app  = (u64)m_widget_sel;
                m_kb_opt  = oi;
                m_kb_text = w->OptionValue(oi);
                m_kb_row = 0; m_kb_col = 0; m_kb_upper = false;
                m_screen = Screen::Keyboard;
            }
        }
        return Action::None;
    }
    Menu::Action Menu::OnButtonThemes(Btn b) {
        const int nThemes = m_theme.Count();
        const int listN   = nThemes + 1;
        const int newIdx  = nThemes; 

        auto openEditor = [&](int theme_idx) {
            m_editing_theme = theme_idx;
            m_theme.Select(theme_idx);
            m_theme_cursor = theme_idx;
            m_edit_cursor  = 0;
            ScanWallpapers();
            m_screen = Screen::ThemeEditor;
        };

        if (b == Btn::Down) { m_theme_cursor = (m_theme_cursor + 1) % listN; if (m_theme_cursor < nThemes) m_theme.Select(m_theme_cursor); }
        if (b == Btn::Up)   { m_theme_cursor = (m_theme_cursor + listN - 1) % listN; if (m_theme_cursor < nThemes) m_theme.Select(m_theme_cursor); }
        if (b == Btn::A) {
            if (m_theme_cursor == newIdx) {
                openEditor(m_theme.AddCustom());   
            } else {
                m_theme.Select(m_theme_cursor);
                m_theme.Save();
                SetStatus("Theme applied");
                m_sfx_confirm = true;
            }
        }
        if (b == Btn::Y && m_theme_cursor < nThemes && m_theme.IsCustom(m_theme_cursor))
            openEditor(m_theme_cursor);
        if (b == Btn::B) {
            m_theme.Load();
            m_theme_cursor = m_theme.CurrentIndex();
            m_screen = Screen::Theming;
            m_sub_scroll = m_theming_cursor;
        }
        return Action::None;
    }
    void Menu::ScanWallpapers() {
        m_wallpapers.clear();
        // Accept images from either the documented themes folder or the slaunch
        // root, so wherever the user drops them works. .mp4 is included here
        // too (video wallpapers, see gfx::VideoPlayer) - IsVideoPath decides
        // at draw time whether a picked path is played back or drawn as a
        // still image, so both kinds share this one picker list.
        const char *dirs[2] = { "sdmc:/slaunch/themes", "sdmc:/slaunch" };
        for (const char *dir : dirs) {
            DIR *d = opendir(dir);
            if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d)) != nullptr) {
                const char *name = e->d_name;
                size_t len = strlen(name);
                if (len < 5) continue;
                const char *e4 = name + len - 4;
                const char *e5 = len >= 5 ? name + len - 5 : "";
                if (strcasecmp(e4, ".jpg") == 0 || strcasecmp(e4, ".png") == 0 ||
                    strcasecmp(e4, ".bmp") == 0 || strcasecmp(e5, ".jpeg") == 0 ||
                    strcasecmp(e4, ".mp4") == 0)
                    m_wallpapers.push_back(std::string(dir) + "/" + name);
            }
            closedir(d);
        }
    }
    // ---- Icon packs --------------------------------------------------------
    // Discover user icon packs: each subdirectory of sdmc:/slaunch/icon_packs/
    // is treated as a pack. The pack name is the directory name.
    void Menu::ScanIconPacks() {
        m_icon_packs.clear();
        DIR *d = opendir("sdmc:/slaunch/icon_packs");
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_type == DT_DIR && e->d_name[0] != '.') {
                m_icon_packs.push_back(e->d_name);
            }
        }
        closedir(d);
        std::sort(m_icon_packs.begin(), m_icon_packs.end());
    }
    void Menu::LoadIconPackSetting() {
        // Default to Minimal when it is installed. The built-in set is the older
        // 64x64 RGB artwork and stays as the fallback - both for anyone who
        // prefers it and for any icon a pack happens not to ship - but it is no
        // longer what a fresh install looks at.
        //
        // Resolved by name rather than by index because the index depends on
        // whatever else the user has dropped in sdmc:/slaunch/icon_packs, and
        // that is sorted alphabetically.
        for (int i = 0; i < (int)m_icon_packs.size(); i++) {
            if (m_icon_packs[i] != "Minimal") continue;
            m_icon_pack_idx = i + 1;   // 0 is the built-in set
            break;
        }

        FILE *fp = fopen(GetUserConfigPath("icon_pack.txt").c_str(), "r");
        if (!fp) return;   // no saved choice: keep the default picked above
        char line[64];
        while (fgets(line, sizeof(line), fp)) {
            int v = 0;
            if (sscanf(line, "icon_pack=%d", &v) == 1)
                m_icon_pack_idx = v;
        }
        fclose(fp);
        // Clamp to valid range (0 = built-in, 1..N = packs)
        const int max_idx = (int)m_icon_packs.size();
        if (m_icon_pack_idx < 0) m_icon_pack_idx = 0;
        if (m_icon_pack_idx > max_idx) m_icon_pack_idx = 0;
    }
    void Menu::SaveIconPackSetting() {
        EnsureUserConfigDir();
        const std::string path = GetUserConfigPath("icon_pack.txt");
        FILE *fp = fopen(path.c_str(), "w");
        if (!fp) return;
        fprintf(fp, "icon_pack=%d\n", m_icon_pack_idx);
        fclose(fp);
    }
    void Menu::CycleBackground(int dir) {
        if (!m_theme.IsCustom(m_editing_theme)) return;
        Theme &c = m_theme.CustomAt(m_editing_theme);
        const int n = (int)BackgroundStyle_Count;
        c.background_style = (c.background_style + dir + n) % n;
        m_theme.Select(m_editing_theme);
        m_theme_cursor    = m_editing_theme;
        m_wallpaper_theme = -1;
    }
    // Cycle the optional photo overlay (independent of background style).
    // Rotates: empty -> first wallpaper -> next wallpaper -> ... -> empty.
    void Menu::CycleWallpaper(int dir) {
        if (!m_theme.IsCustom(m_editing_theme)) return;
        Theme &c = m_theme.CustomAt(m_editing_theme);
        if (m_wallpapers.empty()) {
            c.wallpaper[0] = '\0';
            return;
        }
        // Find current index (or -1 if empty).
        int idx = -1;
        for (int i = 0; i < (int)m_wallpapers.size(); i++) {
            if (m_wallpapers[i] == c.wallpaper) { idx = i; break; }
        }
        int n = (int)m_wallpapers.size() + 1; // +1 for the empty slot
        idx = (idx + 1 + dir + n) % n;        // +1 because idx=-1 maps to slot 0
        if (idx == 0) {
            c.wallpaper[0] = '\0';
        } else {
            strncpy(c.wallpaper, m_wallpapers[idx - 1].c_str(), sizeof(c.wallpaper) - 1);
            c.wallpaper[sizeof(c.wallpaper) - 1] = '\0';
        }
        m_theme.Select(m_editing_theme);
        m_theme_cursor    = m_editing_theme;
        m_wallpaper_theme = -1;
    }
    void Menu::OpenColorPicker(SDL_Color *target, Screen back, bool preview) {
        m_pick_target   = target;
        m_pick_original = *target;
        m_pick_channel  = 0;
        m_pick_return   = back;
        m_pick_preview  = preview;
        m_screen        = Screen::ColorPicker;
    }
    Menu::Action Menu::OnButtonEditor(Btn b) {
        if (!m_theme.IsCustom(m_editing_theme)) { m_screen = Screen::Themes; return Action::None; }
        Theme &c = m_theme.CustomAt(m_editing_theme);

        if (b == Btn::Down) {
            m_edit_cursor = (m_edit_cursor + 1) % EF_Count;
            auto skipHidden = [&](int &cursor, int dir) {
                while ((IsRibbonRow(cursor) || IsFxColourRow(cursor))
                       && !StyleHasParams(c.background_style)) {
                    cursor = (cursor + dir + EF_Count) % EF_Count;
                }
                while (IsBlurRadiusRow(cursor) && !c.wallpaper_blur) {
                    cursor = (cursor + dir + EF_Count) % EF_Count;
                }
            };
            skipHidden(m_edit_cursor, +1);
        }
        if (b == Btn::Up) {
            m_edit_cursor = (m_edit_cursor + EF_Count - 1) % EF_Count;
            auto skipHidden = [&](int &cursor, int dir) {
                while ((IsRibbonRow(cursor) || IsFxColourRow(cursor))
                       && !StyleHasParams(c.background_style)) {
                    cursor = (cursor + dir + EF_Count) % EF_Count;
                }
                while (IsBlurRadiusRow(cursor) && !c.wallpaper_blur) {
                    cursor = (cursor + dir + EF_Count) % EF_Count;
                }
            };
            skipHidden(m_edit_cursor, -1);
        }

        if (m_edit_cursor == EF_Background) {
            if (b == Btn::Right) CycleBackground(+1);
            if (b == Btn::Left)  CycleBackground(-1);
        }
        if (m_edit_cursor == EF_Wallpaper) {
            if (b == Btn::Right) CycleWallpaper(+1);
            if (b == Btn::Left)  CycleWallpaper(-1);
        }
        // Toggle individual effects with left/right/A.
        if (m_edit_cursor == EF_WallpaperDim) {
            if (b == Btn::Right || b == Btn::Left || b == Btn::A) c.wallpaper_dim = !c.wallpaper_dim;
        }
        if (m_edit_cursor == EF_WallpaperBlur) {
            // Live per-frame blur of a playing video is not supported (see
            // EnsureWallpaper) - the row stays visible but greyed out and
            // ignores input while a video wallpaper is active.
            if ((b == Btn::Right || b == Btn::Left || b == Btn::A) && !IsVideoPath(c.wallpaper))
                c.wallpaper_blur = !c.wallpaper_blur;
        }
        if (m_edit_cursor == EF_WallpaperBlurRadius) {
            if (b == Btn::Right) c.wallpaper_blur_radius = (c.wallpaper_blur_radius < 32) ? c.wallpaper_blur_radius + 2 : 2;
            if (b == Btn::Left)  c.wallpaper_blur_radius = (c.wallpaper_blur_radius >  2) ? c.wallpaper_blur_radius - 2 : 32;
        }
        if (m_edit_cursor == EF_WallpaperSnow) {
            if (b == Btn::Right || b == Btn::Left || b == Btn::A) c.wallpaper_snow = !c.wallpaper_snow;
        }

        // Icon plate opacity, in 1/16th steps so the whole range is a sensible
        // number of presses. Clamps at both ends rather than wrapping: sliding
        // off "opaque" straight to "invisible" is never what you meant.
        if (m_edit_cursor == EF_IconBgAlpha) {
            const int step = 16;
            if (b == Btn::Right) c.icon_bg_alpha = (c.icon_bg_alpha + step > 255) ? 255 : c.icon_bg_alpha + step;
            if (b == Btn::Left)  c.icon_bg_alpha = (c.icon_bg_alpha - step <   0) ?   0 : c.icon_bg_alpha - step;
        }

        // Adjust ribbon parameters with left/right.
        if (StyleHasParams(c.background_style)) {
            if (m_edit_cursor == EF_RibbonLines) {
                if (b == Btn::Right) c.Fx().lines  = (c.Fx().lines  < 40) ? c.Fx().lines  + 1 : 1;
                if (b == Btn::Left)  c.Fx().lines  = (c.Fx().lines  >  1) ? c.Fx().lines  - 1 : 40;
            }
            if (m_edit_cursor == EF_RibbonThickness) {
                if (b == Btn::Right) c.Fx().thickness   = (c.Fx().thickness   < 20) ? c.Fx().thickness   + 1 : 1;
                if (b == Btn::Left)  c.Fx().thickness   = (c.Fx().thickness   >  1) ? c.Fx().thickness   - 1 : 20;
            }
            if (m_edit_cursor == EF_RibbonAmplitude) {
                if (b == Btn::Right) c.Fx().amplitude   = (c.Fx().amplitude   < 60) ? c.Fx().amplitude   + 1 : 5;
                if (b == Btn::Left)  c.Fx().amplitude   = (c.Fx().amplitude   >  5) ? c.Fx().amplitude   - 1 : 60;
            }
            if (m_edit_cursor == EF_RibbonSeed) {
                if (b == Btn::Right) c.Fx().seed = (c.Fx().seed < 99) ? c.Fx().seed + 1 : 0;
                if (b == Btn::Left)  c.Fx().seed = (c.Fx().seed >  0) ? c.Fx().seed - 1 : 99;
            }
            if (m_edit_cursor == EF_RibbonLayers) {
                if (b == Btn::Right) c.Fx().layers = (c.Fx().layers < 12) ? c.Fx().layers + 1 : 1;
                if (b == Btn::Left)  c.Fx().layers = (c.Fx().layers >  1) ? c.Fx().layers - 1 : 12;
            }
            if (m_edit_cursor == EF_FxCam) {
                if (b == Btn::Right) c.Fx().cam = (c.Fx().cam < 200) ? c.Fx().cam + 5 : 200;
                if (b == Btn::Left)  c.Fx().cam = (c.Fx().cam >   5) ? c.Fx().cam - 5 : 5;
            }
            if (m_edit_cursor == EF_FxX) {
                // Clamped, not wrapped: sliding off one edge of the screen
                // straight to the other is never what you meant.
                if (b == Btn::Right) c.Fx().x = (c.Fx().x < 100) ? c.Fx().x + 2 : 100;
                if (b == Btn::Left)  c.Fx().x = (c.Fx().x >   0) ? c.Fx().x - 2 : 0;
            }
            if (m_edit_cursor == EF_RibbonYCenter) {
                if (b == Btn::Right) c.Fx().y = (c.Fx().y < 1120) ? c.Fx().y + 10 : -400;
                if (b == Btn::Left)  c.Fx().y = (c.Fx().y > -400) ? c.Fx().y - 10 : 1120;
            }
        }

        // Leaving the editor keeps the edits: B saves exactly as the Save row
        // does, rather than walking away from changes that were never written.
        auto saveAndLeave = [&]() {
            m_theme.Select(m_editing_theme);
            m_theme.Save();
            SetStatus("Theme saved");
            m_sfx_confirm = true;
            m_theme_cursor = m_editing_theme;
            m_screen = Screen::Themes;
        };
        if (b == Btn::A) {
            if (m_edit_cursor == EF_Save) {
                saveAndLeave();
            } else if (m_edit_cursor == EF_Rename) {
                m_kb_purpose = sl::smi::Kb_ThemeName;
                m_kb_app = (u64)m_editing_theme;
                m_kb_text = c.name;
                m_kb_row = 0; m_kb_col = 0; m_kb_upper = false;
                m_screen = Screen::Keyboard;
            } else if (m_edit_cursor == EF_Delete) {
                m_theme.DeleteCustom(m_editing_theme);
                m_theme.Save();
                m_editing_theme = -1;
                m_theme_cursor = m_theme.CurrentIndex();
                // The wallpaper cache is keyed by theme index; deleting a
                // theme shifts every later one down a slot, so the index the
                // cache remembers can now belong to a different theme.
                m_wallpaper_theme = -1;
                SetStatus("Theme deleted");
                m_sfx_confirm = true;
                m_screen = Screen::Themes;
            } else if (SDL_Color *col = EditorColor(c, m_edit_cursor)) {
                OpenColorPicker(col);
            }
        }
        if (b == Btn::B) saveAndLeave();
        return Action::None;
    }
    Menu::Action Menu::OnButtonColorPicker(Btn b) {
        if (!m_pick_target) { m_screen = m_pick_return; return Action::None; }
        Uint8 *ch[3] = { &m_pick_target->r, &m_pick_target->g, &m_pick_target->b };

        if (b == Btn::Down) m_pick_channel = (m_pick_channel + 1) % 3;
        if (b == Btn::Up)   m_pick_channel = (m_pick_channel + 2) % 3;

        auto adjust = [&](int d) {
            int v = (int)*ch[m_pick_channel] + d;
            *ch[m_pick_channel] = (Uint8)(v < 0 ? 0 : v > 255 ? 255 : v);
            if (m_pick_preview) m_theme.Select(m_editing_theme); // live preview
        };
        if (b == Btn::Right) adjust(+1);
        if (b == Btn::Left)  adjust(-1);
        if (b == Btn::R)     adjust(+16); // shoulder buttons = coarse steps
        if (b == Btn::L)     adjust(-16);

        if (b == Btn::A || b == Btn::B) {
            if (b == Btn::B) *m_pick_target = m_pick_original;
            m_pick_target = nullptr;
            if (m_pick_tile) { SaveTileCfg(); m_pick_tile = false; }
            m_screen = m_pick_return;
        }
        return Action::None;
    }
    // ---- Fonts screen -------------------------------------------------------
    Menu::Action Menu::OnButtonFonts(Btn b) {
        int n = (int)m_font_names.size();
        if (n == 0) { if (b == Btn::B) m_screen = Screen::Theming; return Action::None; }

        if (b == Btn::Down) m_font_cursor = (m_font_cursor + 1) % n;
        if (b == Btn::Up)   m_font_cursor = (m_font_cursor + n - 1) % n;
        if (b == Btn::A) {
            ApplyFont(m_font_cursor);
            m_font_applied = m_font_cursor;
            SaveFontConfig();
            SetStatus("Font applied");
                m_sfx_confirm = true;
        }
        if (b == Btn::B) {
            // Revert any live preview back to the applied font, then leave.
            ApplyFont(m_font_applied);
            m_screen = Screen::Theming;
            m_sub_scroll = m_theming_cursor;
        }
        return Action::None;
    }
    // ---- Font management ----------------------------------------------------
    void Menu::ScanFonts() {
        m_font_names.clear();
        m_font_paths.clear();
        m_font_names.push_back("Default (System)");
        m_font_paths.push_back("");
        if (!g_sd_ok) return; // no SD -> system font only

        DIR *d = opendir("sdmc:/slaunch/fonts");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != nullptr) {
                const char *name = e->d_name;
                size_t len = strlen(name);
                if (len < 5) continue;
                // .ttc is a TrueType collection; FreeType opens face 0, which is
                // what the bundled CJK font wants.
                const char *ext = name + len - 4;
                if (strcasecmp(ext, ".ttf") != 0 && strcasecmp(ext, ".otf") != 0 &&
                    strcasecmp(ext, ".ttc") != 0) continue;

                std::string display(name, len - 4); // strip extension
                std::string path = std::string("sdmc:/slaunch/fonts/") + name;
                m_font_names.push_back(std::move(display));
                m_font_paths.push_back(std::move(path));
            }
            closedir(d);
        }
    }
    void Menu::ApplyFont(int index) {
        if (index <= 0 || index >= (int)m_font_paths.size()) {
            m_gfx->ClearContentFont();
            m_font_preview = 0;
            return;
        }
        if (!m_gfx->LoadContentFont(m_font_paths[index].c_str())) {
            // Failed to load - fall back to the system font.
            m_gfx->ClearContentFont();
            m_font_preview = 0;
            return;
        }
        m_font_preview = index;
    }
    void Menu::EnsurePreviewFont(int index) {
        if (index == m_font_preview) return;
        ApplyFont(index);
    }
    void Menu::LoadFontConfig() {
        m_font_applied = 0;
        if (!g_sd_ok) { ApplyFont(0); return; }
        FILE *fp = fopen(GetUserConfigPath("font.cfg").c_str(), "r");
        if (fp) {
            char line[160];
            if (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\r\n")] = '\0';
                const char *val = line;
                if (strncmp(line, "font=", 5) == 0) val = line + 5;
                // Match saved value against a known display name.
                for (int i = 0; i < (int)m_font_names.size(); i++) {
                    if (m_font_names[i] == val) { m_font_applied = i; break; }
                }
            }
            fclose(fp);
        }
        ApplyFont(m_font_applied);
    }
    void Menu::SaveFontConfig() {
        if (!g_sd_ok) return;
        EnsureUserConfigDir();
        FILE *fp = fopen(GetUserConfigPath("font.cfg").c_str(), "w");
        if (!fp) return;
        const char *name = (m_font_applied >= 0 && m_font_applied < (int)m_font_names.size())
                           ? m_font_names[m_font_applied].c_str() : "Default (System)";
        fprintf(fp, "font=%s\n", name);
        fclose(fp);
    }
    Menu::Action Menu::OnButtonDialog(Btn b, u64 &out_app_id) {
        if (m_dialog == Dialog::CrashReport) {
            if (b == Btn::A || b == Btn::B) { m_dialog = Dialog::None; m_dialog_title.clear(); m_dialog_note.clear(); }
            return Action::None;
        }
        if (b == Btn::Up || b == Btn::Down) m_dialog_cursor ^= 1;
        if (b == Btn::B) { m_dialog = Dialog::None; m_pending_launch = 0; return Action::None; }
        if (b == Btn::A) {
            bool yes = (m_dialog_cursor == 0);
            Dialog which = m_dialog;
            m_dialog = Dialog::None;
            if (!yes) { m_pending_launch = 0; m_power_confirm = Action::None; return Action::None; }
            if (which == Dialog::ConfirmCloseForLaunch) {
                out_app_id = m_pending_launch;
                m_pending_launch = 0;
                return Action::LaunchApp;
            }
            if (which == Dialog::ConfirmCloseGame) return Action::TerminateApp;
            if (which == Dialog::ConfirmDelete) { FmConfirmDelete(); return Action::None; }
            if (which == Dialog::ConfirmRetroArch) {
                m_retroarch = true;
                SaveSettings();
                RebuildItems();
                return Action::None;
            }
            if (which == Dialog::ConfirmPower) {
                const Action act = m_power_confirm;
                m_power_confirm  = Action::None;
                return act;
            }
        }
        return Action::None;
    }
    // ---- setup wizard -----------------------------------------------------
    // One page per step, all built the same way: progress along the top, a
    // title and a line saying what this step is for, one piece of content,
    // and the buttons at the bottom. The layout step is a gallery of real
    // screenshots (sdmc:/slaunch/previews, shipped with the menu), because a
    // layout is something you choose by looking at it.
    static const char *kLayoutFiles[] = { "list", "line", "grid", "cover",
                                          "shelf", "xmb", "flow", "deck" };
    static_assert(sizeof(kLayoutFiles) / sizeof(kLayoutFiles[0]) == (size_t)UiMode::Count,
                  "one preview per layout");

    SDL_Texture *Menu::OobePreview(int mode) {
        if (mode < 0 || mode >= (int)UiMode::Count) return nullptr;
        if (!m_oobe_prev_tried[mode]) {
            m_oobe_prev_tried[mode] = true;
            char path[80];
            snprintf(path, sizeof(path), "sdmc:/slaunch/previews/%s.jpg", kLayoutFiles[mode]);
            m_oobe_prev[mode] = m_gfx->LoadImage(path);
        }
        return m_oobe_prev[mode];
    }
    void Menu::FreeOobePreviews() {
        for (int i = 0; i < (int)UiMode::Count; i++) {
            if (m_oobe_prev[i]) m_gfx->FreeImage(m_oobe_prev[i]);
            m_oobe_prev[i] = nullptr;
            m_oobe_prev_tried[i] = false;
        }
    }

    // Break text into lines no wider than `width`, at spaces.
    std::vector<std::string> Menu::WrapText(FontSize fs, const std::string &text, int width) {
        std::vector<std::string> lines;
        std::string line, word;
        auto flush_word = [&]() {
            if (word.empty()) return;
            const std::string tryl = line.empty() ? word : line + " " + word;
            if (!line.empty() && m_gfx->TextWidth(fs, tryl.c_str()) > width) {
                lines.push_back(line);
                line = word;
            } else {
                line = tryl;
            }
            word.clear();
        };
        for (char c : text) {
            if (c == ' ') flush_word(); else word += c;
        }
        flush_word();
        if (!line.empty()) lines.push_back(line);
        return lines;
    }

    void Menu::DrawOobe() {
        const Theme &t = m_theme.Current();
        const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;

        static const char *kLayoutNames[] = { "List", "Line", "Grid", "Cover",
                                              "Shelf", "XMB", "Flow", "Deck" };
        static const char *kLayoutDesc[] = {
            "A simple scrolling list", "A carousel of covers",
            "A wall of tiles", "One big cover at a time",
            "A shelf of covers with details", "The PlayStation cross-media bar",
            "3D game cases on a mirrored floor", "Steam Deck: hero, covers and news" };

        struct Page { const char *title, *sub; };
        const Page pages[kOobeSteps] = {
            { "Welcome to sLaunch", "A clean HOME Menu replacement. Let's set it up - it only takes a moment." },
            { "Choose a layout",    "How your games are laid out. You can change it any time in Theming." },
            { "Pick a theme",       "Applied as you browse. Make your own later in Theming > Themes." },
            { "Good to know",       "A few things worth knowing before you start." },
            { "You're all set",     "Enjoy sLaunch." },
        };
        const int step = m_oobe_step;

        // ---- progress + heading ----
        {
            const int seg = 64, gap = 10, total = kOobeSteps * seg + (kOobeSteps - 1) * gap;
            for (int i = 0; i < kOobeSteps; i++) {
                const int x = (W - total) / 2 + i * (seg + gap);
                const SDL_Color c = (i <= step) ? t.accent : WithAlpha(t.dim, 90);
                m_gfx->FillRect(x, 36, seg, 3, c);
            }
            m_gfx->TextCentered(FontSize::Title, W / 2, 62, t.title, T(pages[step].title));
            m_gfx->TextCentered(FontSize::Normal, W / 2, 130, t.dim,
                                Ellipsize(T(pages[step].sub), W - 160, FontSize::Normal).c_str());
        }

        // A preview card: the screenshot, or a named placeholder when the
        // file is missing (an SD card set up without the previews folder).
        auto preview = [&](int mode, float x, float y, float w, float h, Uint8 a, bool sel) {
            SDL_Texture *tex = OobePreview(mode);
            if (sel) {                                 // a thin frame, nothing more
                const SDL_Color f = WithAlpha(t.accent, a);
                m_gfx->FillRect((int)x - 3, (int)y - 3, (int)w + 6, 3, f);
                m_gfx->FillRect((int)x - 3, (int)(y + h), (int)w + 6, 3, f);
                m_gfx->FillRect((int)x - 3, (int)y, 3, (int)h, f);
                m_gfx->FillRect((int)(x + w), (int)y, 3, (int)h, f);
            }
            m_gfx->FillRect((int)x, (int)y, (int)w, (int)h, WithAlpha(t.bg_bottom, a));
            if (tex) m_gfx->DrawImage(tex, (int)x, (int)y, (int)w, (int)h, a);
            if (!tex)
                m_gfx->TextCentered(FontSize::Large, (int)(x + w / 2), (int)(y + h / 2 - 20),
                                    WithAlpha(t.dim, a), T(kLayoutNames[mode]));
        };

        switch (step) {
            case 0: {   // three layouts fanned out, to show what this is
                preview((int)UiMode::XMB, (W - 640) / 2.0f, 200.0f, 640.0f, 360.0f, 255, false);
                DrawHint({ {{"a"}, "Get started"} });
                break;
            }
            case 1: {   // layout gallery
                const int n = (int)UiMode::Count, cur = (int)m_ui_mode;
                // Chase the choice the short way round, so wrapping from Deck
                // to List slides one step instead of across the whole gallery.
                float target = (float)cur;
                while (target - m_oobe_gallery >  n * 0.5f) target -= n;
                while (target - m_oobe_gallery < -n * 0.5f) target += n;
                m_oobe_gallery += (target - m_oobe_gallery) * 0.22f;
                if (std::abs(target - m_oobe_gallery) < 0.002f) m_oobe_gallery = target;

                std::vector<std::pair<float, int>> order;   // |d|, virtual index
                for (int k = -2; k <= 2; k++) {
                    const int v = (int)lroundf(m_oobe_gallery) + k;
                    order.push_back({ std::abs((float)v - m_oobe_gallery), v });
                }
                std::sort(order.begin(), order.end(), [](auto &p, auto &q) { return p.first > q.first; });
                for (auto &o : order) {
                    const float d  = (float)o.second - m_oobe_gallery;
                    const float ad = std::min(std::abs(d), 1.5f);
                    const float w  = 580.0f * (1.0f - std::min(ad, 1.0f) * 0.34f);
                    const float h  = w * 9.0f / 16.0f;
                    const float cx = W / 2.0f + d * 440.0f;
                    const float cy = 355.0f;
                    const Uint8 a  = (Uint8)(255.0f - ad * 120.0f);
                    const int mode = ((o.second % n) + n) % n;
                    preview(mode, cx - w / 2, cy - h / 2, w, h, a, mode == cur && ad < 0.5f);
                }
                m_gfx->TextCentered(FontSize::Large, W / 2, 540, t.title, T(kLayoutNames[cur]));
                if (cur == (int)UiMode::XMB) {             // the one we suggest
                    const int nw = m_gfx->TextWidth(FontSize::Large, T(kLayoutNames[cur]));
                    m_gfx->Text(FontSize::Small, W / 2 + nw / 2 + 14, 552, t.accent, T("Recommended"));
                }
                m_gfx->TextCentered(FontSize::Normal, W / 2, 588, t.dim, T(kLayoutDesc[cur]));
                for (int i = 0; i < n; i++) {               // where you are in the gallery
                    const int dx = W / 2 + (i - n / 2) * 22 + 11, dy = 638;
                    m_gfx->FillRect(dx - 4, dy - 4, 8, 8, i == cur ? t.accent : WithAlpha(t.dim, 110));
                }
                DrawHint({ {{"left","right"}, "Choose"}, {{"a"}, "Next"}, {{"b"}, "Back"} });
                break;
            }
            case 2: {   // themes, each with its colours
                const int n = m_theme.Count();
                m_oobe_list += ((float)m_theme_cursor - m_oobe_list) * 0.25f;
                if (std::abs((float)m_theme_cursor - m_oobe_list) < 0.01f) m_oobe_list = (float)m_theme_cursor;
                const int rowH = 62, listW = 620, x = (W - listW) / 2, cy = 380;
                for (int i = 0; i < n; i++) {
                    const float d = (float)i - m_oobe_list;
                    if (std::abs(d) > 3.4f) continue;
                    const int   y = cy + (int)(d * rowH) - (rowH - 12) / 2;
                    const Uint8 a = (Uint8)std::max(60.0f, 255.0f - std::abs(d) * 60.0f);
                    const bool  sel = (i == m_theme_cursor);
                    const Theme &th = m_theme.At(i);
                    const int lh = m_gfx->LineHeight(FontSize::Normal);
                    const int ty = y + (rowH - 12 - lh) / 2;
                    if (sel) m_gfx->Text(FontSize::Normal, x - 30, ty, WithAlpha(t.accent, a), ">");
                    m_gfx->Text(FontSize::Normal, x, ty, WithAlpha(sel ? t.accent : t.fg, a), th.name);
                    const SDL_Color sw[4] = { th.bg_top, th.bg_bottom, th.accent, th.title };
                    for (int k = 0; k < 4; k++) {           // the theme's own colours
                        const int sx = x + listW - (4 - k) * 26, sy = y + (rowH - 12) / 2 - 9;
                        m_gfx->FillRect(sx - 1, sy - 1, 20, 20, WithAlpha(t.dim, (Uint8)(a / 3)));
                        m_gfx->FillRect(sx, sy, 18, 18, WithAlpha(sw[k], a));
                    }
                }
                DrawHint({ {{"up","down"}, "Choose"}, {{"a"}, "Next"}, {{"b"}, "Back"} });
                break;
            }
            case 3: {   // tips, as four cards
                struct Tip { const char *key; const char *val; };
                const Tip tips[4] = {
                    { "X",        "Options on any entry: favourite, rename, move" },
                    { "Theming",  "Fonts, colours, background music, widgets" },
                    { "Homebrew", "Browse .nro files and pin them to this menu" },
                    { "HOME",     "Suspends your game and brings this back" },
                };
                const int keyX = 250, textX = 440, y0 = 230, rowGap = 70;
                const int lhN = m_gfx->LineHeight(FontSize::Normal);
                for (int i = 0; i < 4; i++) {
                    const int y = y0 + i * rowGap;
                    m_gfx->Text(FontSize::Normal, keyX, y, t.accent, tips[i].key);
                    m_gfx->Text(FontSize::Normal, textX, y, t.fg,
                                Ellipsize(T(tips[i].val), W - 80 - textX, FontSize::Normal).c_str());
                    (void)lhN;
                }
                DrawHint({ {{"a"}, "Next"}, {{"b"}, "Back"} });
                break;
            }
            default: {  // done: the one last choice
                const int rw = 640, rh = 64, x = (W - rw) / 2, y = 300;
                m_gfx->Text(FontSize::Normal, x - 30, y + (rh - m_gfx->LineHeight(FontSize::Normal)) / 2,
                            t.accent, ">");
                const int lh = m_gfx->LineHeight(FontSize::Normal);
                const char *val = m_check_updates ? T("On") : T("Off");
                m_gfx->Text(FontSize::Normal, x + 24, y + (rh - lh) / 2, t.title,
                            T("Check for updates on startup"));
                m_gfx->Text(FontSize::Normal, x + rw - 24 - m_gfx->TextWidth(FontSize::Normal, val),
                            y + (rh - lh) / 2, t.accent, val);
                m_gfx->TextCentered(FontSize::Small, W / 2, y + rh + 26, t.dim,
                                    T("Everything here can be changed later in Theming."));
                DrawHint({ {{"left","right"}, "Change"}, {{"a"}, "Finish"}, {{"b"}, "Back"} });
                break;
            }
        }
        (void)H;
    }
    // Rows currently shown in Theming. The SteamGridDB key is only meaningful
    // to the coverflow, so in every other layout it is absent rather than
    // sitting there inert - which means the cursor moves over a filtered list,
    // the same way the theme editor handles its ribbon rows.
    std::vector<int> Menu::ThemingRows() const {
        std::vector<int> v;
        v.reserve(TH_Count);
        for (int i = 0; i < TH_Count; i++) {
            // Both of these only mean anything to the coverflow.
            // The key row belongs to any layout that draws box art; the Flow
            // tuning screen only to Flow.
            if (i == TH_TdbRegion && m_ui_mode != UiMode::Flow &&
                                     m_ui_mode != UiMode::Deck && m_ui_mode != UiMode::Shelf) continue;
            if (i == TH_Sgdb && m_ui_mode != UiMode::Flow &&
                                m_ui_mode != UiMode::Deck && m_ui_mode != UiMode::Shelf) continue;
            if (i == TH_SgdbKey && !m_sgdb_enabled) continue;
            if (i == TH_SgdbKey && m_ui_mode != UiMode::Flow &&
                                   m_ui_mode != UiMode::Deck) continue;
            if (i == TH_FlowSet && m_ui_mode != UiMode::Flow) continue;
            // XMB gives the shortcuts a column each whatever this says, so
            // there the row would be a switch wired to nothing.
            if (i == TH_Shortcuts && (m_ui_mode == UiMode::XMB || !m_retroarch)) continue;
            if (i == TH_ShelfVert && m_ui_mode != UiMode::Shelf) continue;
            // The wall shape is only meaningful where there is a wall.
            if ((i == TH_TileCols || i == TH_TileRows) && m_ui_mode != UiMode::Grid) continue;
            v.push_back(i);
        }
        return v;
    }
    bool Menu::SgdbKeyPresent() {
        if (m_sgdb_key_loaded) return !m_sgdb_key.empty();
        m_sgdb_key_loaded = true;
        if (FILE *fp = fopen("sdmc:/slaunch/config/steamgriddb.txt", "r")) {
            char buf[128] = {};
            if (fgets(buf, sizeof(buf), fp)) {
                // Trim trailing CR, LF, space and tab by code, so a key
                // pasted with a stray newline still works.
                size_t kl = strlen(buf);
                while (kl && (buf[kl - 1] == 13 || buf[kl - 1] == 10 ||
                              buf[kl - 1] == 32 || buf[kl - 1] == 9))
                    buf[--kl] = 0;
                m_sgdb_key = buf;
            }
            fclose(fp);
        }
        return !m_sgdb_key.empty();
    }
    void Menu::DrawTheming() {
        DrawTopBar("Theming");
        const char *modes[8]  = { T("List"), T("Line"), T("Grid"), T("Cover"),
                                  T("Shelf"), T("XMB"), T("Flow"), T("Deck") };
        const char *aligns[3] = { T("Left"), T("Center"), T("Right") };
        std::vector<std::string> labels = {
            T("Themes"), T("UI mode"), T("Text position"), T("List icons"),
            T("Icon pack"), T("Anti-aliasing"), T("Vertical covers"),
            T("Columns"), T("Rows"),
            T("Box art region"), T("SteamGridDB"), T("SteamGridDB key"), T("Flow layout"),
            T("Wrap around"), T("Button hints"), T("Position counter"),
            T("RetroArch games"), T("Shortcuts everywhere"),
            T("Fonts"), T("Language"),
            T("Music"), T("Widgets"),
            T("Menu entries"),
            T("Welcome screen"), T("Check for updates"),
            T("About"), T("Back")
        };
        std::vector<std::string> values(labels.size());
        values[TH_UiMode]      = modes[(int)m_ui_mode];
        if (m_ui_mode == UiMode::XMB) values[TH_UiMode] += std::string("  (") + T("recommended") + ")";
        values[TH_TextPos]     = aligns[(int)m_align];
        values[TH_ListIcons]   = m_list_icons ? T("On") : T("Off");
        values[TH_IconPack]    = (m_icon_pack_idx > 0 &&
                                  m_icon_pack_idx <= (int)m_icon_packs.size())
                               ? m_icon_packs[m_icon_pack_idx - 1] : T("Built-in");
        values[TH_Antialias]   = m_antialias ? T("On") : T("Off");
        values[TH_ShelfVert]   = m_shelf_vertical ? T("On") : T("Off");
        {
            char c[16];
            snprintf(c, sizeof(c), "%d", TileCols());
            values[TH_TileCols] = c;
            snprintf(c, sizeof(c), "%d", TileRowsVis());
            values[TH_TileRows] = c;
        }
        values[TH_Wrap]        = m_wrap_nav ? T("On") : T("Off");
        values[TH_Hints]       = m_show_hints ? T("On") : T("Off");
        values[TH_Counter]     = m_show_counter ? T("On") : T("Off");
        values[TH_Shortcuts]   = m_shortcuts_everywhere ? T("On") : T("Off");
        values[TH_RetroArch]   = m_retroarch ? T("On") : T("Off");
        values[TH_TdbRegion]   = kTdbRegions[std::clamp(m_tdb_region, 0, kTdbRegionCount - 1)];
        values[TH_Sgdb]        = m_sgdb_enabled ? T("On") : T("Off");
        values[TH_SgdbKey]     = SgdbKeyPresent() ? T("Set") : T("Not set");
        values[TH_Language]    = kLangs[m_lang_idx].name
                               ? kLangs[m_lang_idx].name : T("Automatic");
        values[TH_Music]       = m_music.Enabled() ? T("On") : T("Off");
        values[TH_Welcome]     = m_welcome_enabled ? T("On") : T("Off");
        values[TH_Updates]     = m_check_updates ? T("On") : T("Off");
        values[TH_About]       = m_upd_available ? T("Update available") : std::string("v") + SL_VERSION;
        {
            char c[32];
            snprintf(c, sizeof(c), "%d %s", m_widgets.Count(), T("found"));
            values[TH_Widgets] = c;
        }

        const std::vector<int> rows = ThemingRows();
        std::vector<std::string> vl, vv;
        int vis_cursor = 0;
        for (int i = 0; i < (int)rows.size(); i++) {
            if (rows[i] == m_theming_cursor) vis_cursor = i;
            vl.push_back(labels[rows[i]]);
            vv.push_back(values[rows[i]]);
        }

        DrawCarousel(vl, vv, vis_cursor, m_sub_scroll);
        DrawHint({ {{"up","down"}, "Select"}, {{"a"}, "Open"}, {{"left","right"}, "Change"}, {{"b"}, "Back"} });
    }

    // ---- Music player ------------------------------------------------------
    // Now playing on the left (cover, title, state), the whole library on the
    // right, a progress bar under both. The list cursor is independent of the
    // playing track: browse freely, A plays what is selected.
    Menu::Action Menu::OnButtonMusic(Btn b) {
        const int n = m_music.TrackCount();
        if (b == Btn::B) {
            m_screen = m_from_theming_music ? Screen::Theming : BackTarget();
            if (m_from_theming_music) m_sub_scroll = TH_Music;
            m_from_theming_music = false;
            return Action::None;
        }
        if (n > 0) {
            if (b == Btn::Down) m_music_cursor = (m_music_cursor + 1) % n;
            if (b == Btn::Up)   m_music_cursor = (m_music_cursor + n - 1) % n;
        }
        switch (b) {
            case Btn::A:
                if (n == 0) break;
                if (m_music_cursor == m_music.TrackIndex())
                    m_music.SetEnabled(!m_music.Enabled());   // play / pause
                else {
                    m_music.SetEnabled(true);
                    m_music.SelectTrack(m_music_cursor);
                }
                break;
            case Btn::Left:  m_music.Seek(m_music.Position() - 10.0); break;
            case Btn::Right: m_music.Seek(m_music.Position() + 10.0); break;
            case Btn::L:     m_music.Prev(); m_music_cursor = m_music.TrackIndex(); break;
            case Btn::R:     m_music.Next(); m_music_cursor = m_music.TrackIndex(); break;
            case Btn::X:     m_music.CycleRepeat(); break;
            case Btn::Y:     m_music.ToggleShuffle(); break;
            case Btn::Plus:  m_music.SetVolume(m_music.Volume() + 5); break;
            case Btn::Minus: m_music.SetVolume(m_music.Volume() - 5); break;
            default: break;
        }
        return Action::None;
    }
    void Menu::OpenMusicPlayer(bool from_theming) {
        m_screen = Screen::Music;
        m_from_theming_music = from_theming;
        m_music_cursor = m_music.TrackIndex();
        const int n = m_music.TrackCount();       // opens settled on what is playing
        m_music_scroll = (float)std::clamp(m_music_cursor - kMuRows / 2, 0, std::max(0, n - kMuRows));
    }
    SDL_Texture *Menu::MusicArt() {
        const int i = m_music.TrackCount() ? m_music.TrackIndex() : -1;
        if (i == m_music_art_idx) return m_music_art;
        if (m_music_art) { m_gfx->FreeImage(m_music_art); m_music_art = nullptr; }
        m_music_art_idx = i;
        const std::vector<u8> img = m_music.CoverArt(i);
        if (!img.empty()) m_music_art = m_gfx->LoadImageScaled(img.data(), img.size(), 360, 360);
        return m_music_art;
    }
    std::string Menu::FormatTime(double seconds) {
        const int t = (int)(seconds < 0.0 ? 0.0 : seconds);
        char buf[16];
        if (t >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
        else           snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
        return buf;
    }
    void Menu::DrawMusic() {
        const Theme &t = m_theme.Current();
        const int     W = gfx::Gfx::Width;
        SDL_Texture *art = MusicArt();
        if (art) m_gfx->DrawCover(art, 40);
        DrawTopBar("Music");

        const int n = m_music.TrackCount();
        if (n == 0) {
            m_gfx->TextCentered(FontSize::Normal, W / 2, 300, t.dim, T("No music found"));
            m_gfx->TextCentered(FontSize::Small, W / 2, 340, t.dim,
                                T("Put mp3/ogg files in sdmc:/slaunch/music"));
            DrawHint({ {{"b"}, "Back"} });
            return;
        }
        const int cur = m_music.TrackIndex();

        // ---- now playing ----
        m_gfx->FillRect(kMuArtX - 2, kMuArtY - 2, kMuArt + 4, kMuArt + 4, WithAlpha(t.dim, 60));
        if (art) {
            m_gfx->DrawImage(art, kMuArtX, kMuArtY, kMuArt, kMuArt, 255);
        } else {
            m_gfx->FillRect(kMuArtX, kMuArtY, kMuArt, kMuArt, WithAlpha(t.bg_bottom, 200));
            if (SDL_Texture *g = SystemIcon(ItemKind::MusicPlayer))
                m_gfx->DrawImageTinted(g, kMuArtX + kMuArt / 2 - 60, kMuArtY + kMuArt / 2 - 60,
                                       120, 120, IconTint(t, 255));
        }
        const int ty = kMuArtY + kMuArt + 18;
        m_gfx->Text(FontSize::Normal, kMuArtX, ty, t.title,
                    Ellipsize(m_music.CurrentName(), kMuListX - kMuArtX - 30, FontSize::Normal).c_str());
        static const char *kRepeat[] = { "Repeat all", "Repeat one", "Repeat off" };
        std::string state = std::string(m_music.Enabled() ? T("Playing") : T("Paused")) +
                            "  ·  " + T(kRepeat[m_music.RepeatMode()]);
        if (m_music.Shuffle()) state += std::string("  ·  ") + T("Shuffle");
        char vol[48];
        snprintf(vol, sizeof(vol), "%s %d%%", T("Volume"), m_music.Volume());
        const int sy = ty + m_gfx->LineHeight(FontSize::Normal) + 6;
        const int sw = kMuListX - kMuArtX - 30;
        m_gfx->Text(FontSize::Small, kMuArtX, sy, t.dim, Ellipsize(state, sw, FontSize::Small).c_str());
        m_gfx->Text(FontSize::Small, kMuArtX, sy + m_gfx->LineHeight(FontSize::Small) + 4, t.dim, vol);

        // ---- library ----
        // m_music_scroll is the top row, chasing whatever keeps the cursor
        // centred - clamped, so a short library starts at the top.
        const float top = (float)std::clamp(m_music_cursor - kMuRows / 2, 0, std::max(0, n - kMuRows));
        m_music_scroll += (top - m_music_scroll) * 0.30f;
        if (std::abs(top - m_music_scroll) < 0.01f) m_music_scroll = top;
        const int listW = W - 80 - kMuListX;
        const int first = std::max(0, (int)m_music_scroll - 1);
        const int last  = std::min(n - 1, (int)m_music_scroll + kMuRows + 1);
        for (int i = first; i <= last; i++) {
            const float d = (float)i - m_music_scroll;
            const int   y = kMuListY + (int)(d * kMuRowH);
            if (y < kMuListY - 4 || y > kMuListY + (kMuRows - 1) * kMuRowH + 4) continue;
            const Uint8 a = 255;
            const bool  sel = (i == m_music_cursor);
            if (sel) m_gfx->FillRect(kMuListX - 12, y, listW + 24, kMuRowH - 4, WithAlpha(t.accent, 40));
            if (i == cur) {                          // what is playing
                const int cy = y + (kMuRowH - 4) / 2;
                if (m_music.Enabled())
                    m_gfx->FillTriangle(kMuListX, cy - 7, kMuListX + 11, cy, kMuListX, cy + 7, WithAlpha(t.accent, a));
                else {
                    m_gfx->FillRect(kMuListX, cy - 7, 4, 14, WithAlpha(t.accent, a));
                    m_gfx->FillRect(kMuListX + 7, cy - 7, 4, 14, WithAlpha(t.accent, a));
                }
            }
            const double dur = m_music.Duration(i);
            const std::string dtxt = dur > 0.0 ? FormatTime(dur) : std::string();
            const int dw = m_gfx->TextWidth(FontSize::Small, dtxt.c_str());
            const int lh = m_gfx->LineHeight(FontSize::Small);
            const int yy = y + (kMuRowH - 4 - lh) / 2;
            m_gfx->Text(FontSize::Small, kMuListX + 22, yy,
                        WithAlpha(i == cur ? t.accent : (sel ? t.title : t.fg), a),
                        Ellipsize(m_music.TrackName(i), listW - 22 - dw - 20, FontSize::Small).c_str());
            if (!dtxt.empty())
                m_gfx->Text(FontSize::Small, kMuListX + listW - dw, yy, WithAlpha(t.dim, a), dtxt.c_str());
        }

        // ---- progress ----
        const double dur = m_music.Duration(cur);
        const double pos = std::min(m_music.Position(), dur > 0.0 ? dur : 1e9);
        const int barW = W - 80 - kMuArtX;
        m_gfx->FillRect(kMuArtX, kMuBarY, barW, 6, WithAlpha(t.dim, 70));
        if (dur > 0.0)
            m_gfx->FillRect(kMuArtX, kMuBarY, (int)(barW * (pos / dur)), 6, t.accent);
        const std::string el = FormatTime(pos), tot = dur > 0.0 ? FormatTime(dur) : std::string("--:--");
        const int lh = m_gfx->LineHeight(FontSize::Small);
        m_gfx->Text(FontSize::Small, kMuArtX, kMuBarY - lh - 6, t.fg, el.c_str());
        m_gfx->Text(FontSize::Small, kMuArtX + barW - m_gfx->TextWidth(FontSize::Small, tot.c_str()),
                    kMuBarY - lh - 6, t.dim, tot.c_str());

        DrawHint({ {{"a"}, m_music_cursor == cur && m_music.Enabled() ? "Pause" : "Play"},
                   {{"left","right"}, "Seek"}, {{"l","r"}, "Track"},
                   {{"x"}, "Repeat"}, {{"y"}, "Shuffle"}, {{"minus","plus"}, "Volume"},
                   {{"b"}, "Back"} });
    }
    // Touch: a track row plays it, the bar seeks, the cover plays / pauses.
    void Menu::OnTouchMusic(int x, int y) {
        const int n = m_music.TrackCount();
        if (n == 0) return;
        const int W = gfx::Gfx::Width;
        if (y >= kMuBarY - 20 && y <= kMuBarY + 26 && x >= kMuArtX && x <= W - 80) {
            const double dur = m_music.Duration(m_music.TrackIndex());
            if (dur > 0.0) m_music.Seek(dur * (x - kMuArtX) / (double)(W - 80 - kMuArtX));
            return;
        }
        if (x >= kMuArtX && x < kMuArtX + kMuArt && y >= kMuArtY && y < kMuArtY + kMuArt) {
            m_music.SetEnabled(!m_music.Enabled());
            return;
        }
        if (x >= kMuListX - 12 && y >= kMuListY && y < kMuListY + kMuRows * kMuRowH) {
            const int i = (int)lroundf(m_music_scroll) + (y - kMuListY) / kMuRowH;
            if (i < 0 || i >= n) return;
            m_music_cursor = i;
            OnButtonMusic(Btn::A);
        }
    }
    // ---- Homebrew (.nro) browser -------------------------------------------
    void Menu::LoadHbPins() {
        m_hb_pins.clear();
        FILE *fp = fopen(GetUserConfigPath("homebrew.txt").c_str(), "r");
        if (!fp) return;
        char line[FS_MAX_PATH + 2];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (!line[0]) continue;
            hb::HbEntry e;                 // fallback name until ResolvePins runs
            e.path = line;
            size_t slash = e.path.find_last_of('/');
            std::string base = (slash == std::string::npos) ? e.path : e.path.substr(slash + 1);
            size_t dot = base.find_last_of('.');
            e.name = (dot == std::string::npos) ? base : base.substr(0, dot);
            m_hb_pins.push_back(std::move(e));
        }
        fclose(fp);
    }
    // Give pinned .nro their real name + (cached) icon on the main menu without
    // opening the browser. Runs on a worker thread so it never sits on the menu
    // -start path: pins show their fallback (file-base) name for a moment, then
    // PollResolvePins folds the resolved names/icons in. Manifest-backed, so in
    // steady state the worker only stat()s each pin.
    void Menu::ResolvePinsTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        hb::Resolve(m->m_pin_result);
        m->m_pin_done.store(true, std::memory_order_release);
    }
    void Menu::StartResolvePins() {
        if (m_hb_pins.empty() || m_pin_running) return;
        m_pin_result = m_hb_pins;   // copy paths (+ fallback names) for the worker
        m_pin_done.store(false, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_pin_thread, &Menu::ResolvePinsTrampoline, this,
                                     nullptr, 0x20000, 0x3B, -2))) {
            threadStart(&m_pin_thread);
            m_pin_running = true;
        } else {
            hb::Resolve(m_hb_pins);   // fallback: synchronous
            RebuildItems();
        }
    }
    void Menu::PollResolvePins() {
        if (!m_pin_running || !m_pin_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_pin_thread);
        threadClose(&m_pin_thread);
        m_pin_running = false;
        // Fold resolved name/icon into the live pins by path (a merge, so a pin the
        // user toggled meanwhile isn't clobbered).
        for (auto &r : m_pin_result)
            for (auto &p : m_hb_pins)
                if (p.path == r.path) { p.name = r.name; p.icon_key = r.icon_key; break; }
        RebuildItems();
    }
    void Menu::SaveHbPins() {
        EnsureUserConfigDir();
        FILE *fp = fopen(GetUserConfigPath("homebrew.txt").c_str(), "w");
        if (!fp) return;
        for (auto &p : m_hb_pins) fprintf(fp, "%s\n", p.path.c_str());
        fclose(fp);
    }
    bool Menu::IsHbPinned(const std::string &path) const {
        for (auto &p : m_hb_pins) if (p.path == path) return true;
        return false;
    }
    void Menu::LoadHbDonor() {
        m_hb_donor = 0;
        FILE *fp = fopen("sdmc:/slaunch/config/hb_donor.txt", "r");
        if (!fp) return;
        char line[32];
        if (fgets(line, sizeof(line), fp)) m_hb_donor = strtoull(line, nullptr, 16);
        fclose(fp);
    }
    void Menu::SaveHbDonor() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen("sdmc:/slaunch/config/hb_donor.txt", "w");
        if (!fp) return;
        fprintf(fp, "%016llX\n", (unsigned long long)m_hb_donor);
        fclose(fp);
    }
    void Menu::ToggleHbPin(const std::string &path) {
        auto it = std::find_if(m_hb_pins.begin(), m_hb_pins.end(),
                               [&](const hb::HbEntry &e){ return e.path == path; });
        if (it != m_hb_pins.end()) {
            m_hb_pins.erase(it);
        } else {
            // Resolve now so the pinned entry gets its name + icon immediately.
            hb::HbEntry e;
            for (auto &h : m_hb) if (h.path == path) { e = h; break; }
            if (e.path.empty()) e = hb::ReadOne(path);
            m_hb_pins.push_back(std::move(e));
        }
        SaveHbPins();
        RebuildItems();
    }
    // Scan homebrew on a worker thread: parsing NROs + extracting icons off the SD
    // can take a second or two, and doing it inline froze the menu on open. The
    // browser shows "Scanning..." until PollHbScan swaps the finished list in.
    void Menu::HbScanTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        m->m_hb_scan_result = hb::Scan();
        m->m_shortcut_scan_result = hb::ScanShortcuts();
        m->m_hb_scan_done.store(true, std::memory_order_release);
    }
    // Swap in a scan's shortcuts and rebuild the key -> art-path map the icon
    // cache consults. Built here rather than looked up per draw so the cache's
    // resolver stays a hash lookup, which is what it is called on every visible
    // entry every frame.
    void Menu::AdoptShortcuts(std::vector<hb::Shortcut> &&found) {
        m_shortcuts = std::move(found);
        m_shortcut_art.clear();
        for (const auto &sc : m_shortcuts)
            if (!sc.icon_path.empty()) m_shortcut_art[sc.icon_key] = sc.icon_path;
    }
    void Menu::StartHbScan() {
        if (m_hb_scanned || m_hb_scan_running) return;
        m_hb_scan_done.store(false, std::memory_order_release);
        m_hb_scan_result.clear();
        m_shortcut_scan_result.clear();
        if (R_SUCCEEDED(threadCreate(&m_hb_thread, &Menu::HbScanTrampoline, this,
                                     nullptr, 0x20000, 0x3B, -2))) {
            threadStart(&m_hb_thread);
            m_hb_scan_running = true;
        } else {
            m_hb = hb::Scan();   // fallback: synchronous (may briefly stall)
            AdoptShortcuts(hb::ScanShortcuts());
            m_hb_scanned = true;
            RebuildItems();
        }
    }
    void Menu::PollHbScan() {
        if (!m_hb_scan_running || !m_hb_scan_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_hb_thread);
        threadClose(&m_hb_thread);
        m_hb_scan_running = false;
        m_hb = std::move(m_hb_scan_result);
        AdoptShortcuts(std::move(m_shortcut_scan_result));
        m_hb_scanned = true;
        if (m_hb_cursor >= (int)m_hb.size()) m_hb_cursor = m_hb.empty() ? 0 : (int)m_hb.size() - 1;
        RebuildItems();
    }
    void Menu::OpenHomebrewBrowser() {
        StartHbScan();   // background; browser shows "Scanning..." until it lands
        m_screen = Screen::Homebrew;
        m_hb_cursor = 0;
    }
    // ---- Album (screenshot browser) -----------------------------------------
    // The console files captures as
    // sdmc:/Nintendo/Album/<year>/<month>/<day>/<timestamp>-<id>.jpg (.mp4 for
    // clips, played through gfx::VideoPlayer). Walking that fixed
    // four-level shape with opendir is enough; there is no need for a general
    // recursive walker, and the depth cap means a stray directory cannot send us
    // wandering over the card.
    //
    // Scanning is synchronous, unlike the homebrew scan: this only ever stats
    // directory entries and never opens a file, so even a few thousand captures
    // cost a fraction of what parsing one NRO does.
    void Menu::ScanAlbum() {
        if (m_album_scanned) return;
        m_album_scanned = true;
        m_album.clear();

        auto entries = [](const std::string &dir, std::vector<std::string> &out, bool files) {
            DIR *d = opendir(dir.c_str());
            if (!d) return;
            while (struct dirent *e = readdir(d)) {
                if (e->d_name[0] == '.') continue;
                const bool is_dir = (e->d_type == DT_DIR);
                if (is_dir != !files) continue;
                out.push_back(dir + "/" + e->d_name);
            }
            closedir(d);
            std::sort(out.begin(), out.end());
        };

        std::vector<std::string> years, months, days;
        entries("sdmc:/Nintendo/Album", years, false);
        for (const auto &y : years) {
            months.clear();
            entries(y, months, false);
            for (const auto &m : months) {
                days.clear();
                entries(m, days, false);
                for (const auto &d : days) {
                    std::vector<std::string> shots;
                    entries(d, shots, true);
                    for (auto &s : shots) {
                        const char *ext = s.c_str() + s.size() - 4;
                        if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".mp4") == 0)
                            m_album.push_back(std::move(s));
                    }
                }
            }
        }
        // Newest first: the path shape sorts chronologically, so one reverse
        // puts the most recent capture under the cursor on open, which is what
        // you almost always want.
        std::reverse(m_album.begin(), m_album.end());
    }
    void Menu::FreeAlbumTexture() {
        if (m_album_tex) m_gfx->FreeImage(m_album_tex);
        m_album_tex     = nullptr;
        m_album_tex_idx = -1;
    }
    // Decode the selected capture, dropping the previous one first. Exactly one
    // screenshot is resident at a time - see the note in the header.
    void Menu::EnsureAlbumTexture() {
        if (m_album.empty()) { FreeAlbumTexture(); return; }
        if (m_album_cursor < 0 || m_album_cursor >= (int)m_album.size()) return;
        if (m_album_tex_idx == m_album_cursor && m_album_tex) return;

        FreeAlbumTexture();
        // Clips have no still to decode; the panel previews them live instead.
        if (IsVideoPath(m_album[m_album_cursor].c_str())) {
            m_album_tex_idx = m_album_cursor;
            return;
        }
        m_album_tex     = m_gfx->LoadImage(m_album[m_album_cursor].c_str());
        m_album_tex_idx = m_album_cursor;   // cached even on failure, so a
                                            // corrupt capture is not retried
                                            // every frame
    }
    void Menu::OpenAlbumViewer() {
        // Rescan on every open so captures taken since the menu started show up.
        // The scan only stats directory entries, so this is cheap enough to do
        // unconditionally rather than trying to watch the tree for changes.
        m_album_scanned = false;
        ScanAlbum();
        m_screen       = Screen::Album;
        m_album_cursor = 0;
        m_album_scroll = 0.0f;   // opens settled, not mid-slide
        m_album_full   = false;
        StopAlbumVideo();        // a clip from a previous visit is not left playing
        FreeAlbumTexture();
    }
    // Clip playback: the selection is played in the menu on its own VideoPlayer
    // (the wallpaper's m_video_player keeps running underneath it), with the
    // background music muted for the clip's duration. PlayAlbumVideo mutes the
    // music only once the clip has actually opened - on failure the music is
    // left exactly as it was. StopAlbumVideo restores the remembered state, so
    // a user who had music off does not wake it up by stopping a clip.
    // Full-screen playback, with sound. Music is paused for the clip and put
    // back as it was by StopAlbumVideo - remembered only on the first clip of
    // a run, so stepping clip to clip does not overwrite it with "off".
    bool Menu::PlayAlbumVideo(const std::string &path) {
        if (!m_album_video_active) m_album_music_was_on = m_music.Enabled();
        m_album_video.Close();
        m_album_video_path = path;
        // False on the no-decoder stub (e.g. the Windows simulator), where
        // nothing can be played at all - the caller shows a status hint.
        m_album_video_ok = m_album_video.Open(m_gfx, path.c_str(), false, true);
        if (!m_album_video_ok) {
            if (m_album_video_active) StopAlbumVideo();
            return false;
        }
        m_album_video_active = true;
        m_music.SetEnabled(false);   // pauses, keeping the play position
        return true;
    }
    // Silent, looping, for the list's preview panel. The path is recorded
    // even when the open fails, so a broken clip is tried once, not per frame.
    void Menu::PreviewAlbumVideo(const std::string &path) {
        if (m_album_video_path == path) return;
        m_album_video.Close();
        m_album_video_path = path;
        m_album_video_ok   = m_album_video.Open(m_gfx, path.c_str());
    }
    // Closes the preview too; only a playing clip has music to give back.
    void Menu::StopAlbumVideo() {
        m_album_video.Close();       // idempotent; joins the decode thread
        m_album_video_path.clear();
        m_album_video_ok = false;
        if (!m_album_video_active) return;
        m_album_video_active = false;
        m_album_full = false;        // stopping a clip lands back on the list
        m_music.SetEnabled(m_album_music_was_on);
    }
    // Put capture i on the full screen viewer: a clip starts playing, a
    // photo just shows (EnsureAlbumTexture decodes it on the next draw).
    void Menu::AlbumShow(int i) {
        const int n = (int)m_album.size();
        if (n == 0) return;
        m_album_cursor = (i % n + n) % n;
        m_album_scroll = (float)m_album_cursor;
        m_album_slide_tick = armGetSystemTick();
        const std::string &p = m_album[m_album_cursor];
        if (IsVideoPath(p.c_str())) {
            if (!PlayAlbumVideo(p)) SetStatus(T("Could not play that clip"));
        } else {
            StopAlbumVideo();
        }
        m_album_full = true;
        AlbumPokeUi();
    }
    void Menu::AlbumPokeUi() { m_album_ui_tick = armGetSystemTick(); }

    Menu::Action Menu::OnButtonAlbum(Btn b) {
        const int n = (int)m_album.size();

        // ---- full screen viewer ----
        if (m_album_full) {
            AlbumPokeUi();
            const bool clip = m_album_video_active;
            switch (b) {
                case Btn::B:
                    StopAlbumVideo();
                    m_album_full = false;
                    m_album_slideshow = false;
                    return Action::None;
                case Btn::Y:
                    StopAlbumVideo(); FreeAlbumTexture();
                    return Action::OpenAlbum;
                case Btn::L: AlbumShow(m_album_cursor - 1); return Action::None;
                case Btn::R: AlbumShow(m_album_cursor + 1); return Action::None;
                case Btn::Left:
                    if (clip) m_album_video.Seek(m_album_video.Position() - 5.0);
                    else      AlbumShow(m_album_cursor - 1);
                    return Action::None;
                case Btn::Right:
                    if (clip) m_album_video.Seek(m_album_video.Position() + 5.0);
                    else      AlbumShow(m_album_cursor + 1);
                    return Action::None;
                case Btn::A:
                    if (clip && m_album_video.Ended()) {        // play it again
                        m_album_video.Seek(0.0);
                        m_album_video.SetPaused(false);
                    } else if (clip) {
                        m_album_video.SetPaused(!m_album_video.Paused());
                    } else {
                        m_album_ui_pinned = !m_album_ui_pinned;  // photo: info on/off
                    }
                    return Action::None;
                case Btn::X:
                    m_album_slideshow = !m_album_slideshow;
                    m_album_slide_tick = armGetSystemTick();
                    SetStatus(m_album_slideshow ? "Slideshow on" : "Slideshow off");
                    return Action::None;
                default: return Action::None;
            }
        }

        // ---- list ----
        if (b == Btn::B) {
            StopAlbumVideo();        // the list's preview, if one is running
            FreeAlbumTexture();      // don't hold a capture open in the menu
            m_screen = BackTarget();
            return Action::None;
        }
        // Y hands off to the console's own Album applet, which is the only way
        // to reach sharing and deletion. That closes the menu outright (see
        // main.cpp), and Music::Exit persists its state for the next launch.
        if (b == Btn::Y) { StopAlbumVideo(); FreeAlbumTexture(); return Action::OpenAlbum; }
        if (n == 0) return Action::None;

        const int was = m_album_cursor;
        if (b == Btn::Down || b == Btn::Right) m_album_cursor = (m_album_cursor + 1) % n;
        if (b == Btn::Up   || b == Btn::Left)  m_album_cursor = (m_album_cursor + n - 1) % n;
        if (b == Btn::A) { AlbumShow(m_album_cursor); return Action::None; }
        if (b == Btn::X) {                      // slideshow from the selection
            m_album_slideshow = true;
            AlbumShow(m_album_cursor);
            return Action::None;
        }

        // Wrapping the ends is a jump, not a scroll: animating it would drag the
        // list past every capture in the library. Snap the animation to the new
        // position so it starts from there instead.
        if (std::abs(m_album_cursor - was) > 1)
            m_album_scroll = (float)m_album_cursor;
        return Action::None;
    }

    // "2026092312000000-ABCDEF.jpg" -> "2026-09-23  12:00:00". The console
    // names every capture by its timestamp, so this is the capture's date.
    static std::string CaptureDate(const std::string &path) {
        const size_t slash = path.find_last_of('/');
        const std::string f = (slash == std::string::npos) ? path : path.substr(slash + 1);
        if (f.size() < 14 || f.find_first_not_of("0123456789") < 14) return f;
        return f.substr(0, 4) + "-" + f.substr(4, 2) + "-" + f.substr(6, 2) + "  " +
               f.substr(8, 2) + ":" + f.substr(10, 2) + ":" + f.substr(12, 2);
    }

    // The viewer's info bar: date and position, and for a clip the transport.
    // It fades in on any input and out a few seconds later, and stays while a
    // clip is paused or a photo has it pinned.
    void Menu::DrawAlbumViewerUi() {
        const Theme &t = m_theme.Current();
        const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
        const bool clip = m_album_video_active;
        const u64 ms = (armGetSystemTick() - m_album_ui_tick) * 1000 / armGetSystemTickFreq();
        const bool held = clip ? (m_album_video.Paused() || m_album_video.Ended()) : m_album_ui_pinned;
        float vis = held ? 1.0f : (ms < 2500 ? 1.0f : std::max(0.0f, 1.0f - (ms - 2500) / 400.0f));
        if (vis <= 0.0f) return;
        const Uint8 a = (Uint8)(255 * vis);

        // Tall enough that its rows sit above the hint bar and the touch
        // Back button, which share the bottom edge with it.
        const int band = clip ? 160 : 120;
        m_gfx->FillRect(0, H - band, W, band, SDL_Color{ 0, 0, 0, (Uint8)(160 * vis) });
        const int lh = m_gfx->LineHeight(FontSize::Small);
        const int y0 = H - band + 14;
        m_gfx->Text(FontSize::Small, 40, y0, WithAlpha(t.fg, a),
                    CaptureDate(m_album[m_album_cursor]).c_str());
        char pos[32];
        snprintf(pos, sizeof(pos), "%d / %d", m_album_cursor + 1, (int)m_album.size());
        std::string right = pos;
        if (m_album_slideshow) right = std::string(T("Slideshow")) + "   " + right;
        m_gfx->Text(FontSize::Small, W - 40 - m_gfx->TextWidth(FontSize::Small, right.c_str()), y0,
                    WithAlpha(t.dim, a), right.c_str());

        if (clip) {
            const double dur = m_album_video.Duration(), p = m_album_video.Position();
            const int bx = 40, bw = W - 80, by = y0 + lh + 14;
            m_gfx->FillRect(bx, by, bw, 6, SDL_Color{ 255, 255, 255, (Uint8)(60 * vis) });
            if (dur > 0.0) m_gfx->FillRect(bx, by, (int)(bw * std::min(1.0, p / dur)), 6, WithAlpha(t.accent, a));
            const std::string tm = FormatTime(p) + " / " + FormatTime(dur);
            m_gfx->Text(FontSize::Small, bx + 30, by + 12, WithAlpha(t.fg, a), tm.c_str());
            const int cx = bx + 8, cy = by + 12 + lh / 2;
            if (m_album_video.Paused() || m_album_video.Ended())
                m_gfx->FillTriangle(cx, cy - 8, cx + 14, cy, cx, cy + 8, WithAlpha(t.fg, a));
            else {
                m_gfx->FillRect(cx, cy - 8, 5, 16, WithAlpha(t.fg, a));
                m_gfx->FillRect(cx + 9, cy - 8, 5, 16, WithAlpha(t.fg, a));
            }
            DrawStatusHint({ {{"a"}, m_album_video.Paused() || m_album_video.Ended() ? "Play" : "Pause"},
                             {{"left","right"}, "Seek"}, {{"l","r"}, "Previous / next"},
                             {{"x"}, "Slideshow"}, {{"b"}, "Back"} });
        } else {
            DrawStatusHint({ {{"left","right"}, "Previous / next"}, {{"a"}, "Info"},
                             {{"x"}, "Slideshow"}, {{"b"}, "Back"}, {{"y"}, "System album"} });
        }
    }

    // Browser: the list of captures on the left, a preview of the selected one
    // on the right, in the same left-anchored arrangement as the rest of XMB.
    // A opens the full screen viewer.
    void Menu::DrawAlbum() {
        const Theme &t = m_theme.Current();
        const int     W = gfx::Gfx::Width;
        const int     H = gfx::Gfx::Height;

        if (m_album_full && !m_album.empty()) {
            // Slideshow: photos hold for a few seconds, clips play out.
            if (m_album_slideshow) {
                const u64 ms = (armGetSystemTick() - m_album_slide_tick) * 1000 / armGetSystemTickFreq();
                if (m_album_video_active ? m_album_video.Ended() : ms >= 4000)
                    AlbumShow(m_album_cursor + 1);
            }
            m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, 255 });
            if (m_album_video_active) {
                m_album_video.Tick();
                if (SDL_Texture *vt = m_album_video.GetTexture()) m_gfx->DrawCover(vt, 255);
            } else {
                EnsureAlbumTexture();
                if (m_album_tex) m_gfx->DrawCover(m_album_tex, 255);
            }
            DrawAlbumViewerUi();
            return;
        }

        EnsureAlbumTexture();

        DrawTopBar("Album");

        if (m_album.empty()) {
            m_gfx->TextCentered(FontSize::Normal, W / 2, H / 2 - 20, t.dim,
                                T("No screenshots or clips found"));
            DrawStatusHint({ {{"b"}, "Back"}, {{"y"}, "System album"} });
            return;
        }

        // Chase the cursor rather than jumping to it, at the same rate the other
        // lists use, and settle exactly so it does not creep forever.
        m_album_scroll += ((float)m_album_cursor - m_album_scroll) * 0.30f;
        if (std::abs((float)m_album_cursor - m_album_scroll) < 0.01f)
            m_album_scroll = (float)m_album_cursor;

        // Preview panel on the right, sized and placed like the XMB thumbnail.
        const int pw = (int)(W * 0.46f);
        const int ph = pw * 9 / 16;
        const int px = W - pw - 40;
        const int py = (H - ph) / 2;
        m_gfx->FillRect(px - 2, py - 2, pw + 4, ph + 4, WithAlpha(t.dim, 60));

        // A clip previews live, silently, once the list has come to rest on it
        // - opening a decoder for every row scrolled past would stall the list.
        const bool clip = IsVideoPath(m_album[m_album_cursor].c_str());
        if (!clip)                                           StopAlbumVideo();
        else if (m_album_scroll == (float)m_album_cursor)    PreviewAlbumVideo(m_album[m_album_cursor]);
        SDL_Texture *pv = m_album_tex;
        if (clip && m_album_video_path == m_album[m_album_cursor]) {
            m_album_video.Tick();
            pv = m_album_video.GetTexture();
        }
        if (pv) m_gfx->DrawImage(pv, px, py, pw, ph, 255);
        else    m_gfx->FillRect(px, py, pw, ph, WithAlpha(t.bg_bottom, 200));
        if (clip) {   // play badge, so a clip reads as one even before it decodes
            const int cx = px + pw / 2, cy = py + ph / 2, s = 22;
            m_gfx->FillRect(cx - s - 12, cy - s - 8, 2 * s + 24, 2 * s + 16,
                            SDL_Color{ 0, 0, 0, 110 });
            m_gfx->FillTriangle(cx - s / 2, cy - s, cx + s, cy, cx - s / 2, cy + s,
                                WithAlpha(t.title, 220));
        }

        // The list, using the XMB placement curve so it matches every other
        // screen while XMB is the active layout.
        const int   rows   = 9;
        const int   pitch  = 44;
        const int   cy0    = H / 2 - (rows / 2) * pitch;
        const int   listX  = 56;
        const int   listW  = px - 40 - listX;
        const int   n      = (int)m_album.size();

        // One extra row each way: at rest they sit off-screen, and during a
        // scroll they are what slides in rather than popping into place.
        for (int off = -(rows / 2) - 1; off <= rows / 2 + 1; off++) {
            const int i = (int)lroundf(m_album_scroll) + off;
            if (i < 0 || i >= n) continue;

            // Distance from the cursor in animated space, so brightness, size
            // and position all move together instead of snapping on the frame
            // the index changes.
            const float d   = (float)i - m_album_scroll;
            const bool  sel = (i == m_album_cursor);
            const int   y   = cy0 + (int)((d + rows / 2) * pitch);
            if (y < 60 || y > H - 60) continue;
            const Uint8 a   = (Uint8)std::max(60.0f, 255.0f - std::abs(d) * 42.0f);

            // Show the capture's own file name, which is its timestamp.
            const std::string &p = m_album[i];
            const size_t slash   = p.find_last_of('/');
            std::string  name    = (slash == std::string::npos) ? p : p.substr(slash + 1);
            if (name.size() > 4) name.resize(name.size() - 4);   // drop ".jpg"/".mp4"

            const FontSize fs = sel ? FontSize::Normal : FontSize::Small;
            const int      lh = m_gfx->LineHeight(fs);
            if (sel) {
                const int s = 9;
                m_gfx->FillTriangle(listX - 22, y - s, listX - 13, y,
                                    listX - 22, y + s, WithAlpha(t.accent, a));
            }
            const bool        vid   = IsVideoPath(p.c_str());
            const std::string shown = Ellipsize(name, listW - (vid ? 24 : 0), fs);
            m_gfx->Text(fs, listX, y - lh / 2,
                        WithAlpha(sel ? t.title : t.fg, a), shown.c_str());
            if (vid) {   // small play mark after the name
                const int mx = listX + m_gfx->TextWidth(fs, shown.c_str()) + 10;
                m_gfx->FillTriangle(mx, y - 6, mx + 10, y, mx, y + 6, WithAlpha(t.accent, a));
            }
        }

        char pos[32];
        snprintf(pos, sizeof(pos), "%d/%d", m_album_cursor + 1, n);
        // Position counters are optional; blanking the string here keeps
        // the layout arithmetic below untouched.
        if (!m_show_counter) pos[0] = '\0';
        const int pwid = m_gfx->TextWidth(FontSize::Small, pos);
        m_gfx->Text(FontSize::Small, W - 8 - pwid,
                    H - 8 - m_gfx->LineHeight(FontSize::Small), t.dim, pos);

        DrawStatusHint({ {{"a"}, clip ? "Play" : "View"}, {{"x"}, "Slideshow"}, {{"b"}, "Back"},
                         {{"y"}, "System album"} });
    }
    // Touch. Viewer: the outer thirds step through the album, the middle
    // plays / pauses (or shows the info), the bar seeks. List: a row opens.
    void Menu::OnTouchAlbum(int x, int y) {
        const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
        if (m_album.empty()) return;
        if (m_album_full) {
            if (m_album_video_active && y > H - 125 && y < H - 90 && m_album_video.Duration() > 0.0) {
                m_album_video.Seek(m_album_video.Duration() * std::max(0, x - 40) / (double)(W - 80));
                AlbumPokeUi();
            } else if (x < W / 3)      OnButtonAlbum(Btn::L);
            else if (x > W * 2 / 3)    OnButtonAlbum(Btn::R);
            else                       OnButtonAlbum(Btn::A);
            return;
        }
        const int pitch = 44, cy0 = H / 2 - 4 * pitch;     // DrawAlbum's list rows
        if (x < (int)(W * 0.54f)) {
            const int i = (int)lroundf(m_album_scroll) + (int)lroundf((float)(y - cy0) / pitch) - 4;
            if (i >= 0 && i < (int)m_album.size()) {
                if (i == m_album_cursor) AlbumShow(i);
                else                     m_album_cursor = i;
            }
        } else {
            AlbumShow(m_album_cursor);   // the preview opens the selection
        }
    }
    Menu::Action Menu::OnButtonHomebrew(Btn b) {
        const int n = (int)m_hb.size();
        if (b == Btn::B) { m_screen = Screen::Main; return Action::None; }
        if (n == 0) return Action::None;
        if (b == Btn::Down) m_hb_cursor = (m_hb_cursor + 1) % n;
        if (b == Btn::Up)   m_hb_cursor = (m_hb_cursor + n - 1) % n;
        if (b == Btn::A) {   // launch: as an application if a donor is set, else applet
            m_hb_launch_path = m_hb[m_hb_cursor].path;
            return m_hb_donor ? Action::LaunchHomebrewApp : Action::LaunchHomebrew;
        }
        if (b == Btn::Y) {   // force applet mode (fallback when app mode misbehaves)
            m_hb_launch_path = m_hb[m_hb_cursor].path;
            return Action::LaunchHomebrew;
        }
        if (b == Btn::X) {   // pin / unpin from the main menu
            ToggleHbPin(m_hb[m_hb_cursor].path);
            SetStatus(IsHbPinned(m_hb[m_hb_cursor].path) ? "Pinned to menu" : "Unpinned");
        }
        return Action::None;
    }
    void Menu::DrawHomebrew() {
        const Theme &t = m_theme.Current();
        DrawTopBar("Homebrew");
        if (m_hb_scan_running) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 320, t.dim,
                                T("Scanning homebrew..."));
            DrawHint({ {{"b"}, "Back"} });
            return;
        }
        if (m_hb.empty()) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 320, t.dim,
                                T("No .nro found in sdmc:/switch"));
            DrawHint({ {{"b"}, "Back"} });
            return;
        }
        m_hb_icons.SetScale(gfx::IconCache::GridScale);

        const int rowH = 84, top = 118, visible = 6;
        int start = std::max(0, m_hb_cursor - visible / 2);
        if (start + visible > (int)m_hb.size()) start = std::max(0, (int)m_hb.size() - visible);
        for (int i = 0; i < visible && start + i < (int)m_hb.size(); i++) {
            const int idx = start + i;
            const hb::HbEntry &h = m_hb[idx];
            const int y = top + i * rowH;
            const bool sel = (idx == m_hb_cursor);
            if (sel) m_gfx->FillRect(60, y - 6, gfx::Gfx::Width - 120, rowH - 12, WithAlpha(t.accent, 40));
            SDL_Texture *ic = h.icon_key ? m_hb_icons.Get(h.icon_key) : nullptr;
            if (ic) m_gfx->DrawImage(ic, 80, y, 64, 64, 255);
            else    m_gfx->FillRect(80, y, 64, 64, WithAlpha(t.bg_bottom, 180));
            m_gfx->Text(FontSize::Normal, 164, y + 16, sel ? t.accent : t.fg, h.name.c_str());
            if (IsHbPinned(h.path))
                m_gfx->Text(FontSize::Small, gfx::Gfx::Width - 210, y + 22, t.accent, T("pinned"));
        }
        char pos[28];
        snprintf(pos, sizeof(pos), "%d / %d", m_hb_cursor + 1, (int)m_hb.size());
        // Position counters are optional; blanking the string here keeps
        // the layout arithmetic below untouched.
        if (!m_show_counter) pos[0] = '\0';
        m_gfx->Text(FontSize::Small, gfx::Gfx::Width - 150, 74, t.dim, pos);
        DrawHint(m_hb_donor ? "A: Run as app   Y: Applet mode   X: Pin   B: Back"
                            : "A: Launch   X: Pin to menu   B: Back");
    }
    // ---- Widgets submenu drawing -------------------------------------------
    void Menu::DrawWidgets() {
        const Theme &t = m_theme.Current();
        DrawTopBar("Widgets");

        const int n = m_widgets.Count();
        if (n == 0) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 320, t.dim,
                                T("No widgets found"));
            m_gfx->TextCentered(FontSize::Small, gfx::Gfx::Width / 2, 372, t.dim,
                                T("Drop .lua widgets in sdmc:/slaunch/widgets/"));
            DrawHint({ {{"b"}, "Back"} });
            return;
        }

        std::vector<std::string> labels, values;
        for (int i = 0; i < n; i++) {
            widgets::IWidget *w = m_widgets.At(i);
            labels.push_back(w ? w->Name() : "Widget");
            values.push_back(m_widgets.IsEnabled(i) ? T("On") : T("Off"));
        }
        DrawCarousel(labels, values, m_widget_cursor, m_sub_scroll);
        DrawHint({ {{"left","right"}, "On/Off"}, {{"a"}, "Configure"}, {{"b"}, "Back"} });
    }
    void Menu::DrawWidgetOptions() {
        const Theme &t = m_theme.Current();
        widgets::IWidget *w = m_widgets.At(m_widget_sel);
        DrawTopBar(w ? w->Name().c_str() : "Widget");

        const int n = w ? w->OptionCount() : 0;
        if (n == 0) {
            m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 340, t.dim,
                                T("This widget has no options"));
            DrawHint({ {{"b"}, "Back"} });
            return;
        }

        std::vector<std::string> labels, values;
        for (int i = 0; i < n; i++) {
            labels.push_back(w->OptionLabel(i));
            if (w->OptionType(i) == "bool") {
                values.push_back(w->OptionValue(i) == "1" ? T("On") : T("Off"));
            } else {
                std::string v = w->OptionValue(i);
                values.push_back(v.empty() ? T("(not set)") : v);
            }
        }
        DrawCarousel(labels, values, m_widgetopt_cursor, m_sub_scroll);
        DrawHint({ {{"a"}, "Edit/Toggle"}, {{"left","right"}, "Toggle"}, {{"b"}, "Back"} });
    }
    void Menu::DrawThemes() {
        DrawTopBar("Themes");
        std::vector<std::string> labels, values;
        for (int i = 0; i < m_theme.Count(); i++) {
            labels.push_back(m_theme.At(i).name);
            values.push_back(i == m_theme.AppliedIndex() ? T("current") : "");
        }
        labels.push_back(T("+ New custom theme"));
        values.push_back("");
        DrawCarousel(labels, values, m_theme_cursor, m_sub_scroll);

        if (m_theme_cursor == m_theme.Count())
            DrawHint({ {{"a"}, "Create new theme"}, {{"b"}, "Back"} });
        else if (m_theme.IsCustom(m_theme_cursor))
            DrawHint({ {{"a"}, "Apply"}, {{"y"}, "Edit"}, {{"b"}, "Back"} });
        else
            DrawHint({ {{"a"}, "Apply"}, {{"b"}, "Back"} });
    }
    void Menu::DrawEditor() {
        const Theme &t = m_theme.Current(); // the edited theme, shown live
        if (!m_theme.IsCustom(m_editing_theme)) return;
        Theme &c = m_theme.CustomAt(m_editing_theme);
        DrawTopBar(c.name);

        const char *labels[EF_Count] = {
            T("Background"), T("Photo"), T("Dim"), T("Blur"),
            T("Blur radius"), T("Snow"),
            T("Gradient top"), T("Gradient bottom"), T("Text"),
            T("Accent"), T("Secondary"), T("Title"),
            T("Icon colour"), T("Icon background"),
            T("Icon background opacity"),
            T("Effect colour"), T("Effect colour 2"),
            T("Wave lines"), T("Wave thickness"), T("Wave amplitude"),
            T("Ribbon seed"), T("Ribbon layers"), T("Ribbon Y"), T("Sun X"),
            T("Camera height"),
            T("Rename theme"), T("Save & Apply"), T("Delete theme")
        };

        // The six shared knobs mean different things per background, so the
        // rows say what they do here rather than always naming the ribbon.
        switch (c.background_style) {
            case BackgroundStyle_Grid:
                labels[EF_RibbonLines]     = T("Grid lines");
                labels[EF_FxCam]           = T("Camera height");
                labels[EF_RibbonThickness] = T("Line width");
                labels[EF_RibbonAmplitude] = T("Scroll speed");
                labels[EF_RibbonSeed]      = T("Hills");
                labels[EF_RibbonLayers]    = T("Sun size");
                labels[EF_RibbonYCenter]   = T("Horizon");
                break;
            case BackgroundStyle_Stars:
                labels[EF_RibbonLines]     = T("Star density");
                labels[EF_RibbonThickness] = T("Star size");
                labels[EF_RibbonAmplitude] = T("Twinkle speed");
                labels[EF_RibbonSeed]      = T("Sky");
                labels[EF_RibbonLayers]    = T("Flare size");
                labels[EF_RibbonYCenter]   = T("Horizon");
                break;
            case BackgroundStyle_Aurora:
                labels[EF_RibbonLines]     = T("Curtains");
                labels[EF_RibbonThickness] = T("Curtain width");
                labels[EF_RibbonAmplitude] = T("Sway");
                labels[EF_RibbonSeed]      = T("Sky");
                labels[EF_RibbonLayers]    = T("Softness");
                labels[EF_RibbonYCenter]   = T("Hem height");
                break;
            case BackgroundStyle_Ocean:
                labels[EF_RibbonLines]     = T("(unused)");
                labels[EF_RibbonThickness] = T("Swell");
                labels[EF_RibbonAmplitude] = T("Speed");
                labels[EF_RibbonSeed]      = T("(unused)");
                labels[EF_RibbonLayers]    = T("Wave density");
                labels[EF_RibbonYCenter]   = T("Horizon");
                labels[EF_FxX]             = T("Reflection");
                break;
            default: break;   // Ribbon and Ribbon glow keep the wave labels
        }

        // Build the visible row list (the six are hidden when bg has no knobs).
        int vis_ids[EF_Count];
        int vis_n = 0;
        int cursor_vis = 0;
        for (int i = 0; i < EF_Count; i++) {
            if (IsRibbonRow(i) && !StyleHasParams(c.background_style))
                continue;
            if (i == EF_FxX && !StyleHasFxX(c.background_style))
                continue;
            if (i == EF_FxCam && !StyleHasFxCam(c.background_style))
                continue;
            if (IsFxColourRow(i) && !StyleHasParams(c.background_style))
                continue;
            if (IsBlurRadiusRow(i) && !c.wallpaper_blur)
                continue;
            if (i == m_edit_cursor) cursor_vis = vis_n;
            vis_ids[vis_n++] = i;
        }

        // Smooth scroll
        m_edit_scroll += (cursor_vis - m_edit_scroll) * 0.30f;
        if (std::abs(cursor_vis - m_edit_scroll) < 0.01f) m_edit_scroll = (float)cursor_vis;

        const int center_y = 360, spacing = 48, span = 7;
        for (int off = -span; off <= span; off++) {
            const int vi = (int)lroundf(m_edit_scroll) + off;
            if (vi < 0 || vi >= vis_n) continue;

            const int i    = vis_ids[vi];
            const float vdist = std::abs((float)vi - m_edit_scroll);
            const bool big   = vdist < 0.5f;
            const FontSize fs = big ? FontSize::Large : FontSize::Normal;
            const Uint8 alpha = (Uint8)std::max(24.0f, 255.0f - vdist * 52.0f);
            const int lh = m_gfx->LineHeight(fs);
            const int y  = center_y + (int)((vi - m_edit_scroll) * spacing) - lh / 2;
            if (y < 90 || y > kHintY - 30) continue;

            const bool sel = (i == m_edit_cursor);
            // Blur cannot apply to a playing video (see EnsureWallpaper) -
            // the row stays in the list (so the user's saved choice is not
            // hidden), but greys out to show it is not doing anything.
            const bool blur_disabled = (i == EF_WallpaperBlur) && IsVideoPath(c.wallpaper);
            const SDL_Color rc = (i == EF_Delete) ? WithAlpha(SDL_Color{235, 90, 90, 255}, alpha)
                                : blur_disabled    ? WithAlpha(t.dim, alpha)
                                                  : WithAlpha(sel ? t.accent : t.fg, alpha);
            m_gfx->Text(fs, kListX, y, rc, labels[i]);

            // Value column
            const int vx = 720;
            if (SDL_Color *col = EditorColor(c, i)) {
                m_gfx->FillRect(vx, y + (lh - 32) / 2, 60, 32, *col);
                char hex[16];
                snprintf(hex, sizeof(hex), "#%02X%02X%02X", col->r, col->g, col->b);
                m_gfx->Text(FontSize::Small, vx + 78, y + 4, WithAlpha(t.dim, alpha), hex);
            } else if (i == EF_Background) {
                const char *bg = BackgroundName(c.background_style);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), bg);
            } else if (i == EF_Wallpaper) {
                const char *wp_label;
                if (c.wallpaper[0]) {
                    const char *slash = strrchr(c.wallpaper, '/');
                    wp_label = slash ? slash + 1 : c.wallpaper;
                } else {
                    wp_label = T("(none)");
                }
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), wp_label);
            } else if (i == EF_WallpaperDim || i == EF_WallpaperBlur || i == EF_WallpaperSnow) {
                // Show On/Off toggle. A disabled Blur row shows its saved
                // value (not forced Off) dimmed, same as its label above.
                int val = (i == EF_WallpaperDim) ? c.wallpaper_dim
                       : (i == EF_WallpaperBlur) ? c.wallpaper_blur
                       : c.wallpaper_snow;
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(blur_disabled ? t.dim : t.fg, alpha),
                            val ? T("On") : T("Off"));
            } else if (i == EF_WallpaperBlurRadius) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.wallpaper_blur_radius);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_IconBgAlpha) {
                // Shown as a percentage and previewed as a swatch of the plate
                // colour at that opacity over the live background, so the effect
                // is visible without leaving the editor.
                char val[16];
                snprintf(val, sizeof(val), "%d%%", (c.icon_bg_alpha * 100 + 127) / 255);
                m_gfx->FillRect(vx, y + (lh - 32) / 2, 60, 32,
                                WithAlpha(c.icon_bg, (Uint8)c.icon_bg_alpha));
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Small, vx + 78, y + 4, WithAlpha(t.dim, alpha), val);
            } else if (i == EF_FxCam) {
                char val[16];
                snprintf(val, sizeof(val), "%d%%", c.Fx().cam);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_FxX) {
                char val[16];
                snprintf(val, sizeof(val), "%d%%", c.Fx().x);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonLines) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.Fx().lines);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonThickness) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.Fx().thickness);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonAmplitude) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.Fx().amplitude);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonSeed) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.Fx().seed);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonLayers) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.Fx().layers);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            } else if (i == EF_RibbonYCenter) {
                char val[16];
                snprintf(val, sizeof(val), "%d", c.Fx().y);
                m_gfx->Text(FontSize::Small, vx - 40, y + 4, WithAlpha(t.dim, alpha), "<");
                m_gfx->Text(FontSize::Normal, vx, y, WithAlpha(t.fg, alpha), val);
                m_gfx->Text(FontSize::Small, vx + 40, y + 4, WithAlpha(t.dim, alpha), ">");
            }
        }

        // Contextual hint for the current row.
        if (m_edit_cursor == EF_Background)
            DrawHint({ {{"up","down"}, "Row"}, {{"left","right"}, "Change background type"}, {{"b"}, "Save & back"} });
        else if (m_edit_cursor == EF_Wallpaper)
            DrawHint({ {{"up","down"}, "Row"}, {{"left","right"}, "Choose photo overlay"}, {{"b"}, "Save & back"} });
        else if (m_edit_cursor == EF_WallpaperBlur && IsVideoPath(c.wallpaper))
            DrawHint("Blur is not available for video wallpapers   B: Back");
        else if (IsEffectRow(m_edit_cursor))
            DrawHint({ {{"up","down"}, "Row"}, {{"left","right","a"}, "Toggle effect"},
                      {{"left","right"}, "Adjust value"}, {{"b"}, "Save & back"} });
        else if (IsRibbonRow(m_edit_cursor))
            DrawHint({ {{"up","down"}, "Row"}, {{"left","right"}, "Adjust value"}, {{"b"}, "Save & back"} });
        else if (m_edit_cursor == EF_Save)
            DrawHint({ {{"up","down"}, "Row"}, {{"a"}, "Save & apply"}, {{"b"}, "Save & back"} });
        else if (m_edit_cursor == EF_Rename)
            DrawHint({ {{"up","down"}, "Row"}, {{"a"}, "Rename"}, {{"b"}, "Save & back"} });
        else if (m_edit_cursor == EF_Delete)
            DrawHint({ {{"up","down"}, "Row"}, {{"a"}, "Delete this theme"}, {{"b"}, "Save & back"} });
        else
            DrawHint({ {{"up","down"}, "Row"}, {{"a"}, "Edit color"}, {{"b"}, "Save & back"} });
    }
    void Menu::DrawColorPicker() {
        if (!m_pick_target) return;
        const int cx = gfx::Gfx::Width / 2;

        // Fixed, always-readable chrome (independent of the Color being edited).
        const SDL_Color white{240, 240, 240, 255}, dim{150, 150, 155, 255},
                        accent{90, 200, 255, 255}, track{50, 50, 58, 255};
        m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{12, 12, 16, 255});

        m_gfx->TextCentered(FontSize::Title, cx, 70, white, T("Color"));

        // Live swatch + hex.
        m_gfx->FillRect(cx - 130, 150, 260, 110, *m_pick_target);
        char hex[16];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                 m_pick_target->r, m_pick_target->g, m_pick_target->b);
        m_gfx->TextCentered(FontSize::Large, cx, 280, white, hex);

        const char *names[3] = { "R", "G", "B" };
        const Uint8 vals[3]  = { m_pick_target->r, m_pick_target->g, m_pick_target->b };
        const SDL_Color chc[3] = {{255,90,90,255},{90,220,110,255},{100,150,255,255}};

        const int tx = cx - 300, tw = 520, sy = 360, rh = 68;
        for (int i = 0; i < 3; i++) {
            const bool sel = (i == m_pick_channel);
            const int  y   = sy + i * rh;
            m_gfx->Text(FontSize::Large, tx - 60, y - 8, sel ? accent : white, names[i]);
            m_gfx->FillRect(tx, y, tw, 10, track);
            m_gfx->FillRect(tx, y, tw * vals[i] / 255, 10, chc[i]);
            m_gfx->FillRect(tx + tw * vals[i] / 255 - 5, y - 8, 10, 26, sel ? accent : white);
            char vb[8]; snprintf(vb, sizeof(vb), "%d", vals[i]);
            m_gfx->Text(FontSize::Normal, tx + tw + 30, y - 10, sel ? accent : white, vb);
        }

        m_gfx->TextCentered(FontSize::Small, cx, kHintY, dim,
            "A: Done   B: Cancel");
    }
    void Menu::DrawFonts() {
        const Theme &t = m_theme.Current();
        DrawTopBar("Fonts");

        std::vector<std::string> labels, values;
        for (int i = 0; i < (int)m_font_names.size(); i++) {
            labels.push_back(m_font_names[i]);
            values.push_back(i == m_font_applied ? T("applied") : "");
        }
        DrawCarousel(labels, values, m_font_cursor, m_sub_scroll);

        // Live preview of the highlighted font, drawn IN that font at the bottom.
        EnsurePreviewFont(m_font_cursor);
        m_gfx->UseDefaultFont(false);
        m_gfx->TextCentered(FontSize::Large, gfx::Gfx::Width / 2, kHintY - 66, t.fg,
                            "The quick brown fox 0123");
        m_gfx->UseDefaultFont(true);

        DrawHint({ {{"up","down"}, "Preview"}, {{"a"}, "Apply"}, {{"b"}, "Back"} });
    }
    void Menu::DrawKeyboard() {
        const Theme &t = m_theme.Current();
        const int cx = gfx::Gfx::Width / 2;
        DrawTopBar("Rename");

        // Current text in an input box.
        m_gfx->FillRect(cx - 400, 130, 800, 60, WithAlpha(t.fg, 24));
        m_gfx->FillRect(cx - 400, 186, 800, 3, t.accent);
        std::string shown = m_kb_text.empty() ? T("(empty)") : m_kb_text;
        m_gfx->Text(FontSize::Large, cx - 384, 142,
                    m_kb_text.empty() ? t.dim : t.fg, shown.c_str());

        // Key grid.
        const int top = 250, rowH = 66, keyW = 66, cxKeys = cx;
        for (int r = 0; r < 4; r++) {
            const int n = (int)strlen(kKbRows[r]);
            const int rowW = n * keyW;
            const int x0 = cxKeys - rowW / 2;
            const int y  = top + r * rowH;
            for (int c = 0; c < n; c++) {
                const bool sel = (m_kb_row == r && m_kb_col == c);
                const int kx = x0 + c * keyW;
                if (sel) m_gfx->FillRect(kx + 4, y - 4, keyW - 8, rowH - 10, WithAlpha(t.accent, 70));
                char ch = kKbRows[r][c];
                if (m_kb_upper && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
                char s[2] = { ch, 0 };
                m_gfx->Text(FontSize::Large, kx + keyW / 2 - 8, y, sel ? t.accent : t.fg, s);
            }
        }

        // Special row.
        const int y = top + 4 * rowH;
        const int sw = 176, gap = 12;
        const int totW = kKbSpecialCols * sw + (kKbSpecialCols - 1) * gap, x0 = cx - totW / 2;
        for (int c = 0; c < kKbSpecialCols; c++) {
            const bool sel = (m_kb_row == kKbSpecialRow && m_kb_col == c);
            const int kx = x0 + c * (sw + gap);
            SDL_Color fill = (c == 0 && m_kb_upper) ? t.accent : t.fg;
            m_gfx->FillRect(kx, y - 4, sw, rowH - 12, WithAlpha(fill, sel ? 90 : 26));
            m_gfx->TextCentered(FontSize::Normal, kx + sw / 2, y + 4,
                                sel ? t.accent : t.fg, T(kKbSpecial[c]));
        }

        DrawHint({ {{"a"}, "Type"}, {{"x"}, "Shift"}, {{"y"}, "Backspace"}, {{"plus"}, "Done"}, {{"b"}, "Cancel"} });
    }
    void Menu::DrawDialog() {
        const Theme &t = m_theme.Current();
        // Dim the whole screen, then draw a centered box.
        m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{0,0,0,150});
        int cx = gfx::Gfx::Width / 2;
        // The note wraps; the box grows to fit it.
        const int bw = 640;
        const auto note = m_dialog_note.empty() ? std::vector<std::string>()
                        : WrapText(FontSize::Small, m_dialog_note, bw - 60);
        const int nlh = m_gfx->LineHeight(FontSize::Small) + 4;
        const int noteH = (int)note.size() * nlh;
        const int nopts = m_dialog == Dialog::CrashReport ? 1 : 2;   // a notice has only OK
        int bh = 276 + std::max(0, noteH - nlh) - (2 - nopts) * 48;
        int bx = cx - bw / 2, by = gfx::Gfx::Height / 2 - bh / 2;
        m_gfx->FillRect(bx, by, bw, bh, WithAlpha(t.bg_bottom, 245));
        m_gfx->FillRect(bx, by, bw, 4, t.accent);

        // Power prompts set their own heading (and sometimes a warning line); the
        // launch/close dialogs are always about the running game.
        m_gfx->TextCentered(FontSize::Large, cx, by + 40, t.title,
                            m_dialog_title.empty() ? T("Close running application?")
                                                   : m_dialog_title.c_str());
        for (size_t l = 0; l < note.size(); l++)
            m_gfx->TextCentered(FontSize::Small, cx, by + 88 + (int)l * nlh, t.dim, note[l].c_str());

        const char *opts[2] = { T("Yes"), T("No") };
        if (nopts == 1) opts[0] = T("OK");
        for (int i = 0; i < nopts; i++) {
            bool sel = (i == m_dialog_cursor);
            int y = by + 136 + std::max(0, noteH - nlh) + i * 48;
            if (sel) m_gfx->FillRect(cx - 90, y - 4, 180, 42, WithAlpha(t.accent, 60));
            m_gfx->TextCentered(FontSize::Normal, cx, y, sel ? t.accent : t.fg, opts[i]);
        }
        // One button explains itself, and a hint would only land on top of
        // the layout's own hint row.
        if (nopts == 2) DrawHint({ {{"up","down"}, "Choose"}, {{"a"}, "Confirm"}, {{"b"}, "Cancel"} });
    }

    // ---- crash reports ------------------------------------------------------
    // When a game or homebrew crashes, Atmosphere writes a report and the
    // system comes back to the menu as if nothing happened. This is the
    // "something happened" - the way the stock HOME menu says an app closed
    // because of an error - plus where the details went.
    //
    // Reports are named <posix seconds>_<program id>.log. The newest time the
    // menu has already reported is kept in cache/crash_seen.txt. With no such
    // file (first run), whatever is already on the card is taken as seen:
    // old crashes from before sLaunch was installed are not news.
    void Menu::CheckCrashReports() {
        m_crash_checked = true;
        constexpr const char *kSeen = "sdmc:/slaunch/cache/crash_seen.txt";
        static const char *const kDirs[] = { "sdmc:/atmosphere/crash_reports",
                                             "sdmc:/atmosphere/fatal_reports" };

        struct Report { unsigned long long time = 0, id = 0; int dir = 0; std::string file; };
        Report newest;
        int fresh = 0;
        unsigned long long seen = 0;
        bool have_seen = false;
        if (FILE *fp = fopen(kSeen, "r")) {
            have_seen = fscanf(fp, "time=%llu", &seen) == 1;
            fclose(fp);
        }
        // Atmosphère stamps reports with the clock at crash time, and a clock
        // that was wrong then (seen: year 2168) would otherwise sort above every
        // real crash forever, so a report from the future is ignored and a
        // "seen" stamp from the future is forgotten.
        const unsigned long long future = (unsigned long long)time(nullptr) + 86400;
        if (seen > future) { seen = 0; have_seen = false; }   // re-baseline quietly
        for (int d = 0; d < 2; d++) {
            DIR *dir = opendir(kDirs[d]);
            if (!dir) continue;
            while (struct dirent *e = readdir(dir)) {
                unsigned long long t = 0, id = 0;
                int n = 0;
                if (sscanf(e->d_name, "%llu_%16llx.log%n", &t, &id, &n) != 2 ||
                    e->d_name[n] != '\0') continue;
                if (t > future) continue;
                if (have_seen && t > seen) fresh++;
                if (t > newest.time) newest = Report{ t, id, d, e->d_name };
            }
            closedir(dir);
        }
        if (newest.time > seen || !have_seen) {
            mkdir("sdmc:/slaunch", 0777);
            mkdir("sdmc:/slaunch/cache", 0777);
            if (FILE *fp = fopen(kSeen, "w")) {
                fprintf(fp, "time=%llu\n", std::max(seen, newest.time));
                fclose(fp);
            }
        }
        if (!have_seen || fresh == 0) return;

        // Who it was. The menu's own id, an installed game, or - for homebrew,
        // which runs under hbloader and a borrowed program id - whatever the
        // menu last launched, if it was launched before the crash.
        std::string who;
        if (newest.id == 0x0100000000001042ULL) who = "sLaunch";
        if (newest.id == 0x010000000000100DULL) who = T("Album");
        for (const auto &a : m_apps)
            if (who.empty() && a.app_id == newest.id) who = a.name;
        if (FILE *fp = fopen("sdmc:/slaunch/cache/last_launch.txt", "r")) {
            long long t = 0;
            unsigned long long id = 0;
            char nro[256] = "";
            if (fscanf(fp, "time=%lld\nid=%llx\nnro=%255[^\n]", &t, &id, nro) >= 2 &&
                nro[0] && (unsigned long long)t <= newest.time && id == newest.id) {
                const char *base = strrchr(nro, '/');
                who = base ? base + 1 : nro;
            }
            fclose(fp);
        }
        if (who.empty()) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%s %016llX", T("Program"), newest.id);
            who = buf;
        }

        char note[512];
        snprintf(note, sizeof(note), T(newest.dir == 1
                     ? "%s hit a fatal error. Atmosphère usually saves the crash details, check the logs: atmosphere/fatal_reports/%s"
                     : "%s crashed. Atmosphère usually saves the crash details, check the logs: atmosphere/crash_reports/%s"),
                 who.c_str(), newest.file.c_str());
        m_dialog_note = note;
        if (fresh > 1) {
            char more[96];
            snprintf(more, sizeof(more), T("(%d crashes since you last saw this menu)"), fresh);
            m_dialog_note += std::string(" ") + more;
        }
        m_dialog_title  = T("App crash detected! :(");
        m_dialog_cursor = 0;
        m_dialog        = Dialog::CrashReport;
    }

    // ---- Deck layout --------------------------------------------------------
    // The SteamOS gamepad UI, as close as a Switch home menu gets: the selected
    // game as one big hero panel, its neighbours as covers running off to the
    // right, and a row of cards under both carrying news or the Lua widgets.
    //
    // The rows are what navigation moves between - covers, tabs, cards - which is
    // why this layout keeps a row index of its own instead of reusing the flat
    // cursor the 1-D layouts share. The selection itself is still m_cursor, so
    // launching, options, favourites and the move mode all work here untouched.
} // namespace sl::menu::ui
