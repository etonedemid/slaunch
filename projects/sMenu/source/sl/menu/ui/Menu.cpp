#include <sl/menu/ui/Menu.hpp>
#include <unordered_set>
#include <sl/menu/ui/Locale.hpp>
#include <sl/menu/net/Http.hpp>
#include <sl/smi/Protocol.hpp>
#include <SDL2/SDL_image.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include "Menu_Internal.hpp"

namespace sl::menu::ui {

    // One-time cleanup: Steam news and back-of-case screenshots fetched before
    // net::SteamAppFor took whichever game Steam's search listed first, so a
    // title that is not on Steam could be carrying another game's. Dropping
    // them makes the next visit fetch again, matched properly. A marker file
    // keeps this to a single pass.
    static void ForgetUnmatchedSteamArt() {
        const char *marker = "sdmc:/slaunch/cache/steam_match_v2";   // v2: sequels no longer match
        struct stat st {};
        if (!g_sd_ok || stat(marker, &st) == 0) return;
        auto sweep = [](const char *dir, auto match) {
            DIR *d = opendir(dir);
            if (!d) return;
            while (struct dirent *e = readdir(d))
                if (match(std::string(e->d_name)))
                    remove((std::string(dir) + "/" + e->d_name).c_str());
            closedir(d);
        };
        auto ends = [](const std::string &n, const char *suf) {
            const size_t k = strlen(suf);
            return n.size() > k && n.compare(n.size() - k, k, suf) == 0;
        };
        sweep("sdmc:/slaunch/covers", [&](const std::string &n) {
            return ends(n, "_s0.jpg") || ends(n, "_s1.jpg");
        });
        sweep("sdmc:/slaunch/cache/news", [](const std::string &n) {
            return n.compare(0, 6, "steam_") == 0;
        });
        if (FILE *f = fopen(marker, "w")) fclose(f);
    }

    void Menu::Init(gfx::Gfx *gfx, AccountUid user, u64 suspended_app_id, bool start_oobe) {
        PhaseReset();
        m_gfx       = gfx;
        m_user      = user;
        m_suspended = suspended_app_id;
        ForgetUnmatchedSteamArt();
        m_icons.Init(gfx);
        m_hb_icons.Init(gfx, hb::IconDir);
        // Shortcut art is RetroArch's thumbnail folder, not anything sLaunch
        // extracted, so those keys resolve to a path of their own. Any other
        // key returns empty and falls back to the cache directory.
        m_hb_icons.SetPathFn([this](u64 key) {
            auto it = m_shortcut_art.find(key);
            return it == m_shortcut_art.end() ? std::string() : it->second;
        });
        LocaleInit();   // load the system-language locale (English is the fallback)
        Phase("locale");

        // Resolve the user's nickname; if the launcher handed us an invalid uid,
        // fall back to the first account so the top bar isn't stuck on "Player".
        if (!accountUidIsValid(&m_user)) {
            s32 count = 0;
            AccountUid uids[ACC_USER_LIST_SIZE];
            if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &count)) && count > 0)
                m_user = uids[0];
        }
        AccountProfile profile;
        AccountProfileBase base = {};
        if (accountUidIsValid(&m_user) && R_SUCCEEDED(accountGetProfile(&profile, m_user))) {
            accountProfileGet(&profile, nullptr, &base);
            accountProfileClose(&profile);
            if (base.nickname[0]) {
                strncpy(m_nickname, base.nickname, 32);
                m_nickname[32] = '\0';
            }
        }

        // Point the config layer at whoever this is before the first file is
        // read. main already did this - it needs settings before Menu exists -
        // but Init may have resolved a different uid just above, and the sim
        // comes in through here only.
        cfg::SetUser(m_user);
        cfg::NoteNickname(m_nickname);
        Phase("account");

        m_theme.Load();
        m_theme_cursor = m_theme.CurrentIndex();
        Phase("theme.Load");

        ScanFonts();
        Phase("ScanFonts");
        LoadFontConfig();   // applies the saved font (or default)
        Phase("LoadFontConfig");

        LoadFavourites();
        LoadSort();
        LoadOrder();
        LoadTileCfg();
        LoadHbPins();
        LoadHbFavourites();
        LoadHbDonor();
        LoadSettings();
        { struct stat st;
          m_memtrace_on = (stat("sdmc:/slaunch/config/memtrace.txt", &st) == 0); }
        LoadSysEntries();
        LoadNames();
        Phase("config files");
        ScanIconPacks();
        Phase("ScanIconPacks");
        LoadIconPackSetting();
        LoadFlowConfig();
        LoadPlayCache();    // last run's play times, refreshed from pdm after frame 1
        Phase("play cache");
        ShowPowerError();   // a chainload the daemon could not carry out
        // Widget loading (Lua parse + curl init) is deferred to InitDeferred so it
        // doesn't sit on the suspend->first-frame critical path.

        // Welcome screen: after setup, and once per console boot. The menu applet
        // restarts every time you come back from a game, so we tag the boot we
        // greeted. armGetSystemTick() counts from power-on, so (wall clock -
        // uptime) is a stable id within a boot and differs across boots.
        if (start_oobe) {
            m_screen = Screen::Oobe;
        } else if (m_welcome_enabled && m_suspended == 0 && BootWelcomePending()) {
            EnterWelcome();
            MarkBootWelcomed();
        } else {
            m_screen = Screen::Main;
        }
        RebuildItems();
        // So the very first Render() does not read a mismatch against the
        // Screen::Main default above and fire a spurious transition fade on
        // top of the appear fade it already draws.
        m_screen_seen = m_screen;
    }
    bool Menu::BootWelcomePending() const {
        FILE *fp = fopen(kWelcomedPath, "r");
        if (!fp) return true;                 // never greeted -> this boot counts
        long long stored = 0;
        const bool got = (fscanf(fp, "%lld", &stored) == 1);
        fclose(fp);
        if (!got) return true;
        // Same boot if the recorded boot id is within a few seconds of ours.
        const long long d = BootId() - stored;
        return (d > 5 || d < -5);
    }
    void Menu::MarkBootWelcomed() const {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/cache", 0777);
        FILE *fp = fopen(kWelcomedPath, "w");
        if (!fp) return;
        fprintf(fp, "%lld\n", BootId());
        fclose(fp);
    }
    // Everything here runs on a worker. None of it touches the renderer: Widgets
    // only ever receives a Gfx* as a Render() argument and never stores one, and
    // the mixer is a separate subsystem from SDL_video.
    void Menu::DeferredTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        m->m_widgets.Init();                       // curl global init + Lua parse
        m->m_news.Init();                          // whatever news the card holds
        m->m_deferred_audio = m->m_music.Init();   // mixer + decoders
        m->m_sfx.Init(m->m_deferred_audio);        // ~1.4 MB of WAV off the SD
        m->m_deferred_flag.store(true, std::memory_order_release);
    }
    void Menu::InitDeferred() {
        if (m_deferred_started) return;
        m_deferred_started = true;
        m_deferred_done    = true;

        if (R_SUCCEEDED(threadCreate(&m_deferred_thread, &Menu::DeferredTrampoline,
                                     this, nullptr, 0x20000, 0x3B, -2))) {
            threadStart(&m_deferred_thread);
        } else {
            // No thread available: do it inline. Slow, but a menu with sound
            // beats a menu that silently lost half its init.
            DeferredTrampoline(this);
        }
    }
    void Menu::PollDeferred() {
        if (!m_deferred_started || m_deferred_joined) return;
        if (!m_deferred_flag.load(std::memory_order_acquire)) return;

        threadWaitForExit(&m_deferred_thread);
        threadClose(&m_deferred_thread);
        m_deferred_joined = true;

        // Widget tiles cannot be built until the widgets themselves exist, so
        // the entry list is rebuilt now that they do.
        {
            const std::string keep = m_items.empty() ? std::string()
                                                     : ItemKey(m_items[m_cursor]);
            RebuildItems();
            if (!keep.empty()) SelectByKey(keep);
        }

        // Welcome chime only on a fresh open (no game suspended behind us), so it
        // isn't heard every single time you HOME out of a game. On the boot
        // welcome screen the opening jingle takes its place (audio only comes up
        // on the worker, so it cannot be played from Init).
        if (m_deferred_audio && m_suspended == 0) {
            if (m_screen == Screen::Welcome) {
                m_sfx.Play(audio::Sfx::Startup);
                m_welcome_start = armGetSystemTick();   // hold for the jingle
            } else {
                m_sfx.Play(audio::Sfx::Welcome);
            }
        }
        // Resolve pinned homebrew names/icons on a worker thread (never blocks the
        // menu-start path); they show fallback names until it lands.
        StartResolvePins();
        // Optional: check GitHub for a newer release (off the main thread).
        StartUpdateCheck();
    }
    Menu::~Menu() {
        // Let the homebrew workers finish before we tear down (they write into
        // members and would outlive them otherwise).
        if (m_hb_scan_running) {
            threadWaitForExit(&m_hb_thread);
            threadClose(&m_hb_thread);
            m_hb_scan_running = false;
        }
        if (m_pin_running) {
            threadWaitForExit(&m_pin_thread);
            threadClose(&m_pin_thread);
            m_pin_running = false;
        }
        if (m_upd_running) {
            threadWaitForExit(&m_upd_thread);
            threadClose(&m_upd_thread);
            m_upd_running = false;
        }
        if (m_play_running) {
            threadWaitForExit(&m_play_thread);
            threadClose(&m_play_thread);
            m_play_running = false;
        }
        // The deferred worker builds the mixer, the sound chunks and the widget
        // list, so it has to be finished before any of those are torn down
        // below - closing the mixer out from under a thread still opening it is
        // a crash on exit that would only ever reproduce on a slow SD.
        if (m_cover_running) {
            threadWaitForExit(&m_cover_thread);
            threadClose(&m_cover_thread);
            m_cover_running = false;
        }
        StopArt();   // holds surfaces too, and must not outlive the renderer
        // The hero decoder holds surfaces of its own, and outliving the renderer
        // would leak them at best.
        if (m_shot_running) {
            threadWaitForExit(&m_shot_thread);
            threadClose(&m_shot_thread);
            m_shot_running = false;
            if (m_shot_surf_a) { SDL_FreeSurface(m_shot_surf_a); m_shot_surf_a = nullptr; }
            if (m_shot_surf_b) { SDL_FreeSurface(m_shot_surf_b); m_shot_surf_b = nullptr; }
        }
        if (m_deferred_started && !m_deferred_joined) {
            threadWaitForExit(&m_deferred_thread);
            threadClose(&m_deferred_thread);
            m_deferred_joined = true;
        }
        // Free SFX chunks before Music::Exit() closes the mixer, then stop the
        // network thread.
        m_sfx.Exit();
        m_music.Exit();
        m_widgets.Exit();
        // Free textures while the renderer is still alive (main() runs gfx.Exit()
        // only after the Menu is destroyed).
        m_icons.Exit();
        m_hb_icons.Exit();
        DeckFreeArt();      // news card art, decoded on the render thread
        if (m_wallpaper)   { m_gfx->FreeImage(m_wallpaper);   m_wallpaper = nullptr; }
        if (m_wallpaper_blur) { m_gfx->FreeImage(m_wallpaper_blur); m_wallpaper_blur = nullptr; }
        m_video_player.Close();
        m_album_video.Close();   // album clip playback (if any) outlives the renderer
        FreeAlbumTexture();   // a capture is resident whenever the viewer is open
        if (m_flow_wrap) { m_gfx->FreeImage(m_flow_wrap); m_flow_wrap = nullptr; }
        FreeWidgetTileTextures();
        if (m_tile_pic)      { m_gfx->FreeImage(m_tile_pic);      m_tile_pic = nullptr; }
        if (m_music_art)     { m_gfx->FreeImage(m_music_art);     m_music_art = nullptr; }
        if (m_bd_cur)        { m_gfx->FreeImage(m_bd_cur);        m_bd_cur = nullptr; }
        if (m_bd_old)        { m_gfx->FreeImage(m_bd_old);        m_bd_old = nullptr; }
        if (m_fm_view)       { m_gfx->FreeImage(m_fm_view);       m_fm_view = nullptr; }
        if (m_fm_running)    { m_fm_cancel.store(true); threadWaitForExit(&m_fm_thread);
                               threadClose(&m_fm_thread); m_fm_running = false; }
        FreeOobePreviews();
        if (m_tile_pic_next) { m_gfx->FreeImage(m_tile_pic_next); m_tile_pic_next = nullptr; }
        for (auto &kv : m_covers)
            if (kv.second) m_gfx->FreeImage(kv.second);
        m_covers.clear();
        for (auto &kv : m_game_wraps)
            if (kv.second) m_gfx->FreeImage(kv.second);
        m_game_wraps.clear();
        for (auto &kv : m_hero_art)
            if (kv.second) m_gfx->FreeImage(kv.second);
        m_hero_art.clear();
        for (auto &kv : m_shots) {
            if (kv.second.a) m_gfx->FreeImage(kv.second.a);
            if (kv.second.b) m_gfx->FreeImage(kv.second.b);
        }
        m_shots.clear();
        for (auto &kv : m_sys_icons)
            if (kv.second) m_gfx->FreeImage(kv.second);
        m_sys_icons.clear();
        for (auto &kv : m_hint_icons)
            if (kv.second) m_gfx->FreeImage(kv.second);
        m_hint_icons.clear();
    }
    void Menu::RebuildItems() {
        m_items.clear();

        // Split games into favourites and the rest, each ordered by sort mode.
        std::vector<MenuItem> favs, rest;
        for (auto &a : m_apps) {
            MenuItem it;
            it.kind = ItemKind::Game;
            it.app_id = a.app_id;
            const std::string *custom = CustomName(a.app_id);
            it.name = custom ? *custom : a.name;
            it.is_gamecard  = a.is_gamecard;
            it.is_favourite = IsFavourite(a.app_id);
            (it.is_favourite ? favs : rest).push_back(std::move(it));
        }
        auto sort_group = [&](std::vector<MenuItem> &v) {
            auto title = [](const MenuItem &x, const MenuItem &y) {
                return strcasecmp(x.name.c_str(), y.name.c_str());
            };
            // Play-stat sorts: entries with no record (homebrew, never-launched
            // games) keep their relative order at the bottom rather than mixing
            // into the middle as zeroes.
            auto by_stat = [&](const MenuItem &x, const MenuItem &y, bool recent) {
                const play::PlayInfo *px = Play(x.app_id), *py = Play(y.app_id);
                const u64 vx = !px ? 0 : (recent ? px->last_played : px->seconds);
                const u64 vy = !py ? 0 : (recent ? py->last_played : py->seconds);
                if (vx != vy) return vx > vy;
                return title(x, y) < 0;
            };
            switch (m_sort) {
                case SortMode::TitleAsc:
                    std::sort(v.begin(), v.end(),
                              [&](const MenuItem &x, const MenuItem &y){ return title(x, y) < 0; });
                    break;
                case SortMode::TitleDesc:
                    std::sort(v.begin(), v.end(),
                              [&](const MenuItem &x, const MenuItem &y){ return title(x, y) > 0; });
                    break;
                case SortMode::GamecardFirst:
                    // Physical carts grouped on top, each group A-Z.
                    std::sort(v.begin(), v.end(),
                              [&](const MenuItem &x, const MenuItem &y){
                                  if (x.is_gamecard != y.is_gamecard) return x.is_gamecard;
                                  return title(x, y) < 0;
                              });
                    break;
                case SortMode::RecentlyPlayed:
                    std::stable_sort(v.begin(), v.end(),
                              [&](const MenuItem &x, const MenuItem &y){ return by_stat(x, y, true); });
                    break;
                case SortMode::MostPlayed:
                    std::stable_sort(v.begin(), v.end(),
                              [&](const MenuItem &x, const MenuItem &y){ return by_stat(x, y, false); });
                    break;
                default: break;   // Default: keep the built arrangement (custom
                                  // move-order is applied to the whole list below)
            }
        };
        // Pinned homebrew (name + cached icon resolved by ResolvePins; fallback
        // file-base name until then). Favourited pins join the favourites group so
        // they sort to the very top with the games; the rest sit just below.
        std::vector<MenuItem> hb_pinned;
        for (auto &p : m_hb_pins) {
            MenuItem it;
            it.kind        = ItemKind::Homebrew;
            it.hb_path     = p.path;
            it.name        = p.name;
            it.hb_icon     = p.icon_key;
            it.is_favourite = IsHbFavourite(p.path);
            (it.is_favourite ? favs : hb_pinned).push_back(std::move(it));
        }

        sort_group(favs);   // games + favourited homebrew, ordered by sort mode
        sort_group(rest);

        // Favourites pinned at the very top for quick access.
        for (auto &it : favs) m_items.push_back(std::move(it));

        // Remaining (non-favourite) pinned homebrew, just under the favourites.
        for (auto &it : hb_pinned) m_items.push_back(std::move(it));

        // Scanned homebrew - XMB only.
        //
        // XMB gives homebrew a category of its own, and a category holding only
        // what you happened to pin is a category with nothing in it. So in XMB
        // the whole scan goes in, minus anything already pinned above. The other
        // layouts are deliberately left alone: they list pinned homebrew and
        // nothing else, which is what they have always done and what keeps a
        // flat dump of every .nro on the card out of the main carousel.
        //
        // StartHbScan is lazy and a no-op once the scan has landed. Its
        // thread-creation failure path calls RebuildItems back, but it sets
        // m_hb_scanned first, so the re-entry returns here immediately and the
        // recursion is one level deep at most.
        if (m_ui_mode == UiMode::XMB) {
            StartHbScan();
            for (const auto &h : m_hb) {
                if (IsHbPinned(h.path)) continue;   // already added above
                MenuItem it;
                it.kind         = ItemKind::Homebrew;
                it.hb_path      = h.path;
                it.name         = h.name;
                it.hb_icon      = h.icon_key;
                it.is_favourite = IsHbFavourite(h.path);
                m_items.push_back(std::move(it));
            }
        }

        // Launcher shortcuts (RetroArch playlists, shortcuts.txt). XMB always
        // takes them - a column each is the whole reason they exist - and the
        // other layouts only on request, since a scanned ROM library is easily
        // larger than everything else on this list put together.
        //
        // Appended in scan order, which ScanShortcuts already sorted by
        // category then name, so XmbRebuild can group them by walking once.
        if (m_retroarch && (m_ui_mode == UiMode::XMB || m_shortcuts_everywhere)) {
            StartHbScan();   // same worker reads both; no-op once it has landed
            for (const auto &sc : m_shortcuts) {
                MenuItem it;
                it.kind     = ItemKind::Homebrew;
                it.hb_path  = sc.nro;
                it.hb_argv  = sc.argv;
                it.name     = sc.name;
                it.category = sc.category;
                it.hb_icon  = sc.icon_key;
                m_items.push_back(std::move(it));
            }
        }

        // System shortcuts (hidden ones are skipped; Theming is never hideable).
        auto add = [&](ItemKind k, const char *name) {
            if (IsSysHidden(k)) return;
            MenuItem it; it.kind = k; it.name = name; m_items.push_back(std::move(it));
        };
        add(ItemKind::Theming,      T("Theming"));          // always shown
        add(ItemKind::RandomGame,   T("Random game"));
        add(ItemKind::Controllers,  T("Controllers"));
        add(ItemKind::Album,        T("Album"));
        add(ItemKind::MusicPlayer,  T("Music"));
        add(ItemKind::UserPage,     T("User Page"));
        add(ItemKind::WebBrowser,   T("Web Browser"));
        add(ItemKind::MiiEdit,      T("Mii Edit"));
        add(ItemKind::Wifi,         T("Network"));
        add(ItemKind::Power,        T("Power"));
        add(ItemKind::HomebrewMenu, T("Homebrew menu"));
        add(ItemKind::FileManager,  T("Files"));

        // Widget tiles, sitting with the system shortcuts. They only exist once
        // the deferred worker has built the widgets, which is why RebuildItems
        // runs again when it lands.
        //
        // The tiled flag comes from the config whatever the layout, so a placed
        // widget keeps ticking and its tile is current the moment you switch
        // back. The ENTRY only exists on the wall: everywhere else it would be a
        // row you can land on that does nothing when you press A.
        if (m_deferred_joined) {
            for (int i = 0; i < m_widgets.Count(); i++) {
                widgets::IWidget *w = m_widgets.At(i);
                if (!w) continue;
                const bool placed = m_tilecfg.count("w" + w->Name()) != 0;
                m_widgets.SetTiled(i, placed);
                if (!placed || m_ui_mode != UiMode::Grid) continue;
                MenuItem it;
                it.kind = ItemKind::WidgetTile;
                it.name = w->Name();
                m_items.push_back(std::move(it));
            }
        }

        // The remaining (non-favourite) games.
        for (auto &it : rest) m_items.push_back(std::move(it));

        // Custom arrangement (from Move): reorder the whole list by the saved key
        // order. Seeded from the built arrangement, so it starts as a no-op and
        // only reflects entries the user has actually moved; new/unlisted entries
        // sort stably to the end in their default position. Default sort only.
        if (m_sort == SortMode::Default && !m_order.empty()) {
            // Rank each entry once up front. Looking the key up inside the
            // comparator instead (a scan of m_order per comparison, plus an
            // ItemKey string built per comparison) made this the slowest thing
            // in the menu on a large library.
            std::unordered_map<std::string, int> rank_of;
            rank_of.reserve(m_order.size());
            for (int i = 0; i < (int)m_order.size(); i++)
                rank_of.emplace(m_order[i], i);
            const int unranked = (int)m_order.size() + 1;

            std::vector<std::pair<int, const MenuItem *>> keyed;
            keyed.reserve(m_items.size());
            for (const auto &it : m_items) {
                auto f = rank_of.find(ItemKey(it));
                keyed.emplace_back(f == rank_of.end() ? unranked : f->second, &it);
            }
            std::stable_sort(keyed.begin(), keyed.end(),
                [](const auto &x, const auto &y){ return x.first < y.first; });

            std::vector<MenuItem> sorted;
            sorted.reserve(m_items.size());
            for (const auto &k : keyed) sorted.push_back(*k.second);
            m_items = std::move(sorted);
        }

        // Name filter. Applied here, after everything has been added, rather
        // than at each push_back: one place to get right, and the sort and
        // custom order above have already run on the full list so the matches
        // keep the order they would have had.
        //
        // System entries drop out while a search is active - you are looking for
        // something to play, and leaving Power and Theming in every result reads
        // as the search having failed. B clears the search and brings them back.
        if (!m_search.empty()) {
            m_items.erase(std::remove_if(m_items.begin(), m_items.end(),
                [&](const MenuItem &it) {
                    if (it.kind != ItemKind::Game && it.kind != ItemKind::Homebrew)
                        return true;
                    return !ContainsFold(it.name, m_search);
                }), m_items.end());
        }

        if (m_cursor >= (int)m_items.size())
            m_cursor = m_items.empty() ? 0 : (int)m_items.size() - 1;

        XmbRebuild();   // regroup the cross-media bar for the new list
        FlowRebuild();  // ...and the coverflow's games-only row
    }
    // ---- Favourites + sorting ----------------------------------------------
    bool Menu::IsFavourite(u64 app_id) const {
        return std::find(m_favourites.begin(), m_favourites.end(), app_id) != m_favourites.end();
    }
    void Menu::ToggleFavourite(u64 app_id) {
        auto it = std::find(m_favourites.begin(), m_favourites.end(), app_id);
        if (it != m_favourites.end()) m_favourites.erase(it);
        else                          m_favourites.push_back(app_id);
        SaveFavourites();
    }
    void Menu::LoadFavourites() {
        m_favourites.clear();
        FILE *fp = fopen(GetUserConfigPath("favourites.txt").c_str(), "r");
        if (!fp) return;
        char line[32];
        while (fgets(line, sizeof(line), fp)) {
            u64 id = strtoull(line, nullptr, 16);
            if (id) m_favourites.push_back(id);
        }
        fclose(fp);
    }
    void Menu::SaveFavourites() {
        EnsureUserConfigDir();
        const std::string path = GetUserConfigPath("favourites.txt");
        FILE *fp = fopen(path.c_str(), "w");
        if (!fp) return;
        for (u64 id : m_favourites) fprintf(fp, "%016llX\n", (unsigned long long)id);
        fclose(fp);
    }
    // Homebrew favourites are kept as .nro paths, one per line.
    bool Menu::IsHbFavourite(const std::string &path) const {
        return std::find(m_hb_favs.begin(), m_hb_favs.end(), path) != m_hb_favs.end();
    }
    void Menu::ToggleHbFavourite(const std::string &path) {
        auto it = std::find(m_hb_favs.begin(), m_hb_favs.end(), path);
        if (it != m_hb_favs.end()) m_hb_favs.erase(it);
        else                       m_hb_favs.push_back(path);
        SaveHbFavourites();
    }
    void Menu::LoadHbFavourites() {
        m_hb_favs.clear();
        FILE *fp = fopen(GetUserConfigPath("hb_favourites.txt").c_str(), "r");
        if (!fp) return;
        char line[FS_MAX_PATH + 2];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0]) m_hb_favs.push_back(line);
        }
        fclose(fp);
    }
    void Menu::SaveHbFavourites() {
        EnsureUserConfigDir();
        const std::string path = GetUserConfigPath("hb_favourites.txt");
        FILE *fp = fopen(path.c_str(), "w");
        if (!fp) return;
        for (auto &p : m_hb_favs) fprintf(fp, "%s\n", p.c_str());
        fclose(fp);
    }
    void Menu::LoadSort() {
        FILE *fp = fopen(GetUserConfigPath("sort.txt").c_str(), "r");
        if (!fp) return;
        int v = 0;
        if (fscanf(fp, "%d", &v) == 1 && v >= 0 && v < (int)SortMode::Count)
            m_sort = (SortMode)v;
        fclose(fp);
    }
    void Menu::SaveSort() {
        EnsureUserConfigDir();
        const std::string path = GetUserConfigPath("sort.txt");
        FILE *fp = fopen(path.c_str(), "w");
        if (!fp) return;
        fprintf(fp, "%d\n", (int)m_sort);
        fclose(fp);
    }
    // Case-insensitive substring. ASCII folding only: a UTF-8 name passes
    // through byte for byte, so an accented title still matches when typed the
    // way it is spelled - which is all the built-in keyboard can produce.
    bool Menu::ContainsFold(const std::string &hay, const std::string &needle) {
        if (needle.empty()) return true;
        if (hay.size() < needle.size()) return false;
        auto lower = [](unsigned char c) {
            return (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        };
        for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
            size_t k = 0;
            while (k < needle.size() &&
                   lower((unsigned char)hay[i + k]) == lower((unsigned char)needle[k])) k++;
            if (k == needle.size()) return true;
        }
        return false;
    }

    // Search opens the menu's own keyboard - no applet round trip, so the list
    // is still on screen behind it and coming back is a frame, not a relaunch.
    void Menu::OpenSearch() {
        m_kb_purpose = sl::smi::Kb_Search;
        m_kb_text    = m_search;      // seeded, so refining beats retyping
        m_kb_row = 0; m_kb_col = 0; m_kb_upper = false;
        m_screen = Screen::Keyboard;
    }

    // Stable per-entry key for the custom order: games by title id, homebrew by
    // .nro path, system shortcuts by kind. Kept text so order.txt is one key/line.
    std::string Menu::ItemKey(const MenuItem &it) const {
        char b[40];
        switch (it.kind) {
            case ItemKind::Game:
                snprintf(b, sizeof(b), "g%016llX", (unsigned long long)it.app_id);
                return b;
            case ItemKind::Homebrew:
                // Every ROM in a playlist runs the same core, so the .nro alone
                // would give a whole library one shared key - and with it one
                // shared favourite, name and place in the custom order. The
                // argv is what actually distinguishes them.
                return "h" + (it.hb_argv.empty() ? it.hb_path : it.hb_argv);
            case ItemKind::WidgetTile:
                return "w" + it.name;
            default:
                snprintf(b, sizeof(b), "s%d", (int)it.kind);
                return b;
        }
    }
    void Menu::SelectByKey(const std::string &key) {
        for (int i = 0; i < (int)m_items.size(); i++)
            if (ItemKey(m_items[i]) == key) {
                m_cursor = i;
                m_scroll_pos = (float)i;
                m_grid_scroll = (float)std::min(std::max(0, TileRowOf(i) - 1),
                                                TileMaxScroll());
                XmbSyncFromCursor();
                return;
            }
    }
    void Menu::LoadOrder() {
        m_order.clear();
        FILE *fp = fopen(GetUserConfigPath("order.txt").c_str(), "r");
        if (!fp) return;
        char line[FS_MAX_PATH + 4];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0]) m_order.push_back(line);
        }
        fclose(fp);
    }
    void Menu::SaveOrder() {
        EnsureUserConfigDir();
        const std::string path = GetUserConfigPath("order.txt");
        FILE *fp = fopen(path.c_str(), "w");
        if (!fp) return;
        for (auto &k : m_order) fprintf(fp, "%s\n", k.c_str());
        fclose(fp);
    }
    // Reorder the held entry one slot in the given direction, swapping with the
    // adjacent entry (any kind), then persist. Keeps the cursor on the moved item.
    void Menu::MoveSelected(int dir) {
        if (!m_move_mode || m_move_key.empty()) return;
        int cur = -1;
        for (int i = 0; i < (int)m_items.size(); i++)
            if (ItemKey(m_items[i]) == m_move_key) { cur = i; break; }
        const int nb = cur + dir;
        if (cur < 0 || nb < 0 || nb >= (int)m_items.size()) return;

        // Seed m_order from the full current arrangement the first time, so the
        // ranking is complete (a single move is otherwise ambiguous).
        if (m_order.empty())
            for (auto &it : m_items) m_order.push_back(ItemKey(it));

        auto pos = [&](const std::string &key) {
            for (int i = 0; i < (int)m_order.size(); i++) if (m_order[i] == key) return i;
            m_order.push_back(key); return (int)m_order.size() - 1;
        };
        std::swap(m_order[pos(m_move_key)], m_order[pos(ItemKey(m_items[nb]))]);
        RebuildItems();
        SelectByKey(m_move_key);   // keep the cursor on the moved entry
    }
    bool Menu::SelectApp(u64 app_id) {
        for (int i = 0; i < (int)m_items.size(); i++) {
            if (m_items[i].kind == ItemKind::Game && m_items[i].app_id == app_id) {
                m_cursor = i;
                m_scroll_pos = (float)i;
                m_grid_scroll = (float)std::min(std::max(0, TileRowOf(i) - 1),
                                                TileMaxScroll());
                XmbSyncFromCursor();
                return true;
            }
        }
        return false;
    }
    void Menu::SetApps(std::vector<AppEntry> apps) {
        m_apps = std::move(apps);
        m_play_dirty = true;   // new/removed titles -> re-query their play stats
        RebuildItems();
        // Drop the cursor onto the suspended game so it's one button away. Only
        // mark it done once the jump actually lands - the game may not be in the
        // first (cached) list yet (e.g. a gamecard just inserted), and we want to
        // retry on the next SetApps once it shows up.
        if (m_suspended != 0 && !m_jumped_to_suspended && SelectApp(m_suspended))
            m_jumped_to_suspended = true;
    }
    void Menu::SetSuspendedApp(u64 app_id) { m_suspended = app_id; }
    void Menu::ClearSuspendedApp()          { m_suspended = 0; }
    // ---- Settings (text alignment) + custom names --------------------------
    void Menu::LoadSettings() {
        FILE *fp = fopen(GetUserConfigPath("settings.txt").c_str(), "r");
        if (!fp) return;
        char line[64];
        while (fgets(line, sizeof(line), fp)) {
            int v = 0;
            if (sscanf(line, "align=%d", &v) == 1 && v >= 0 && v <= 2)
                m_align = (TextAlign)v;
            else if (sscanf(line, "ui_mode=%d", &v) == 1 &&
                     v >= 0 && v < (int)UiMode::Count)
                m_ui_mode = (UiMode)v;
            else if (sscanf(line, "antialias=%d", &v) == 1)
                m_antialias = (v != 0);
            else if (sscanf(line, "tile_cols=%d", &v) == 1)
                m_tile_cols = std::min(std::max(kTileColsMin, v), kTileColsMax);
            else if (sscanf(line, "tile_rows=%d", &v) == 1)
                m_tile_rows = std::min(std::max(kTileRowsMin, v), kTileRowsMax);
            else if (sscanf(line, "list_icons=%d", &v) == 1)
                m_list_icons = (v != 0);
            else if (sscanf(line, "shelf_vertical=%d", &v) == 1)
                m_shelf_vertical = (v != 0);
            else if (sscanf(line, "wrap_nav=%d", &v) == 1)
                m_wrap_nav = (v != 0);
            else if (sscanf(line, "show_hints=%d", &v) == 1)
                m_show_hints = (v != 0);
            else if (sscanf(line, "show_counter=%d", &v) == 1)
                m_show_counter = (v != 0);
            else if (sscanf(line, "shortcuts_everywhere=%d", &v) == 1)
                m_shortcuts_everywhere = (v != 0);
            else if (sscanf(line, "retroarch=%d", &v) == 1)
                m_retroarch = (v != 0);
            else if (sscanf(line, "sgdb=%d", &v) == 1)
                m_sgdb_enabled = (v != 0);
            else if (sscanf(line, "tdb_region=%d", &v) == 1)
                m_tdb_region = std::clamp(v, 0, kTdbRegionCount - 1);
            else if (sscanf(line, "check_updates=%d", &v) == 1)
                m_check_updates = (v != 0);
            else if (sscanf(line, "welcome=%d", &v) == 1)
                m_welcome_enabled = (v != 0);
            else if (strncmp(line, "lang=", 5) == 0) {
                char code[8] = {};
                if (sscanf(line + 5, "%7s", code) == 1) {
                    // Only accept a code we actually offer; anything else (a
                    // hand-edited file, a language dropped from a later build)
                    // falls back to following the console.
                    for (int i = 0; i < kLangN; i++) {
                        if (strcmp(code, kLangs[i].code) != 0) continue;
                        m_lang_idx = i;
                        strncpy(m_lang, code, sizeof(m_lang) - 1);
                        m_lang[sizeof(m_lang) - 1] = '\0';
                        break;
                    }
                }
            }
        }
        fclose(fp);

        // Apply whatever we ended up with. Harmless when it is "auto": that is
        // exactly what main() already asked for at startup.
        LocaleInit(m_lang);
    }
    void Menu::SaveSettings() {
        EnsureUserConfigDir();
        const std::string path = GetUserConfigPath("settings.txt");
        FILE *fp = fopen(path.c_str(), "w");
        if (!fp) return;
        fprintf(fp, "align=%d\n", (int)m_align);
        fprintf(fp, "ui_mode=%d\n", (int)m_ui_mode);
        fprintf(fp, "antialias=%d\n", m_antialias ? 1 : 0);
        fprintf(fp, "tile_cols=%d\n", m_tile_cols);
        fprintf(fp, "tile_rows=%d\n", m_tile_rows);
        fprintf(fp, "list_icons=%d\n", m_list_icons ? 1 : 0);
        fprintf(fp, "shelf_vertical=%d\n", m_shelf_vertical ? 1 : 0);
        fprintf(fp, "wrap_nav=%d\n", m_wrap_nav ? 1 : 0);
        fprintf(fp, "show_hints=%d\n", m_show_hints ? 1 : 0);
        fprintf(fp, "show_counter=%d\n", m_show_counter ? 1 : 0);
        fprintf(fp, "shortcuts_everywhere=%d\n", m_shortcuts_everywhere ? 1 : 0);
        fprintf(fp, "retroarch=%d\n", m_retroarch ? 1 : 0);
        fprintf(fp, "check_updates=%d\n", m_check_updates ? 1 : 0);
        fprintf(fp, "tdb_region=%d\n", m_tdb_region);
        fprintf(fp, "sgdb=%d\n", m_sgdb_enabled ? 1 : 0);
        fprintf(fp, "welcome=%d\n", m_welcome_enabled ? 1 : 0);
        fprintf(fp, "lang=%s\n", m_lang);
        fclose(fp);
    }
    void Menu::LoadNames() {
        m_names.clear();
        FILE *fp = fopen(GetUserConfigPath("names.txt").c_str(), "r");
        if (!fp) return;
        char line[160];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            u64 id = strtoull(line, nullptr, 16);
            if (id && eq[1]) m_names.emplace_back(id, std::string(eq + 1));
        }
        fclose(fp);
    }
    void Menu::SaveNames() {
        EnsureUserConfigDir();
        const std::string path = GetUserConfigPath("names.txt");
        FILE *fp = fopen(path.c_str(), "w");
        if (!fp) return;
        for (auto &n : m_names)
            fprintf(fp, "%016llX=%s\n", (unsigned long long)n.first, n.second.c_str());
        fclose(fp);
    }
    const std::string *Menu::CustomName(u64 app_id) const {
        for (auto &n : m_names)
            if (n.first == app_id) return &n.second;
        return nullptr;
    }
    void Menu::SetCustomName(u64 app_id, const char *name) {
        for (auto it = m_names.begin(); it != m_names.end(); ++it) {
            if (it->first == app_id) {
                if (name && name[0]) it->second = name;
                else                 m_names.erase(it); // empty clears the rename
                SaveNames();
                return;
            }
        }
        if (name && name[0]) m_names.emplace_back(app_id, std::string(name));
        SaveNames();
    }
    void Menu::RenameSelected() {
        if (m_items.empty()) return;
        const MenuItem &it = m_items[m_cursor];
        if (it.kind != ItemKind::Game) return;
        m_kb_purpose = sl::smi::Kb_RenameGame;
        m_kb_app = it.app_id;
        m_kb_text = it.name;
        m_kb_row = 0;
        m_kb_col = 0;
        m_kb_upper = false;
        m_screen = Screen::Keyboard;
    }
    // Switch the menu to another account: the settings go with the person, so
    // this re-points the config layer and reads their files back in rather than
    // leaving the previous account's values loaded (which the next save would
    // then write into the new account's folder).
    void Menu::SetUser(AccountUid uid, const char *nickname) {
        m_user = uid;
        strncpy(m_nickname, nickname, 32);
        m_nickname[32] = '\0';

        cfg::SetUser(m_user);
        cfg::NoteNickname(m_nickname);

        m_theme.Load();
        m_theme_cursor = m_theme.CurrentIndex();
        LoadFontConfig();
        LoadFavourites();
        LoadSort();
        LoadOrder();
        LoadTileCfg();
        LoadHbPins();
        LoadHbFavourites();
        LoadSettings();
        LoadSysEntries();
        LoadNames();
        LoadIconPackSetting();
        LoadFlowConfig();
        RebuildItems();
    }
    void Menu::SetStatus(const char *msg) {
        strncpy(m_status, T(msg), 127);   // localized; unknown messages pass through
        m_status[127] = '\0';
        m_status_tick = armGetSystemTick();
    }

    // ---- Per-user config paths ---------------------------------------------
    // Thin wrappers over sl::menu::cfg so the ten or so call sites in the menu
    // read the same as they always did. The account is chosen once, in main,
    // before the first config file is read; see UserCfg.hpp for what is per
    // account and what stays with the console.
    std::string Menu::GetUserConfigPath(const char *filename) const {
        return cfg::Path(filename);
    }

    void Menu::EnsureUserConfigDir() const {
        cfg::EnsureDir();
    }

    // Ask the daemon to show the keyboard: write a request file, then flag the
    // applet to exit so qlaunch can display swkbd and hand the text back.
    // =========================================================================
    // Input
    // What a press sounds like is decided here, once, rather than scattered
    // through twenty handlers. The handler runs first and the cue is chosen from
    // what it did: an Action that leaves the menu is a confirmation, a handler
    // can ask for one explicitly via m_sfx_confirm (theme saves and the like),
    // anything else that took an A is an ordinary click, and B is always Back.
    //
    // Deciding after the fact is what keeps a launch from playing both a click
    // and a confirmation, which is what happens if each site plays its own.
    void Menu::PlayButtonSfx(Btn b, Action a) {
        switch (a) {
            case Action::LaunchApp:
            case Action::ResumeApp:
            case Action::LaunchHomebrew:
            case Action::LaunchHomebrewApp:
            case Action::PowerSleep:
            case Action::PowerReboot:
            case Action::PowerShutdown:
            case Action::PowerPayload:
            case Action::TerminateApp:
            case Action::FinishSetup:
                m_sfx.Play(audio::Sfx::Confirm);
                return;
            default:
                break;
        }
        if (m_sfx_confirm)   { m_sfx.Play(audio::Sfx::Confirm); return; }
        if (b == Btn::A)     { m_sfx.Play(audio::Sfx::Click);   return; }
        if (b == Btn::B)     { m_sfx.Play(audio::Sfx::Back);    return; }
    }
    // The actual per-screen routing, shared by OnButton (real button presses)
    // and OnTouch (a tap standing in for one). Split out so a tap gets the
    // same PlayButtonSfx() cue a button press does - OnTouch used to call
    // OnButtonMain/OnButtonDeckLibrary/etc directly, which reached the right
    // screen handler but skipped the sound entirely, because PlayButtonSfx()
    // used to be called only here, from what was then OnButton's own body.
    Menu::Action Menu::DispatchButton(Btn b, u64 &out_app_id) {
        out_app_id = 0;

        // Cleared before each dispatch; a handler raises it when its press earns
        // the confirmation cue rather than the ordinary click.
        m_sfx_confirm = false;

        Action a = Action::None;
        if (m_options_open)               a = OnButtonOptions(b, out_app_id);
        else if (m_dialog != Dialog::None) a = OnButtonDialog(b, out_app_id);
        else switch (m_screen) {
            case Screen::Oobe:          a = OnButtonOobe(b);          break;
            case Screen::Main:          a = OnButtonMain(b, out_app_id); break;
            case Screen::Theming:       a = OnButtonTheming(b);       break;
            case Screen::Themes:        a = OnButtonThemes(b);        break;
            case Screen::ThemeEditor:   a = OnButtonEditor(b);        break;
            case Screen::ColorPicker:   a = OnButtonColorPicker(b);   break;
            case Screen::Fonts:         a = OnButtonFonts(b);         break;
            case Screen::Widgets:       a = OnButtonWidgets(b);       break;
            case Screen::WidgetOptions: a = OnButtonWidgetOptions(b); break;
            case Screen::Keyboard:      a = OnButtonKeyboard(b);      break;
            case Screen::Music:         a = OnButtonMusic(b);         break;
            case Screen::Homebrew:      a = OnButtonHomebrew(b);      break;
            case Screen::Album:         a = OnButtonAlbum(b);         break;
            case Screen::Files:         a = OnButtonFiles(b);         break;
            case Screen::FlowMenu:      a = OnButtonFlowMenu(b, out_app_id); break;
            case Screen::DeckMenu:      a = OnButtonDeckMenu(b, out_app_id); break;
            case Screen::DeckLibrary:   a = OnButtonDeckLibrary(b, out_app_id); break;
            case Screen::DeckNews:      a = OnButtonDeckNews(b);      break;
            case Screen::FlowSettings:  a = OnButtonFlowSettings(b);  break;
            case Screen::About:         a = OnButtonAbout(b);         break;
            case Screen::Welcome:       a = OnButtonWelcome(b);       break;
            case Screen::SysEntries:    a = OnButtonSysEntries(b);    break;
            case Screen::Network:       a = OnButtonNetwork(b);       break;
            case Screen::CoverPicker:   a = OnButtonCoverPicker(b);   break;
            case Screen::Power:         a = OnButtonPower(b);         break;
            case Screen::Payloads:      a = OnButtonPayloads(b);      break;
        }

        PlayButtonSfx(b, a);
        return a;
    }

    Menu::Action Menu::OnButton(Btn b, u64 &out_app_id) {
        out_app_id = 0;
        if (m_sd_removed) return Action::None;   // frozen: awaiting reboot
        return DispatchButton(b, out_app_id);
    }

    Menu::Action Menu::OnHomeButton(u64 &out_app_id) {
        out_app_id = 0;
        if (m_sd_removed)                 return Action::None;
        if (m_launch_tick != 0)           return Action::None;  // already launching
        if (m_options_open)               return Action::None;
        if (m_dialog != Dialog::None)     return Action::None;

        if (m_ui_mode == UiMode::Deck && m_screen == Screen::Main) {
            m_screen = Screen::DeckMenu;   // same as pressing Minus on this screen
            m_deck_menu_cursor = 0;
            m_sub_scroll = 0;
            return Action::None;
        }
        if (m_suspended != 0) return Action::ResumeApp;
        return Action::None;
    }
    // What an A press on m_items[m_cursor] does, independent of how it was
    // reached - the home screen's own A, or DeckMenu/FlowMenu handing off
    // their own selection to it. See the call site in OnButtonMain for why
    // this is not just inlined there.
    Menu::Action Menu::ActivateSelected(u64 &out_app_id) {
        const MenuItem &it = m_items[m_cursor];

        switch (it.kind) {
            // A live tile is something to look at, not somewhere to go.
            case ItemKind::WidgetTile:
                return Action::None;
            case ItemKind::Game:
                if (m_suspended != 0 && it.app_id == m_suspended) {
                    if (m_ui_mode == UiMode::Flow) {
                        StartLaunchAnim(Action::ResumeApp, it.app_id);
                        return Action::None;
                    }
                    return Action::ResumeApp;
                }
                if (m_suspended != 0) {
                    m_pending_launch = it.app_id;
                    m_dialog_cursor  = 1;
                    m_dialog_title.clear();   // default "Close running application?"
                    m_dialog_note.clear();
                    m_dialog = Dialog::ConfirmCloseForLaunch;
                    return Action::None;
                }
                if (m_ui_mode == UiMode::Flow) {
                    StartLaunchAnim(Action::LaunchApp, it.app_id);
                    return Action::None;
                }
                out_app_id = it.app_id;
                return Action::LaunchApp;
            case ItemKind::RandomGame: {
                const u64 pick = RollRandomGame();   // animates, then lands
                if (pick == 0) { SetStatus("No games to pick from"); return Action::None; }
                if (m_suspended != 0 && pick == m_suspended) return Action::ResumeApp;
                if (m_suspended != 0) {
                    m_pending_launch = pick;
                    m_dialog_cursor  = 1;
                    m_dialog_title.clear();
                    m_dialog_note.clear();
                    m_dialog = Dialog::ConfirmCloseForLaunch;
                    return Action::None;
                }
                out_app_id = pick;
                return Action::LaunchApp;
            }
            case ItemKind::Theming:
                m_screen = Screen::Theming;
                m_theming_cursor = 0;
                m_sub_scroll = 0;
                return Action::None;
            case ItemKind::Themes:
                m_screen = Screen::Themes;
                m_theme_cursor = m_theme.CurrentIndex();
                return Action::None;
            case ItemKind::Fonts:
                m_screen = Screen::Fonts;
                m_font_cursor = m_font_applied;
                return Action::None;
            case ItemKind::Album:        OpenAlbumViewer(); return Action::None;
            case ItemKind::MusicPlayer:  OpenMusicPlayer(false); return Action::None;
            case ItemKind::UserPage:     return Action::OpenUserPage;
            case ItemKind::WebBrowser:   return Action::OpenWebBrowser;
            case ItemKind::MiiEdit:      return Action::OpenMiiEdit;
            case ItemKind::Controllers:  return Action::OpenControllers;
            case ItemKind::HomebrewMenu: OpenHomebrewBrowser(); return Action::None;
            case ItemKind::FileManager:  OpenFileManager(); return Action::None;
            case ItemKind::Homebrew:
                m_hb_launch_path = it.hb_path;
                m_hb_launch_argv = it.hb_argv;
                // Run as an application if a donor is set, else as an applet.
                return m_hb_donor ? Action::LaunchHomebrewApp : Action::LaunchHomebrew;
            // Retired as an entry: Network does this in the menu instead
            // of throwing you into the system applet. The kind itself has
            // to stay - it numbers the saved hide list and it is the icon
            // the XMB Settings column and the OOBE step row borrow - so
            // this stays with it, unreachable.
            case ItemKind::Settings:     return Action::OpenNetConnect;
            case ItemKind::Wifi:
                m_screen = Screen::Network; m_net_cursor = 0; m_sub_scroll = 0;
                RefreshNetwork(true);
                return Action::None;
            case ItemKind::Power:        EnterPower(); return Action::None;
        }
        return Action::None;
    }
    Menu::Action Menu::OnButtonMain(Btn b, u64 &out_app_id) {
        // The bounce is playing and the launch is already committed; scrolling
        // away underneath it would animate the wrong case.
        if (m_launch_tick != 0) return Action::None;
        if (m_items.empty()) {
            if (b == Btn::Plus) EnterPower();
            // A search that matched nothing empties the list, and every other
            // way out of the main screen is an entry in it - so without these
            // two the only thing left on the console is the power menu.
            if (b == Btn::Y) OpenSearch();
            if (b == Btn::B && !m_search.empty()) {
                m_search.clear();
                RebuildItems();
                m_xmb_placed = false;
                SetStatus(T("Search cleared"));
            }
            return Action::None;
        }
        // Move mode: the D-pad reorders the held entry instead of navigating.
        if (m_move_mode) {
            // On the tile wall a row is a real row, so up and down have to cross
            // one. Stepping the entry a single slot along the flat list - which
            // is all a 1-D layout needs - just nudged the tile sideways, so an
            // entry could never be moved vertically at all.
            //
            // The distance is whatever the packed layout says (it varies: rows
            // hold different numbers of entries once wide tiles are mixed in),
            // and it is walked one adjacent swap at a time, so everything in
            // between shifts by one exactly as a single press does.
            if (m_ui_mode == UiMode::Grid && (b == Btn::Up || b == Btn::Down)) {
                int cur = -1;
                for (int i = 0; i < (int)m_items.size(); i++)
                    if (ItemKey(m_items[i]) == m_move_key) { cur = i; break; }
                if (cur >= 0) {
                    const int tgt  = TileNeighbour(b == Btn::Up ? 2 : 3);
                    const int step = (tgt > cur) ? +1 : -1;
                    for (int i = cur; i != tgt; i += step) MoveSelected(step);
                }
                return Action::None;
            }
            if (b == Btn::Left  || b == Btn::Up)   { MoveSelected(-1); return Action::None; }
            if (b == Btn::Right || b == Btn::Down) { MoveSelected(+1); return Action::None; }
            if (b == Btn::A) { m_move_mode = false; SaveOrder(); SetStatus("Order saved"); }
            if (b == Btn::B) { m_move_mode = false; LoadOrder(); RebuildItems(); SelectByKey(m_move_key); }
            return Action::None;
        }
        if (b == Btn::L) m_sfx.Play(audio::Sfx::PageLeft);
        if (b == Btn::R) m_sfx.Play(audio::Sfx::PageRight);
        // Carousel navigation: the selected item is always centred, so we only
        // move the cursor. Scrolling stops at the ends; a *fresh* press (not an
        // auto-repeat) at an end wraps around, so holding the stick can't spin
        // the list endlessly.
        const int last = (int)m_items.size() - 1;
        auto step     = [&](int d) { m_cursor = std::min(std::max(0, m_cursor + d), last); };
        // Wrapping only ever happens on a fresh press, so holding a direction
        // stops at the end instead of looping forever - and it can be turned off
        // entirely under Theming.
        auto wrapNext = [&]() {
            if (m_cursor < last)    m_cursor++;
            else if (m_nav_fresh && m_wrap_nav) { m_cursor = 0;    m_scroll_pos = 0.0f; }
        };
        auto wrapPrev = [&]() {
            if (m_cursor > 0)       m_cursor--;
            else if (m_nav_fresh && m_wrap_nav) { m_cursor = last; m_scroll_pos = (float)last; }
        };

        // Navigation depends on the layout: the text List and the Line cover
        // carousel are 1-D; the Grid moves in two dimensions by whole rows.
        if (m_ui_mode == UiMode::Grid) {
            // Every direction is resolved against the packed tile layout rather
            // than against index arithmetic: with mixed tile widths a row is no
            // longer a fixed number of entries, so "the one above" is a question
            // only the geometry can answer.
            if (b == Btn::Right) { m_cursor = TileNeighbour(1); return Action::None; }
            if (b == Btn::Left)  { m_cursor = TileNeighbour(0); return Action::None; }
            if (b == Btn::Up)    { m_cursor = TileNeighbour(2); return Action::None; }
            if (b == Btn::Down)  { m_cursor = TileNeighbour(3); return Action::None; }
            // Shoulders page by a screenful of rows.
            if (b == Btn::R || b == Btn::L) {
                const int row = TileRowOf(m_cursor)
                              + (b == Btn::R ? TileRowsVis() : -TileRowsVis());
                m_cursor = TileFirstInRow(std::min(std::max(0, row),
                                                   std::max(0, TileRowCount() - 1)));
                return Action::None;
            }
            // Y works the music tile without leaving the wall - the point of a
            // live tile is that you do not have to open it.
            if (b == Btn::Y && m_items[m_cursor].kind == ItemKind::MusicPlayer
                    && m_deferred_joined) {
                m_music.SetEnabled(!m_music.Enabled());
                return Action::None;
            }
        } else if (m_ui_mode == UiMode::XMB) {
            // Y searches, B clears an active search. A thousand-entry column is
            // not something you scroll to the end of, and the letter jump only
            // helps if you know the first letter.
            if (b == Btn::Y) { OpenSearch(); return Action::None; }
            if (b == Btn::B && !m_search.empty()) {
                m_search.clear();
                RebuildItems();
                m_xmb_placed = false;      // reopen on Games, as a fresh bar does
                SetStatus(T("Search cleared"));
                return Action::None;
            }
            // Cross-media bar: left/right rides the category bar, up/down walks
            // the selected category's column. Both keep m_cursor pointing at the
            // same entry, so A/X and the options overlay need no special case.
            if (m_xmb_cols.empty()) return Action::None;
            if (m_xmb_col < 0) XmbSyncFromCursor();
            if (m_xmb_col < 0) { m_xmb_col = 0; m_xmb_item = 0; }

            const int ncols = (int)m_xmb_cols.size();
            const int ncur  = (int)m_xmb_cols[m_xmb_col].items.size();

            // Columns clamp at the ends like the handheld's: there are only a
            // few and they are all on screen, so wrapping would just disorient.
            if (b == Btn::Right || b == Btn::Left) {
                const int dir = (b == Btn::Right) ? +1 : -1;
                const int next = m_xmb_col + dir;
                if (next >= 0 && next < ncols) {
                    m_xmb_col  = next;
                    // Land on the entry you were last on in that column would be
                    // nice, but the handheld always opens a column at the top.
                    m_xmb_item = 0;
                    m_xmb_item_scroll = 0.0f;
                    XmbApplyCursor();
                }
                return Action::None;
            }
            // The column does wrap on a fresh press: a library can be hundreds
            // of entries long and reaching the end from the top is otherwise a
            // very long hold. Auto-repeat still stops at the ends.
            if (b == Btn::Down) {
                if (m_xmb_item < ncur - 1)   m_xmb_item++;
                else if (m_nav_fresh)      { m_xmb_item = 0; m_xmb_item_scroll = 0.0f; }
                XmbApplyCursor();
                return Action::None;
            }
            if (b == Btn::Up) {
                if (m_xmb_item > 0)          m_xmb_item--;
                else if (m_nav_fresh)      { m_xmb_item = ncur - 1; m_xmb_item_scroll = (float)m_xmb_item; }
                XmbApplyCursor();
                return Action::None;
            }
            // Shoulders page by five everywhere except a shortcut category,
            // where they jump by initial instead. Paging five is useless in a
            // scanned ROM library - reaching the S's from the A's is four
            // hundred presses - but it is the right thing everywhere else: a
            // letter jump only means something in a list that is ordered by
            // name, and Games follows whatever sort mode is set while the
            // system column is a handful of entries in no alphabetical order
            // at all. A non-empty label is exactly "this column is a shortcut
            // category", which is the only list built name-sorted.
            if (b == Btn::R || b == Btn::L) {
                const int dir = (b == Btn::R) ? +1 : -1;
                m_xmb_item = m_xmb_cols[m_xmb_col].label.empty()
                           ? std::min(std::max(0, m_xmb_item + dir * 5), ncur - 1)
                           : XmbLetterJump(dir);
                XmbApplyCursor();
                return Action::None;
            }
        } else if (m_ui_mode == UiMode::Flow) {
            // The shelf holds games only, so navigation walks m_flow_items and
            // maps back to the shared cursor.
            const int fn = (int)m_flow_items.size();
            if (fn > 0) {
                int at = 0;
                for (int i = 0; i < fn; i++)
                    if (m_flow_items[i] == m_cursor) { at = i; break; }
                int delta = 0;
                if (b == Btn::Right || b == Btn::Down) delta = +1;
                if (b == Btn::Left  || b == Btn::Up)   delta = -1;
                if (b == Btn::R) delta = +5;
                if (b == Btn::L) delta = -5;
                if (delta) {
                    // Taking hold of the row with a button stops it coasting,
                    // the same as putting a finger on it. Without this the fling
                    // kept overwriting the cursor and the press did nothing.
                    m_fling_vel = 0.0f;

                    if (m_wrap_nav) {
                        at = (at + delta % fn + fn) % fn;
                    } else {
                        at += delta;
                        if (at < 0) at = 0;
                        if (at > fn - 1) at = fn - 1;
                    }
                    m_cursor = m_flow_items[at];
                    return Action::None;
                }
            }
            if (b == Btn::Minus) {
                m_screen = Screen::FlowMenu;
                m_flow_menu_cursor = 0;
                m_sub_scroll = 0;
                return Action::None;
            }
        } else if (m_ui_mode == UiMode::Deck) {
            // Deck moves between rows - covers, tabs, cards - so its navigation
            // lives in one place rather than being spread across the direction
            // tests here. Anything it does not claim (A on the cover row) falls
            // through to the shared handling below, which is what launches.
            Action da = Action::None;
            if (DeckNav(b, da, out_app_id)) return da;
        } else if (m_ui_mode == UiMode::Line || m_ui_mode == UiMode::Cover ||
                   m_ui_mode == UiMode::Shelf) {
            if (b == Btn::Right || b == Btn::Down) { wrapNext(); return Action::None; }
            if (b == Btn::Left  || b == Btn::Up)   { wrapPrev(); return Action::None; }
            if (b == Btn::R) { step(5);  return Action::None; }
            if (b == Btn::L) { step(-5); return Action::None; }
        } else { // List (text carousel)
            if (b == Btn::Down) { wrapNext(); return Action::None; }
            if (b == Btn::Up)   { wrapPrev(); return Action::None; }
            if (b == Btn::R) { step(5);  return Action::None; }
            if (b == Btn::L) { step(-5); return Action::None; }
        }

        // Whatever m_cursor points to, activated - factored out of the A-press
        // handling below so DeckMenu/FlowMenu (their own A opens whatever their
        // own cursor points to) can call exactly this and nothing else. They
        // used to call OnButtonMain(Btn::A, ...) itself, which re-enters the
        // Deck/Flow mode-routing above this point too - and that routing reads
        // m_deck_row/m_flow_menu_cursor-style state left over from whatever the
        // row/tab layout was doing before the side menu opened, so the same
        // press could silently be swallowed by that instead of ever reaching
        // here, or worse, mutate that unrelated state as a side effect.
        if (b == Btn::A) return ActivateSelected(out_app_id);

        if (b == Btn::X) {
            m_options_sub = Sub_None;
            BuildOptions();
            m_options_cursor = 0;
            if (!m_options.empty()) m_options_open = true;
            return Action::None;
        }
        if (b == Btn::Plus) { EnterPower(); return Action::None; }
        return Action::None;
    }
    // Widgets that are not already on the wall.
    int Menu::UnplacedWidgets() {
        if (!m_deferred_joined) return 0;
        int n = 0;
        for (int i = 0; i < m_widgets.Count(); i++) {
            widgets::IWidget *w = m_widgets.At(i);
            if (w && !m_tilecfg.count("w" + w->Name())) n++;
        }
        return n;
    }
    void Menu::BuildOptions() {
        m_options.clear();
        if (m_items.empty()) return;

        // A submenu replaces the list rather than nesting a second overlay:
        // same box, same input handling, one flag.
        if (m_options_sub == Sub_Widgets) {
            for (int i = 0; i < m_widgets.Count(); i++) {
                widgets::IWidget *w = m_widgets.At(i);
                if (!w || m_tilecfg.count("w" + w->Name())) continue;
                m_options.push_back({ w->Name(), OptAddWidget, i });
            }
            m_options.push_back({ T("Back"), OptSubBack });
            return;
        }

        const MenuItem &it = m_items[m_cursor];
        if (it.kind == ItemKind::Game) {
            m_options.push_back({ IsFavourite(it.app_id) ? T("Remove from Favourites")
                                                         : T("Add to Favourites"), OptFav });
            m_options.push_back({ T("Rename"), OptRename });
            m_options.push_back({ T("Choose cover"), OptPickCover });
            m_options.push_back({ m_hb_donor == it.app_id ? T("Homebrew donor (set)")
                                                          : T("Use as homebrew donor"), OptSetDonor });
        }
        if (it.kind == ItemKind::Homebrew) {
            m_options.push_back({ IsHbFavourite(it.hb_path) ? T("Remove from Favourites")
                                                            : T("Add to Favourites"), OptFav });
            m_options.push_back({ T("Remove from menu"), OptUnpinHb });
        }
        // Any entry can be reordered.
        m_options.push_back({ T("Move"), OptMove });

        // Tile options only mean anything on the wall, so they are not offered
        // in the layouts that have no tiles.
        if (m_ui_mode == UiMode::Grid) {
            const std::string key = ItemKey(it);
            m_options.push_back({ std::string(T("Tile size: ")) + T(TileSizeLabel(key)),
                                  OptTileSize });
            m_options.push_back({ T("Tile colour"), OptTileColor });
            const auto tc = m_tilecfg.find(key);
            if (tc != m_tilecfg.end() && tc->second.has_color)
                m_options.push_back({ T("Default colour"), OptTileReset });
            if (it.kind == ItemKind::WidgetTile)
                m_options.push_back({ T("Remove tile"), OptRemoveWidget });

            // Behind a submenu: one line per widget would push everything else
            // off the bottom of the overlay as soon as a couple of scripts are
            // on the card.
            if (m_deferred_joined && UnplacedWidgets() > 0)
                m_options.push_back({ T("Add widget"), OptAddWidgetMenu });
        }
        if (it.kind == ItemKind::Game && m_suspended != 0 && it.app_id == m_suspended)
            m_options.push_back({ T("Close game"), OptCloseGame });
        m_options.push_back({ std::string(T("Sort: ")) + SortLabel(), OptSort });
        m_options.push_back({ T("Cancel"), OptDismiss });
    }
    const char *Menu::SortLabel() const {
        switch (m_sort) {
            case SortMode::TitleAsc:       return T("Title A - Z");
            case SortMode::TitleDesc:      return T("Title Z - A");
            case SortMode::GamecardFirst:  return T("Game card first");
            case SortMode::RecentlyPlayed: return T("Recently played");
            case SortMode::MostPlayed:     return T("Most played");
            default:                       return T("Default");
        }
    }
    Menu::Action Menu::OnButtonOptions(Btn b, u64 &out_app_id) {
        (void)out_app_id;
        int n = (int)m_options.size();
        if (n == 0) { m_options_open = false; return Action::None; }
        if (b == Btn::Down) { m_options_cursor = (m_options_cursor + 1) % n; return Action::None; }
        if (b == Btn::Up)   { m_options_cursor = (m_options_cursor + n - 1) % n; return Action::None; }
        if (b == Btn::B || b == Btn::X) {
            if (m_options_sub != Sub_None) {   // back to the entry's own options
                m_options_sub = Sub_None;
                BuildOptions();
                m_options_cursor = 0;
                return Action::None;
            }
            m_options_open = false;
            return Action::None;
        }
        if (b != Btn::A) return Action::None;

        // Capture the selection before RebuildItems can invalidate references.
        const MenuItem &sel = m_items[m_cursor];
        const bool sel_is_game = (sel.kind == ItemKind::Game);
        const u64  sel_id      = sel.app_id;
        const std::string sel_key  = ItemKey(sel);
        const std::string sel_hb   = sel.hb_path;

        switch (m_options[m_options_cursor].action) {
            case OptFav: {
                bool now_fav;
                if (sel_is_game) { now_fav = !IsFavourite(sel_id);   ToggleFavourite(sel_id); }
                else             { now_fav = !IsHbFavourite(sel_hb); ToggleHbFavourite(sel_hb); }
                RebuildItems();
                SelectByKey(sel_key);   // generic: works for games and homebrew
                SetStatus(now_fav ? "Added to Favourites" : "Removed from Favourites");
                m_options_open = false;
                return Action::None;
            }
            case OptRename:
                m_options_open = false;
                RenameSelected();   // shows the software keyboard, then rebuilds
                return Action::None;
            case OptMove:
                // Custom order only takes effect in Default sort.
                if (m_sort != SortMode::Default) { m_sort = SortMode::Default; SaveSort(); RebuildItems(); }
                m_move_mode = true;
                m_move_key  = sel_key;
                SelectByKey(sel_key);
                SetStatus("Move: D-pad to reorder, A to place");
                m_options_open = false;
                return Action::None;
            case OptUnpinHb:
                ToggleHbPin(sel_hb);   // remove from the main menu
                SetStatus("Removed from menu");
                m_options_open = false;
                return Action::None;
            case OptSetDonor:
                m_hb_donor = (m_hb_donor == sel_id) ? 0 : sel_id;   // toggle
                SaveHbDonor();
                SetStatus(m_hb_donor ? "Homebrew donor set (browser Y: run as app)"
                                     : "Homebrew donor cleared");
                m_options_open = false;
                return Action::None;
            case OptSort:
                m_sort = (SortMode)(((int)m_sort + 1) % (int)SortMode::Count);
                SaveSort();
                RebuildItems();
                SelectByKey(sel_key);
                BuildOptions(); // refresh the sort label, keep the menu open
                if (m_options_cursor >= (int)m_options.size())
                    m_options_cursor = (int)m_options.size() - 1;
                return Action::None;
            case OptPickCover:
                m_options_open = false;
                // Opened from Flow's own menu, B should return there, not to the
                // shelf - the same rule every screen reached that way follows.
                m_from_flow_menu = (m_screen != Screen::Main);
                EnterCoverPicker();
                return Action::None;
            case OptCloseGame:
                m_options_open = false;
                return Action::TerminateApp;
            case OptTileSize:
                CycleTileSize(sel_key);
                SetStatus(TileSizeLabel(sel_key));
                m_options_open = false;
                return Action::None;
            case OptTileColor: {
                TileCfg &c = TileCfgFor(sel_key);
                // Start from whatever the tile shows now, so the picker opens on
                // the current colour instead of black.
                if (!c.has_color) { c.color = TileColor(m_cursor); c.has_color = true; }
                m_pick_tile = true;
                // No theme preview: this colour is not part of the theme, and
                // re-selecting the theme would only rebuild the wallpaper blur.
                OpenColorPicker(&c.color, Screen::Main, false);
                m_options_open = false;
                return Action::None;
            }
            case OptTileReset: {
                auto c = m_tilecfg.find(sel_key);
                if (c != m_tilecfg.end()) {
                    c->second.has_color = false;
                    // Drop only the entry that carries nothing else, so a widget
                    // tile is not deleted just by resetting its colour.
                    if (c->second.w == 0) m_tilecfg.erase(c);
                    SaveTileCfg();
                }
                m_options_open = false;
                return Action::None;
            }
            case OptAddWidgetMenu:
                m_options_sub = Sub_Widgets;
                BuildOptions();
                m_options_cursor = 0;
                return Action::None;
            case OptSubBack:
                m_options_sub = Sub_None;
                BuildOptions();
                m_options_cursor = 0;
                return Action::None;
            case OptAddWidget:
                AddWidgetTile(m_options[m_options_cursor].arg);
                SetStatus("Widget added");
                m_options_sub  = Sub_None;
                m_options_open = false;
                return Action::None;
            case OptRemoveWidget:
                RemoveWidgetTile(sel.name);
                m_options_open = false;
                return Action::None;
            case OptDismiss:
            default:
                m_options_open = false;
                return Action::None;
        }
    }

    // ---- Theming submenu ---------------------------------------------------
    void Menu::UpdateCheckTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        std::string body;
        if (net::Get("https://api.github.com/repos/etonedemid/slaunch/releases/latest",
                     body, 15)) {
            std::string tag = JsonStr(body, "tag_name");
            if (!tag.empty() && CmpVer(tag.c_str(), SL_VERSION) > 0) {
                m->m_upd_latest    = tag;
                m->m_upd_available = true;
            }
        }
        m->m_upd_done.store(true, std::memory_order_release);
    }
    void Menu::StartUpdateCheck() {
        if (!m_check_updates || m_upd_running || m_upd_available) return;
        m_upd_done.store(false, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_upd_thread, &Menu::UpdateCheckTrampoline, this,
                                     nullptr, 0x8000, 0x3B, -2))) {
            threadStart(&m_upd_thread);
            m_upd_running = true;
        }
    }
    void Menu::PollUpdateCheck() {
        if (!m_upd_running || !m_upd_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_upd_thread);
        threadClose(&m_upd_thread);
        m_upd_running = false;
    }
    void Menu::EnterWelcome() {
        m_screen        = Screen::Welcome;
        m_welcome_start = armGetSystemTick();
        m_welcome_msg   = (int)(randomGet64() % kWelcomeMsgN);
    }

    // ---- system entry visibility -------------------------------------------
    // kSysEntries / kSysEntryN live near the top of the file (the touch code
    // needs the row count).
    void Menu::StartLaunchAnim(Action a, u64 app_id) {
        m_launch_tick   = armGetSystemTick();
        m_launch_fired  = false;
        m_launch_action = a;
        m_launch_id     = app_id;
    }
    float Menu::LaunchAnimT() const {
        if (m_launch_tick == 0) return -1.0f;
        const u64 ms = (armGetSystemTick() - m_launch_tick) * 1000 / armGetSystemTickFreq();
        const float t = (float)ms / (float)kLaunchMs;
        return (t > 1.0f) ? 1.0f : t;
    }
    Menu::Action Menu::TakePendingAction(u64 &out_app_id) {
        const Action a = m_pending_action;
        if (a == Action::None) return a;
        out_app_id      = m_pending_id;
        m_pending_action = Action::None;
        m_pending_id     = 0;

        // Cleared here rather than when the animation ended. The black frame has
        // already been drawn and presented by the time the host asks for the
        // action, so on the console - where this is immediately followed by the
        // applet exiting - black is the last thing left on screen. Anywhere the
        // host does not exit (the simulator), the menu simply carries on rather
        // than sitting black forever.
        m_launch_tick   = 0;
        m_launch_fired  = false;
        m_launch_action = Action::None;
        return a;
    }
    // A chainload the daemon could not carry out leaves its reason on the SD;
    // show it once on the next menu start, then drop it.
    void Menu::ShowPowerError() {
        FILE *fp = fopen(sl::smi::PowerErrorPath, "r");
        if (!fp) return;
        char line[160] = {};
        if (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0]) SetStatus(line);
        }
        fclose(fp);
        remove(sl::smi::PowerErrorPath);
    }
    void Menu::LoadPlayCache() {
        FILE *fp = fopen(kPlayCachePath, "r");
        if (!fp) return;
        char line[96];
        while (fgets(line, sizeof(line), fp)) {
            unsigned long long id = 0, secs = 0, last = 0;
            unsigned launches = 0;
            if (sscanf(line, "%llX=%llu,%llu,%u", &id, &secs, &last, &launches) != 4) continue;
            if (id == 0) continue;
            play::PlayInfo pi;
            pi.seconds     = secs;
            pi.last_played = last;
            pi.launches    = launches;
            m_play[id] = pi;
        }
        fclose(fp);
    }
    void Menu::SavePlayCache() const {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/cache", 0777);
        FILE *fp = fopen(kPlayCachePath, "w");
        if (!fp) return;
        for (auto &kv : m_play)
            fprintf(fp, "%016llX=%llu,%llu,%u\n", (unsigned long long)kv.first,
                    (unsigned long long)kv.second.seconds,
                    (unsigned long long)kv.second.last_played, kv.second.launches);
        fclose(fp);
    }
    const play::PlayInfo *Menu::Play(u64 app_id) const {
        if (app_id == 0) return nullptr;          // homebrew / system entries
        auto it = m_play.find(app_id);
        return (it == m_play.end()) ? nullptr : &it->second;
    }
    void Menu::PlayStatsTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        play::Query(m->m_play_ids, m->m_play_result);
        m->m_play_done.store(true, std::memory_order_release);
    }
    void Menu::StartPlayStats() {
        if (m_play_running || m_apps.empty()) return;
        m_play_ids.clear();
        m_play_ids.reserve(m_apps.size());
        for (auto &a : m_apps) m_play_ids.push_back(a.app_id);
        m_play_result.clear();
        m_play_dirty = false;
        m_play_done.store(false, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_play_thread, &Menu::PlayStatsTrampoline, this,
                                     nullptr, 0x8000, 0x3B, -2))) {
            threadStart(&m_play_thread);
            m_play_running = true;
        }
    }
    void Menu::PollPlayStats() {
        if (!m_play_running) {
            // Only after the first frame: play stats are background information and
            // must never hold up the menu appearing.
            if (m_deferred_done && m_play_dirty) StartPlayStats();
            return;
        }
        if (!m_play_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_play_thread);
        threadClose(&m_play_thread);
        m_play_running = false;

        for (size_t i = 0; i < m_play_ids.size() && i < m_play_result.size(); i++)
            m_play[m_play_ids[i]] = m_play_result[i];
        m_play_result.clear();
        SavePlayCache();   // next menu start has these on its first frame

        // The numbers just changed (a game was played since we last looked), so
        // redo the play-based sorts, keeping the cursor on the same entry.
        if (m_sort == SortMode::RecentlyPlayed || m_sort == SortMode::MostPlayed) {
            const std::string key = m_items.empty() ? std::string()
                                                    : ItemKey(m_items[m_cursor]);
            RebuildItems();
            if (!key.empty()) SelectByKey(key);
        }
    }
    // The wallpaper, blurred on the GPU (Gfx::Blurred). The sharp copy is
    // only a stepping stone and is freed again; if render targets are not
    // available the sharp image is returned instead, which beats no wallpaper.
    SDL_Texture *Menu::BlurImage(const char *path) {
        SDL_Texture *sharp = path ? m_gfx->LoadImage(path) : nullptr;
        if (!sharp) return nullptr;
        const int radius = std::clamp(m_theme.Current().wallpaper_blur_radius, 2, 32);
        SDL_Texture *soft = m_gfx->Blurred(sharp, radius);
        if (!soft) return sharp;
        m_gfx->FreeImage(sharp);
        return soft;
    }
    dbg::Counters Menu::DebugCounters() const {
        dbg::Counters dc;
        dc.app_icons = m_icons.Live();
        dc.hb_icons  = m_hb_icons.Live();
        dc.sys_icons = (int)m_sys_icons.size();
        dc.items     = (int)m_items.size();
        dc.widgets   = const_cast<widgets::Widgets &>(m_widgets).Count();
        dc.ui_mode   = (int)m_ui_mode;
        const auto ts = m_gfx->Textures();
        dc.tex_created  = ts.creates;
        dc.tex_failures = ts.failures;
        dc.tex_slots    = ts.slots;
        dc.tex_cached   = ts.cached;
        dc.tex_bytes    = ts.cached_bytes;
        return dc;
    }
    // Turning the overlay off snapshots the numbers to sdmc:/slaunch/debug.log,
    // so a reading can be pulled off the card instead of copied by eye.
    void Menu::ToggleDebugOverlay() {
        const bool was_on = m_debug.Visible();
        m_debug.Toggle();
        if (was_on) {
            m_debug.Dump(DebugCounters());
            SetStatus("Debug snapshot saved");
        }
    }
    // =========================================================================
    // Rendering
    void Menu::EnsureWallpaper() {
        int idx = m_theme.CurrentIndex();
        const Theme &t = m_theme.Current();

        auto reloadBlur = [&]() {
            if (m_wallpaper_blur) { m_gfx->FreeImage(m_wallpaper_blur); m_wallpaper_blur = nullptr; }
            if (t.wallpaper_blur && !m_wallpaper_path.empty())
                m_wallpaper_blur = BlurImage(m_wallpaper_path.c_str());
        };

        // Check if wallpaper path changed -> reload
        if (idx != m_wallpaper_theme) {
            if (m_wallpaper) { m_gfx->FreeImage(m_wallpaper); m_wallpaper = nullptr; }
            if (m_wallpaper_blur) { m_gfx->FreeImage(m_wallpaper_blur); m_wallpaper_blur = nullptr; }
            m_wallpaper_path.clear();
            m_wallpaper_theme = idx;
            m_video_player.Close();

            if (g_sd_ok && t.wallpaper[0]) {
                if (IsVideoPath(t.wallpaper)) {
                    // Blur is not supported for video wallpapers (see
                    // Menu_Screens.cpp's editor, which disables that toggle
                    // when IsVideoPath is true) - re-blurring every decoded
                    // frame live would be a real performance cost for a
                    // cosmetic effect, so it is simply skipped here.
                    m_video_player.Open(m_gfx, t.wallpaper);
                    m_wallpaper_path = t.wallpaper;
                } else {
                    // With blur on, the sharp wallpaper is never drawn - DrawBackground
                    // uses the blurred texture instead - so decoding it is a whole
                    // 1280x720 JPEG of pure waste on every single launch. The blur
                    // comes from the file on its own path, so nothing needs it.
                    const bool need_sharp = !t.wallpaper_blur;
                    m_wallpaper_path = t.wallpaper;
                    if (need_sharp) m_wallpaper = m_gfx->LoadImage(t.wallpaper);
                    reloadBlur();
                }
            }
        } else {
            // Same theme: advance the video decode (a no-op when the active
            // wallpaper is a still image - GetTexture() stays null and
            // Tick() returns immediately).
            m_video_player.Tick();
        }
    }
    void Menu::DrawBackground() {
        const Theme &t = m_theme.Current();
        m_gfx->GradientV(t.bg_top, t.bg_bottom);

        // Wallpaper first (when set), so ribbons draw on top.
        EnsureWallpaper();
        Phase("wallpaper");
        SDL_Texture *video_tex = m_video_player.GetTexture();
        if (m_wallpaper || m_wallpaper_blur || video_tex) {
            const int W = gfx::Gfx::Width;
            const int H = gfx::Gfx::Height;

            // Draw the wallpaper. A video wallpaper never blurs (see
            // EnsureWallpaper); a static image is blurred when that toggle
            // is on and its pre-baked blur texture is ready.
            if (video_tex) {
                m_gfx->DrawCover(video_tex, 255);
            } else if (t.wallpaper_blur && m_wallpaper_blur) {
                m_gfx->DrawCover(m_wallpaper_blur, 255);
            } else {
                m_gfx->DrawCover(m_wallpaper, 255);
            }

            // Dim overlay (independent toggle).
            if (t.wallpaper_dim) {
                m_gfx->FillRect(0, 0, W, H, SDL_Color{0,0,0,90});
            }

            // Snow overlay (independent toggle): on the GPU when it can be.
            if (t.wallpaper_snow && m_gfx->FxBegin(m_gfx->FxProgram(kSnowVs, kSnowFs))) {
                m_gfx->FxQuads(120);
                m_gfx->FxEnd();
            } else if (t.wallpaper_snow) {
                const float elapsed = (float)armGetSystemTick() / (float)armGetSystemTickFreq();
                const int count = 120;
                for (int i = 0; i < count; i++) {
                    const float seed = (float)i * 7.77f;
                    const int baseX = (int)((sinf(seed * 3.1f) * 0.5f + 0.5f) * (float)W);
                    const float speed = 15.0f + sinf(seed * 2.3f) * 10.0f;
                    const float drift = sinf(seed * 5.1f) * 30.0f;
                    const int sz = 1 + (int)((sinf(seed * 1.7f) * 0.5f + 0.5f) * 3.0f);
                    const Uint8 baseA = (Uint8)(60 + (int)((sinf(seed * 4.3f) * 0.5f + 0.5f) * 140));
                    const float yf = fmodf(elapsed * speed + seed * 100.0f, (float)(H + 40)) - 20.0f;
                    const int y = (int)yf;
                    const int x = baseX + (int)(sinf(elapsed * 0.5f + seed) * drift);
                    if (y < 0 || y >= H) continue;
                    m_gfx->FillRect(x, y, sz, sz, SDL_Color{255, 255, 255, baseA});
                }
            }
        }

        switch (t.background_style) {
            case BackgroundStyle_Ribbon: DrawRibbonBackground(m_gfx, t); break;
            case BackgroundStyle_Stars:  DrawStarsBackground(m_gfx, t);  break;
            case BackgroundStyle_Aurora: DrawAuroraBackground(m_gfx, t); break;
            case BackgroundStyle_Grid:   DrawGridBackground(m_gfx, t);   break;
            case BackgroundStyle_RibbonHD: DrawRibbonBackground(m_gfx, t, true); break;
            case BackgroundStyle_Ocean:  DrawOceanBackground(m_gfx, t);  break;
            default: break;   // Gradient: the gradient above is the whole of it
        }
    }
    // XMB header: title hard left on the title margin, clock and battery hard
    // right on the same line. XMB has no centred top bar and no rule beneath it.
    void Menu::DrawXmbHeader(const char *title) {
        const Theme &t = m_theme.Current();
        const int    W = gfx::Gfx::Width;

        if (title && *title)
            m_gfx->Text(FontSize::Title, kXmbTitleLeft, kXmbTitleTop, t.title, title);

        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char clock[32];
        strftime(clock, sizeof(clock), "%H:%M", &tm_now);

        u32 charge = 0;
        psmGetBatteryChargePercentage(&charge);
        PsmChargerType charger = PsmChargerType_Unconnected;
        bool charging = false;
        if (R_SUCCEEDED(psmGetChargerType(&charger)))
            charging = charger != PsmChargerType_Unconnected;
        // Charging marked with a leading '+' rather than a bolt glyph, for the
        // same reason as DrawTopBar: user-supplied fonts draw tofu.
        char batt[24];
        snprintf(batt, sizeof(batt), charging ? "+%lu%%" : "%lu%%",
                 (unsigned long)charge);

        const int bw = m_gfx->TextWidth(FontSize::Small, batt);
        const int cw = m_gfx->TextWidth(FontSize::Small, clock);
        const int hy = kXmbTitleTop + 10;
        m_gfx->Text(FontSize::Small, W - kXmbTitleLeft - bw, hy, t.fg, batt);
        m_gfx->Text(FontSize::Small, W - kXmbTitleLeft - bw - 24 - cw, hy, t.dim, clock);
        DrawUsbTag(W - kXmbTitleLeft - bw - 24 - cw - 24, hy);
    }
    void Menu::DrawTopBar(const char *center_title) {
        const Theme &t = m_theme.Current();

        // In XMB the sub-screens wear the same header as the main screen, so the
        // menu reads as one thing rather than an XMB list under a centred bar.
        if (m_ui_mode == UiMode::XMB) {
            DrawXmbHeader((center_title && *center_title) ? T(center_title) : "");
            return;
        }

        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char clock[32];
        strftime(clock, sizeof(clock), "%H:%M   %a %b %d", &tm_now);
        m_gfx->Text(FontSize::Small, 40, 16, t.dim, clock);

        // Right side: nickname then battery (with charging indicator), laid out
        // by measured width so the gap is even and nothing crowds the screen edge.
        u32 charge = 0;
        psmGetBatteryChargePercentage(&charge);
        PsmChargerType charger = PsmChargerType_Unconnected;
        bool is_charging = false;
        if (R_SUCCEEDED(psmGetChargerType(&charger)))
            is_charging = charger != PsmChargerType_Unconnected;
        // Charging is marked with a leading '+' rather than a bolt glyph: the
        // menu can be rendered in any user-supplied font, and a font without
        // that glyph draws tofu here.
        char batt[24];
        snprintf(batt, sizeof(batt), is_charging ? "+%lu%%" : "%lu%%",
                 (unsigned long)charge);
        char name[24];
        snprintf(name, sizeof(name), "%.20s", m_nickname);

        int bw = m_gfx->TextWidth(FontSize::Small, batt);
        int nw = m_gfx->TextWidth(FontSize::Small, name);
        const int edge = 40, gap = 24;
        m_gfx->Text(FontSize::Small, gfx::Gfx::Width - edge - bw, 16, t.fg, batt);
        m_gfx->Text(FontSize::Small, gfx::Gfx::Width - edge - bw - gap - nw, 16, t.dim, name);
        DrawUsbTag(gfx::Gfx::Width - edge - bw - gap - nw - gap, 16);

        // Localized when it's a known UI title; user data (theme/widget names)
        // passes through T() unchanged.
        if (center_title && center_title[0])
            m_gfx->TextCentered(FontSize::Title, gfx::Gfx::Width / 2, 26, t.title, T(center_title));

        (void)kTopBarH;
    }
    void Menu::DrawHint(const char *hint) {
        if (!m_show_hints) return;
        const Theme &t = m_theme.Current();
        m_gfx->TextCentered(FontSize::Small, gfx::Gfx::Width / 2, kHintY, t.dim, T(hint));
    }

    // Button-prompt hint bar: real icons (Xelu pack, see
    // assets/icons/buttons/ATTRIBUTION.md) instead of a typed "A: Launch".
    // Two passes, same reason DrawTopBar measures before right-aligning the
    // battery/nickname block: the row has to be centred, and there is no way
    // to know its total width without laying it out once first.
    void Menu::DrawHint(std::initializer_list<HintSeg> segs) {
        if (!m_show_hints) return;
        const Theme &t = m_theme.Current();
        constexpr int kIcon = 22, kIconGap = 2, kLabelGap = 7, kSegGap = 26;
        const FontSize fs = FontSize::Small;
        const int lh = m_gfx->LineHeight(fs);

        auto segWidth = [&](const HintSeg &seg) {
            const int n = (int)seg.icons.size();
            return n * kIcon + (n - 1) * kIconGap + kLabelGap
                 + m_gfx->TextWidth(fs, T(seg.label));
        };
        int total = 0;
        bool first = true;
        for (const HintSeg &seg : segs) {
            if (!first) total += kSegGap;
            first = false;
            total += segWidth(seg);
        }

        int x = gfx::Gfx::Width / 2 - total / 2;
        const int y  = kHintY;
        const int iy = kHintY + (lh - kIcon) / 2;
        first = true;
        for (const HintSeg &seg : segs) {
            if (!first) x += kSegGap;
            first = false;
            for (const char *icon : seg.icons) {
                if (SDL_Texture *tex = HintIcon(icon))
                    m_gfx->DrawImageTinted(tex, x, iy, kIcon, kIcon, t.dim);
                x += kIcon + kIconGap;
            }
            x += kLabelGap - kIconGap;   // undo the last icon's trailing gap
            const char *label = T(seg.label);
            m_gfx->Text(fs, x, y, t.dim, label);
            x += m_gfx->TextWidth(fs, label);
        }
    }

    // The one tappable "go back" every non-Main screen gets, regardless of
    // layout - drawn from Render() so it never has to be added to a Draw*
    // function by hand, and OnTouch's hit-test reads the exact same
    // kBackTap* box, so the two can never drift apart. See the constants'
    // own comment for why this ignores m_show_hints.
    void Menu::DrawBackTap() {
        const Theme &t = m_theme.Current();
        const int icon = 22;
        const int cy = kBackTapY + kBackTapH / 2;
        const int ix = kBackTapX + 4;
        if (SDL_Texture *tex = HintIcon("b"))
            m_gfx->DrawImageTinted(tex, ix, cy - icon / 2, icon, icon, t.dim, 200);
        m_gfx->Text(FontSize::Small, ix + icon + 6, cy - m_gfx->LineHeight(FontSize::Small) / 2,
                    WithAlpha(t.dim, 200), T("Back"));
    }
    // XMB-styled sub-screen list.
    //
    // Same placement curve, zoom and fade as the main XMB column, rendered in
    // RetroArch's "entry icons off" compact form: sMenu has no per-setting
    // artwork, and that is precisely the mode RetroArch itself falls back to
    // when icons are disabled - the whole column shifts left by one icon and
    // the cursor is marked by an arrow on the left margin instead.
    void Menu::DrawCarouselXmb(const std::vector<std::string> &labels,
                               const std::vector<std::string> &values,
                               int cursor, float &scroll_pos, Uint8 alpha) {
        const Theme &t = m_theme.Current();
        const int     W = gfx::Gfx::Width;
        if (labels.empty()) return;

        scroll_pos += (cursor - scroll_pos) * 0.30f;
        if (std::abs(cursor - scroll_pos) < 0.01f) scroll_pos = (float)cursor;

        const int textX  = kXmbAnchorX + kXmbIcon / 2 + kXmbLabelLeft - kXmbIcon;
        const int valueX = kXmbMarginLeft + kXmbSpacingH + kXmbLabelLeft
                         + kXmbSettingLeft - kXmbIcon;

        const int first = std::max(0, (int)scroll_pos - kXmbAbove - 1);
        const int last  = std::min((int)labels.size() - 1,
                                   (int)scroll_pos + kXmbBelow + 1);

        for (int i = first; i <= last; i++) {
            const float d  = (float)i - scroll_pos;
            const int   cy = kXmbMarginTop + kXmbIcon / 2 + (int)XmbRowOffset(d);
            if (cy >= kXmbFadeBotEnd) break;
            if (cy < kXmbFadeEnd)     continue;

            float fade = 1.0f;
            if (cy < kXmbFadeStart)
                fade = (float)(cy - kXmbFadeEnd) / (float)(kXmbFadeStart - kXmbFadeEnd);
            else if (cy > kXmbFadeBotStart)
                fade = (float)(kXmbFadeBotEnd - cy)
                     / (float)(kXmbFadeBotEnd - kXmbFadeBotStart);

            const bool  sel  = (i == cursor);
            const float prox = std::max(0.0f, 1.0f - std::abs(d));
            const float al   = kXmbAlphaPassive + (kXmbAlphaActive - kXmbAlphaPassive) * prox;
            const Uint8 a    = (Uint8)((float)alpha * fade * al);

            const FontSize fs = sel ? FontSize::Normal : FontSize::Small;
            const int      lh = m_gfx->LineHeight(fs);
            m_gfx->Text(fs, textX, cy - lh / 2,
                        WithAlpha(sel ? t.title : t.fg, a),
                        Ellipsize(labels[i], valueX - 24 - textX, fs).c_str());

            if (i < (int)values.size() && !values[i].empty()) {
                const int vh = m_gfx->LineHeight(FontSize::Small);
                m_gfx->Text(FontSize::Small, valueX, cy - vh / 2,
                            WithAlpha(t.accent, a),
                            Ellipsize(values[i], W - 60 - valueX, FontSize::Small).c_str());
            }

            if (sel) {
                const int ax = kXmbMarginLeft + kXmbIcon / 3;
                const int s  = kXmbIcon / 6;
                m_gfx->FillTriangle(ax, cy - s, ax + s, cy, ax, cy + s,
                                    WithAlpha(t.accent, a));
            }
        }
    }
    void Menu::DrawCarousel(const std::vector<std::string> &labels,
                            const std::vector<std::string> &values,
                            int cursor, float &scroll_pos) {
        const Theme &t = m_theme.Current();
        if (labels.empty()) return;

        // Every sub-screen routes through here, so this one branch is what
        // carries the XMB look across the whole menu.
        if (m_ui_mode == UiMode::XMB) {
            DrawCarouselXmb(labels, values, cursor, scroll_pos);
            return;
        }

        scroll_pos += (cursor - scroll_pos) * 0.30f;
        if (std::abs(cursor - scroll_pos) < 0.01f) scroll_pos = (float)cursor;

        const int margin = kListX, center_y = 360, spacing = 48, span = 7;
        for (int off = -span; off <= span; off++) {
            const int idx = (int)lroundf(scroll_pos) + off;
            if (idx < 0 || idx >= (int)labels.size()) continue;

            const float vdist = std::abs((float)idx - scroll_pos);
            const bool  big   = vdist < 0.5f;
            const FontSize fs = big ? FontSize::Large : FontSize::Normal;
            const Uint8 alpha = (Uint8)std::max(24.0f, 255.0f - vdist * 52.0f);
            const int   lh    = m_gfx->LineHeight(fs);
            const int   y     = center_y + (int)((idx - scroll_pos) * spacing) - lh / 2;
            if (y < 90 || y > kHintY - 30) continue;

            const bool sel = (idx == cursor);
            const std::string &label = labels[idx];
            const int lw = m_gfx->TextWidth(fs, label.c_str());
            int tx;
            switch (m_align) {
                case TextAlign::Center: tx = (gfx::Gfx::Width - lw) / 2; break;
                case TextAlign::Right:  tx = gfx::Gfx::Width - margin - lw; break;
                default:                tx = margin; break;
            }

            if (sel)
                m_gfx->Text(FontSize::Large, tx - 34, y, WithAlpha(t.accent, alpha), ">");
            m_gfx->Text(fs, tx, y, WithAlpha(big ? t.accent : t.fg, alpha), label.c_str());

            if (idx < (int)values.size() && !values[idx].empty()) {
                const std::string &v = values[idx];
                const int vy = y + lh - m_gfx->LineHeight(FontSize::Small) - 2;
                const int vw = m_gfx->TextWidth(FontSize::Small, v.c_str());
                const int vx = (m_align == TextAlign::Right) ? (tx - vw - 18)
                                                             : (tx + lw + 18);
                m_gfx->Text(FontSize::Small, vx, vy, WithAlpha(t.accent, alpha), v.c_str());
            }
        }
    }

    // ---- Momentum, shared by every layout -----------------------------------
    // Whichever scroll value the current layout actually moves, and how far it
    // may travel. Each layout keeps its own, which is why this exists: without
    // it the fling would have to be written out once per mode.
    Menu::ScrollAxis Menu::ActiveAxis() {
        ScrollAxis a;
        const int last = (int)m_items.size() - 1;
        switch (m_ui_mode) {
            case UiMode::Flow:
                a.pos  = &m_flow_scroll;
                a.max  = (int)m_flow_items.size() - 1;
                a.wrap = m_wrap_nav;
                break;
            case UiMode::Grid:
                a.pos = &m_grid_scroll;
                a.max = TileMaxScroll();
                break;
            case UiMode::XMB:
                if (m_xmb_col >= 0 && m_xmb_col < (int)m_xmb_cols.size()) {
                    a.pos = &m_xmb_item_scroll;
                    a.max = (int)m_xmb_cols[m_xmb_col].items.size() - 1;
                }
                break;
            default:
                a.pos = &m_scroll_pos;
                a.max = last;
                break;
        }
        if (a.max < 0) a.max = 0;
        return a;
    }
    // Put the cursor where the scroll now points. The layouts differ in what
    // "where" means - a row for the grid, an entry in the open column for XMB,
    // an index into the filtered row for Flow - so it cannot be shared.
    void Menu::SyncCursorFromScroll() {
        switch (m_ui_mode) {
            case UiMode::Flow: {
                const int fn = (int)m_flow_items.size();
                if (fn <= 0) break;
                int at = (int)lroundf(m_flow_scroll);
                if (m_wrap_nav) {
                    at %= fn;
                    if (at < 0) at += fn;
                } else {
                    if (at < 0) at = 0;
                    if (at > fn - 1) at = fn - 1;
                }
                m_cursor = m_flow_items[at];
                break;
            }
            case UiMode::Grid: {
                // The grid scrolls by rows and its cursor follows the row, so
                // that letting go does not spring the view back to wherever the
                // selection happened to be.
                const int row = std::min(std::max(0, (int)lroundf(m_grid_scroll)),
                                         std::max(0, TileRowCount() - 1));
                m_cursor = TileFirstInRow(row);
                break;
            }
            case UiMode::XMB:
                m_xmb_item = (int)lroundf(m_xmb_item_scroll);
                XmbApplyCursor();
                break;
            default: {
                int idx = (int)lroundf(m_scroll_pos);
                if (idx < 0) idx = 0;
                if (idx > (int)m_items.size() - 1) idx = (int)m_items.size() - 1;
                m_cursor = idx;
                break;
            }
        }
    }
    // True while something other than the settle animation owns the scroll: a
    // finger on the screen, or a throw still coasting. Every layout checks this
    // before easing toward its selection, because that ease is what would
    // otherwise drag the list back the moment you let go.
    bool Menu::ScrollBusy() const {
        return m_touch_scroll_active || std::abs(m_fling_vel) > kFlowFlingStop;
    }
    void Menu::StepFling() {
        const u64 now = armGetSystemTick();
        float dt = m_fling_tick ? (float)(now - m_fling_tick) / (float)armGetSystemTickFreq()
                                : 0.0f;
        m_fling_tick = now;
        // A frame that took a whole second - a load, a suspend - would otherwise
        // teleport the list across the library.
        if (dt > 0.10f) dt = 0.10f;

        if (m_touch_scroll_active) return;              // the finger owns it
        if (std::abs(m_fling_vel) <= kFlowFlingStop) { m_fling_vel = 0.0f; return; }

        ScrollAxis a = ActiveAxis();
        if (!a.pos) { m_fling_vel = 0.0f; return; }

        *a.pos += m_fling_vel * dt;
        // Exponential decay, so a hard throw travels far and a gentle one barely
        // coasts, and neither depends on the frame rate.
        m_fling_vel *= expf(-kFlowFlingDrag * dt);

        if (a.wrap) {
            // Endless: fold the position back into the list instead of stopping,
            // and keep it near zero so a long coast cannot drift into the range
            // where a float stops resolving single items.
            const float span = (float)(a.max + 1);
            if (span > 0.0f) {
                while (*a.pos >= span) *a.pos -= span;
                while (*a.pos <  0.0f) *a.pos += span;
            }
        } else {
            // Finite: running into either end stops it dead rather than
            // straining against the clamp.
            if (*a.pos < 0.0f)          { *a.pos = 0.0f;          m_fling_vel = 0.0f; }
            if (*a.pos > (float)a.max)  { *a.pos = (float)a.max;  m_fling_vel = 0.0f; }
        }

        SyncCursorFromScroll();
    }
    void Menu::Render() {
        PollHbScan();       // swap in the homebrew browser list when its worker finishes
        PollResolvePins();  // fold in pinned-homebrew names/icons when its worker finishes
        PollUpdateCheck();  // join the update-check worker when it finishes
        PollPlayStats();    // start/collect the pdm play-time query

        // SD card pulled while powered on: nothing else matters, show the warning
        // (in the always-loaded system font) until the daemon reboots the console.
        if (m_sd_removed) {
            m_gfx->UseDefaultFont(true);
            DrawSdRemoved();
            m_gfx->Present();
            return;
        }

        // Cap how much image decoding a single frame may do. The menu appearing
        // promptly matters more than every icon being present on the very first
        // frame - they fill in over the next few, which reads as instant.
        StepFling();   // momentum, before any layout reads its scroll

        // Frame one gets no image budget at all. Decoding three covers - each a
        // 600x900 JPEG rescaled to 480x720 - before the first present is most of
        // the wait after a HOME press, and none of it is needed to put the menu
        // on screen. From frame two the normal budget applies and the art fills
        // in over the next handful of frames, exactly as it already does while
        // scrolling.
        const bool first_frame = !g_phase_done;
        m_icons.BeginFrame(first_frame ? 0 : 3);
        m_hb_icons.BeginFrame(first_frame ? 0 : 2);
        // Reading and decoding art happens on the art worker; this caps how
        // many finished pictures are uploaded per frame (PollArt takes half).
        m_cover_budget  = first_frame ? 0 : 6;

        // Fold in the deferred worker the moment it lands, before anything below
        // reads what it built.
        PollDeferred();
        // Cover statistics, once the shelf has had time to fill.
        if (g_phase_done && !g_cover_logged && g_frame1_tick != 0 &&
            (armGetSystemTick() - g_frame1_tick) > armGetSystemTickFreq() * 3)
            CoverStatsFlush();

        PollArt();          // covers, box scans, hero art decoded on the worker
        PollShotDecode();   // background hero panels, uploaded when they land
        PollCoverFetch();   // a fetched cover becomes visible on the next frame
        PollCoverPicker();  // ...and so does one chosen by hand

        // Bounce finished: hand the launch to the host, which dispatches it just
        // as it would an OnButton result.
        //
        // The animation state is NOT cleared here. Clearing it made LaunchAnimT
        // report "not running" for the very frame that was about to be drawn, so
        // the fade was skipped and the last thing presented was an unfaded menu
        // - which then sat on screen for as long as the daemon took to start the
        // game. Leaving it set holds the overlay at full black, which is what
        // stays up until the applet goes away.
        if (m_launch_tick != 0 && !m_launch_fired && LaunchAnimT() >= 1.0f) {
            m_launch_fired   = true;
            m_pending_action = m_launch_action;
            m_pending_id     = m_launch_id;
        }

        // Both of these are built by that worker, so they stay untouched until it
        // has been joined - reading a half-constructed mixer or widget list is
        // exactly the kind of race that only shows up on someone else's console.
        if (m_deferred_joined)
            m_music.Update();   // advance playback position / roll to the next track

        // Welcome screen bows out on its own once the jingle has played.
        if (m_screen == Screen::Welcome) {
            const u64 ms = (armGetSystemTick() - m_welcome_start) * 1000 / armGetSystemTickFreq();
            if (ms >= kWelcomeMs) m_screen = Screen::Main;
        }

        // Screen-transition fade: whatever changed m_screen since last frame -
        // there are around seventy call sites for that across every mode and
        // submenu, from a button press, a tap, the welcome screen's own
        // timeout above, anything - this is the one place that notices, so
        // it is the one place that has to.
        if (m_screen != m_screen_seen) {
            // DeckMenu gets its own slide (DrawDeckMenu / DrawDeckMenuClosing)
            // on both ends of the transition, not this generic darken too.
            m_suppress_screen_fade = (m_screen == Screen::DeckMenu ||
                                      m_screen_seen == Screen::DeckMenu);
            m_screen_seen = m_screen;
            m_screen_trans_tick = armGetSystemTick();
        }

        // The Fonts and Color-picker screens always render their chrome in the
        // default system font so they can never make themselves unreadable.
        m_gfx->UseDefaultFont(m_screen == Screen::Fonts || m_screen == Screen::ColorPicker ||
                              m_screen == Screen::Keyboard);

        Phase("pre-frame");
        DrawBackground();
        Phase("background");
        switch (m_screen) {
            case Screen::Oobe:        DrawOobe();   break;
            case Screen::Main:        DrawMain();   break;
            case Screen::Theming:     DrawTheming(); break;
            case Screen::Themes:      DrawThemes(); break;
            case Screen::ThemeEditor: DrawEditor(); break;
            case Screen::ColorPicker: DrawColorPicker(); break;
            case Screen::Fonts:       DrawFonts();  break;
            case Screen::Widgets:       DrawWidgets(); break;
            case Screen::WidgetOptions: DrawWidgetOptions(); break;
            case Screen::Keyboard:    DrawKeyboard(); break;
            case Screen::Music:       DrawMusic(); break;
            case Screen::Homebrew:    DrawHomebrew(); break;
            case Screen::Album:       DrawAlbum();    break;
            case Screen::Files:       DrawFiles();    break;
            case Screen::FlowMenu:    DrawFlowMenu(); break;
            case Screen::DeckMenu:    DrawDeckMenu(); break;
            case Screen::DeckLibrary: DrawDeckLibrary(); break;
            case Screen::DeckNews:    DrawDeckNews(); break;
            case Screen::FlowSettings: DrawFlowSettings(); break;
            case Screen::About:       DrawAbout(); break;
            case Screen::Welcome:     DrawWelcome(); break;
            case Screen::SysEntries:  DrawSysEntries(); break;
            case Screen::Network:     DrawNetwork();    break;
            case Screen::CoverPicker: DrawCoverPicker(); break;
            case Screen::Power:       DrawPower(); break;
            case Screen::Payloads:    DrawPayloads(); break;
        }
        // Touch-only "go back", on every screen a button-B would leave from.
        // Under the options overlay and any dialog on purpose - both already
        // have their own way out, and neither is what B does while they're up.
        if (m_screen != Screen::Main && m_screen != Screen::Oobe &&
            !m_options_open && m_dialog == Dialog::None)
            DrawBackTap();
        DrawUsbStatus();   // over the screen, under the options overlay and dialogs
        if (m_options_open) DrawOptions();
        Phase("draw screen");
        if (m_dialog != Dialog::None) DrawDialog();

        // Last, so it sits over every screen including dialogs, and always in
        // the system font: a user-selected font may have no digits worth
        // reading, and the whole point of this panel is the numbers.
        // Memory trace. Enabled only when sdmc:/slaunch/config/memtrace.txt
        // exists, so it costs nothing normally. The crash we are chasing kills
        // the process mid-frame, which means the only evidence that survives is
        // what has already been written to the card - hence a periodic snapshot
        // rather than a dump on exit.
        if (m_memtrace_on) {
            const u64 now = armGetSystemTick(), freq = armGetSystemTickFreq();
            if (m_memtrace_tick == 0 || (now - m_memtrace_tick) > 2 * freq) {
                m_memtrace_tick = now;
                m_debug.Frame();                 // refresh the kernel counters
                m_debug.Dump(DebugCounters());
            }
        }
        if (m_debug.Visible()) {
            m_debug.Frame();
            m_gfx->UseDefaultFont(true);
            m_debug.Draw(m_gfx, DebugCounters());
        }

        // Building the cover cache is the one wait worth explaining. It happens
        // only the first time each cover is seen, but the frames carrying those
        // decodes are slow, and an unexplained pause reads as a lock-up. The
        // notice clears itself once no decode has happened for a moment.
        if (m_cache_msg_tick != 0) {
            const u64 since = (armGetSystemTick() - m_cache_msg_tick) * 1000
                            / armGetSystemTickFreq();
            if (since < 700) {
                const Theme &tt = m_theme.Current();
                const int W  = gfx::Gfx::Width;
                // Up top, clear of both the centred title at the bottom and
                // the tops of the cases, which start around y=120.
                const int pw = 360, ph = 54;
                const int px = (W - pw) / 2;
                const int py = 56;

                m_gfx->FillRect(px, py, pw, ph, WithAlpha(tt.bg_bottom, 235));
                m_gfx->FillRect(px, py, pw, 3, tt.accent);

                char line[80];
                snprintf(line, sizeof(line), "%s  %d",
                         T("Preparing box art"), m_cache_built);
                m_gfx->TextCentered(FontSize::Small, W / 2, py + 8,  tt.fg, line);
                m_gfx->TextCentered(FontSize::Small, W / 2, py + 29, tt.dim,
                                    T("First time only"));
            } else {
                m_cache_msg_tick = 0;
            }
        }

        // Appear fade: black lifting off, the reverse of the launch fade below.
        // Started on the first frame rather than at Init, so the timing is not
        // skewed by however long start-up took.
        {
            constexpr u64 kAppearMs = 420;
            const u64 now_t = armGetSystemTick();
            if (m_appear_prev == 0) m_appear_prev = now_t;
            const u64 dms = (now_t - m_appear_prev) * 1000 / armGetSystemTickFreq();
            m_appear_prev = now_t;

            if (m_appear_p < 1.0f) {
                // Follows wall time normally, but never advances more than an
                // eighth in one frame - so however slow the early frames are,
                // the fade is always at least eight steps rather than a jump.
                float step = (float)dms / (float)kAppearMs;
                if (step > 0.125f) step = 0.125f;
                m_appear_p += step;
                if (m_appear_p > 1.0f) m_appear_p = 1.0f;

                const float t = m_appear_p;
                // Ease out, so it clears quickly and lingers least where the
                // picture is already complete.
                const float a = (1.0f - t) * (1.0f - t);
                m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height,
                                SDL_Color{0, 0, 0, (Uint8)(255.0f * a)});
            }
        }

        // Screen-transition fade: the new screen above is already the right
        // one - Draw* never needed to change for this - so all that is left
        // is to cover the cut with the same lifting-black overlay the appear
        // fade above uses. Every mode and every submenu gets this from one
        // mechanism, because every one of them already funnels through this
        // Render() to get drawn at all.
        if (m_screen_trans_tick != 0) {
            constexpr u64 kScreenTransMs = 160;
            const u64 ms = (armGetSystemTick() - m_screen_trans_tick) * 1000
                         / armGetSystemTickFreq();
            if (ms < kScreenTransMs) {
                if (!m_suppress_screen_fade) {
                    const float t = (float)ms / (float)kScreenTransMs;
                    const float a = (1.0f - t) * (1.0f - t);   // same ease-out as the appear fade
                    m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height,
                                    SDL_Color{0, 0, 0, (Uint8)(255.0f * a)});
                }
            } else {
                m_screen_trans_tick = 0;   // done; skip the check from here on
            }
        }

        // DeckMenu closing: it isn't a screen change away from Deck's home
        // screen so much as the same screen with the panel pulled back off
        // it, so this runs after that screen has already drawn itself above,
        // laying the panel back over it sliding out instead of just cutting.
        if (m_deck_menu_close_tick != 0) {
            constexpr u64 kDeckMenuSlideMs = 150;
            const u64 ms = (armGetSystemTick() - m_deck_menu_close_tick) * 1000
                         / armGetSystemTickFreq();
            if (ms < kDeckMenuSlideMs && m_ui_mode == UiMode::Deck &&
                m_screen == Screen::Main) {
                const float t = (float)ms / (float)kDeckMenuSlideMs;
                DrawDeckMenuClosing(1.0f - (1.0f - t) * (1.0f - t));   // ease-out
            } else {
                m_deck_menu_close_tick = 0;
            }
        }

        // The back half of the launch bounce fades the whole screen down, so the
        // menu goes out under the growing case instead of being cut off by the
        // game appearing. Drawn last, over everything including the widgets.
        {
            const float lt = LaunchAnimT();
            if (lt > kLaunchFadeAt) {
                const float u = (lt - kLaunchFadeAt) / (1.0f - kLaunchFadeAt);
                m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height,
                                SDL_Color{0, 0, 0, (Uint8)(255.0f * u)});
            }
        }

        m_gfx->Present();
        // First frame is up: write the phase breakdown out once. Everything
        // after this is a no-op.
        Phase("present");
        PhaseFlush();
    }
    void Menu::DrawSdRemoved() {
        const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
        const int cx = W / 2;

        // Classic full-screen blue warning.
        const SDL_Color blue  = {  15,  70, 180, 255 };
        const SDL_Color white = { 255, 255, 255, 255 };
        const SDL_Color soft  = { 205, 222, 255, 255 };
        m_gfx->FillRect(0, 0, W, H, blue);

        // Localised like everything else: the strings are already in memory, so
        // losing the card does not cost us the translation.
        m_gfx->TextCentered(FontSize::Title,  cx, 210, white, T("SD card removed"));
        m_gfx->FillRect(cx - 150, 292, 300, 3, white);
        m_gfx->TextCentered(FontSize::Normal, cx, 336, white,
                            T("Please only remove the SD card while the console is off."));
        m_gfx->TextCentered(FontSize::Small,  cx, 388, soft,
                            T("Taking it out while powered on can corrupt your data."));
        m_gfx->TextCentered(FontSize::Normal, cx, 470, soft, T("Restarting..."));
    }
    void Menu::DrawMainEmpty() {
        const Theme &t = m_theme.Current();
        m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, 340, t.dim,
                            m_loading   ? T("Loading games...")
                          : !m_search.empty() ? T("No matches")
                                          : T("No apps found"));
        // Without this an empty result looks like a menu that lost everything.
        if (!m_loading && !m_search.empty()) {
            m_gfx->TextCentered(FontSize::Small, gfx::Gfx::Width / 2, 380, t.dim,
                                ("\"" + m_search + "\"").c_str());
            m_gfx->TextCentered(FontSize::Small, gfx::Gfx::Width / 2, 420, t.accent,
                                T("B: Clear search    Y: Search again"));
        }

        // Three dots, pulsing out of phase with each other - shared by every
        // UI mode, since they all fall back to this one placeholder while the
        // library is still loading. Doesn't touch how long that actually
        // takes (see Phase 3's note in the plan for why not); just says the
        // menu is working rather than stuck, which the static text above
        // never could on its own.
        if (m_loading) {
            const float sec = (float)armGetSystemTick() / (float)armGetSystemTickFreq();
            const int cx = gfx::Gfx::Width / 2, cy = 386, gap = 22, dot = 8;
            for (int i = 0; i < 3; i++) {
                const float phase = sec * 3.4f - (float)i * 0.75f;
                const float p = 0.5f + 0.5f * sinf(phase);      // 0..1 breathing
                const int   s = dot - 2 + (int)(4.0f * p);       // 6..10px
                const Uint8 a = (Uint8)(90.0f + 130.0f * p);     // 90..220
                const int   x = cx + (i - 1) * gap - s / 2;
                m_gfx->FillRect(x, cy - s / 2, s, s, WithAlpha(t.accent, a));
            }
        }

        DrawHint({ {{"plus"}, "Power"} });
    }
    void Menu::DrawMain() {
        switch (m_ui_mode) {
            case UiMode::Line:    DrawMainLine();    break;
            case UiMode::Grid:    DrawMainGrid();    break;
            case UiMode::Cover:   DrawMainCover();   break;
            case UiMode::Shelf:   DrawMainShelf();   break;
            case UiMode::XMB:     DrawMainXmb();     break;
            case UiMode::Flow:    DrawMainFlow();    break;
            case UiMode::Deck:    DrawMainDeck();    break;
            default:              DrawMainList();    break;
        }

        // Widgets overlay every layout, drawn at their own (draggable) positions.
        // Nothing to draw until the deferred worker has finished building them.
        // Deck is the exception: it gives the widgets a row of cards of their
        // own, and the floating copies would land on top of it.
        if (m_deferred_joined && m_widgets.AnyEnabled() && m_ui_mode != UiMode::Deck) {
            const Theme &t = m_theme.Current();
            m_widgets.Render(m_gfx, t, m_ui_mode == UiMode::Grid);

            // Accent outline around the widget being dragged, so it reads as "held".
            int bx, by, bw, bh;
            if (m_drag_active && m_widgets.GetBox(m_drag_widget, bx, by, bw, bh) && bh > 0) {
                const SDL_Color a = t.accent;
                m_gfx->FillRect(bx - 3, by - 3,  bw + 6, 3,      a);
                m_gfx->FillRect(bx - 3, by + bh, bw + 6, 3,      a);
                m_gfx->FillRect(bx - 3, by - 3,  3,      bh + 6, a);
                m_gfx->FillRect(bx + bw, by - 3, 3,      bh + 6, a);
            }
        }
    }
    std::string Menu::Ellipsize(const std::string &s, int maxw, gfx::FontSize fs) const {
        if (maxw <= 0) return std::string();
        if (m_gfx->TextWidth(fs, s.c_str()) <= maxw) return s;

        int lo = 0, hi = (int)s.size();
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            while (mid > lo && ((unsigned char)s[mid] & 0xC0) == 0x80) mid--;  // UTF-8 boundary
            if (mid == lo) break;
            if (m_gfx->TextWidth(fs, (s.substr(0, mid) + "...").c_str()) <= maxw) lo = mid;
            else                                                                  hi = mid - 1;
        }
        while (lo > 0 && ((unsigned char)s[lo] & 0xC0) == 0x80) lo--;
        return s.substr(0, lo) + "...";
    }
    void Menu::DrawStatusHint(std::initializer_list<HintSeg> segs) {
        const Theme &t = m_theme.Current();
        // Fresh status (< 3s) shows above the control hint, then fades out.
        if (m_status[0] != '\0') {
            u64 nowt = armGetSystemTick(), freq = armGetSystemTickFreq();
            if ((nowt - m_status_tick) < 3 * freq)
                m_gfx->TextCentered(FontSize::Normal, gfx::Gfx::Width / 2, kHintY - 44, t.accent, m_status);
            else
                m_status[0] = '\0';
        }
        DrawHint(segs);
    }
    // One square app/entry tile: the cached icon when present, otherwise a
    // themed placeholder card carrying the item's initial + name so system
    // entries (Theming, Album, ...) and icon-less titles still read clearly.
    // Clear all cached system icon textures so a pack switch takes effect.
    void Menu::InvalidateSysIcons() {
        for (auto &kv : m_sys_icons) { if (kv.second) m_gfx->FreeImage(kv.second); }
        m_sys_icons.clear();
    }
    // Load (and cache) the black/white icon drawn for a non-game menu entry.
    // When a custom icon pack is active (m_icon_pack_idx > 0), the icon is loaded
    // from sdmc:/slaunch/icon_packs/<pack_name>/<name>.png and rescaled to 64x64
    // so it matches the built-in size. If the custom icon is missing the lookup
    // falls back to the built-in at sdmc:/slaunch/icons/<name>.png.
    // Missing files -> nullptr and the tile falls back to its lettered placeholder.
    SDL_Texture *Menu::SystemIcon(ItemKind kind) {
        auto it = m_sys_icons.find((int)kind);
        if (it != m_sys_icons.end()) return it->second;

        const u64 t_icon0 = armGetSystemTick();   // cold load; see PhaseFlush
        const char *file = nullptr;
        switch (kind) {
            case ItemKind::Theming:      file = "theming";      break;
            case ItemKind::RandomGame:   file = "random";       break;
            // Games themselves always draw their own title icon, so this is only
            // ever reached for the XMB Game category header. It used to borrow
            // "random", which is a shuffle glyph and reads as "random game", not
            // as "games".
            case ItemKind::Game:         file = "games";        break;
            case ItemKind::Controllers:  file = "controllers";  break;
            case ItemKind::Album:        file = "album";        break;
            case ItemKind::MusicPlayer:  file = "music";        break;
            case ItemKind::MediaCat:     file = "media";        break;
            case ItemKind::UserPage:     file = "user";         break;
            case ItemKind::WebBrowser:   file = "browser";      break;
            case ItemKind::MiiEdit:      file = "mii";          break;
            case ItemKind::Settings:     file = "settings";     break;
            case ItemKind::Wifi:         file = "wifi";         break;
            case ItemKind::Power:        file = "power";        break;
            case ItemKind::HomebrewMenu: file = "homebrewmenu"; break;
            case ItemKind::FileManager:  file = "filemanager";  break;
            default: break;
        }
        SDL_Texture *tex = nullptr;
        if (file) {
            // Try custom icon pack first (if selected). LoadImageScaled handles
            // both existence check and rescale in one pass, returning nullptr on miss.
            // LoadGlyph separates the artwork from its background field so the
            // theme can colour the field (see Theme::icon_bg); the shipped PNGs
            // are white-on-black with no alpha channel.
            //
            // Loaded at native resolution. Forcing 64x64 here threw away most of
            // a 256x256 pack and left the XMB bar upscaling a 64px texture to
            // 88px, which is exactly as soft as it sounds. There are only ten of
            // these and they are cached, so full size costs little.
            if (m_icon_pack_idx > 0 && m_icon_pack_idx <= (int)m_icon_packs.size()) {
                const std::string &pack = m_icon_packs[m_icon_pack_idx - 1];
                char path[256];
                snprintf(path, sizeof(path), "sdmc:/slaunch/icon_packs/%s/%s.png", pack.c_str(), file);
                tex = m_gfx->LoadGlyph(path, 0, 0);
            }
            // Fall back to built-in icons if custom pack missing or icon not found
            if (!tex) {
                char path[64];
                snprintf(path, sizeof(path), "sdmc:/slaunch/icons/%s.png", file);
                tex = m_gfx->LoadGlyph(path, 0, 0);
            }
        }
        m_sys_icons[(int)kind] = tex; // cache even nullptr so we don't re-stat
        if (!g_phase_done) {
            g_sysicon_n++;
            g_sysicon_ms += (unsigned)((armGetSystemTick() - t_icon0) * 1000
                                       / armGetSystemTickFreq());
        }
        return tex;
    }

    // Not themed by an icon pack - these aren't menu entries, they're button
    // glyphs, so there is exactly one of each rather than one per pack. Same
    // cache-nullptr-on-miss shape as SystemIcon: a hint bar missing one icon
    // still lays out, just without that glyph.
    SDL_Texture *Menu::HintIcon(const char *name) {
        auto it = m_hint_icons.find(name);
        if (it != m_hint_icons.end()) return it->second;
        char path[64];
        snprintf(path, sizeof(path), "sdmc:/slaunch/icons/buttons/%s.png", name);
        SDL_Texture *tex = m_gfx->LoadGlyph(path, 0, 0);
        m_hint_icons[name] = tex;
        return tex;
    }

    void Menu::DrawOptions() {
        const Theme &t = m_theme.Current();
        const int n = (int)m_options.size();
        if (n == 0) return;

        // Dim the menu behind the panel.
        m_gfx->FillRect(0, 0, gfx::Gfx::Width, gfx::Gfx::Height, SDL_Color{0, 0, 0, 150});

        const int rowH   = 62;
        const int panelW = 560;
        const int panelH = rowH * n + 40;
        const int px = (gfx::Gfx::Width  - panelW) / 2;
        const int py = (gfx::Gfx::Height - panelH) / 2;

        m_gfx->FillRect(px, py, panelW, panelH, WithAlpha(t.bg_bottom, 245));
        m_gfx->FillRect(px, py, panelW, 4, t.accent);

        for (int i = 0; i < n; i++) {
            const bool sel = (i == m_options_cursor);
            const int  ry  = py + 26 + i * rowH;
            if (sel) m_gfx->FillRect(px + 14, ry - 8, panelW - 28, rowH - 10, WithAlpha(t.accent, 60));
            m_gfx->Text(FontSize::Normal, px + 36, ry, sel ? t.accent : t.fg,
                        m_options[i].label.c_str());
        }
    }
} // namespace sl::menu::ui
