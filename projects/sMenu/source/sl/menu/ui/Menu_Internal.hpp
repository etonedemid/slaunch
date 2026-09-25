#pragma once
// Shared implementation detail for the Menu.cpp / Menu_*.cpp split: row-id
// enums, per-layout constants and small helpers that used to live in
// anonymous namespaces inside one Menu.cpp. OnTouch's generic submenu
// dispatch needs the *_Count values, and several of the draw/layout
// constants and helpers below are read from more than one of the split
// files, so none of this can stay file-local anymore - everything here is
// `inline` (functions and, per C++17, variables) rather than `static`, so
// each split .cpp gets the same definitions instead of its own private copy
// that would violate ODR or silently disagree with another file's copy.
//
// Not part of the public sl::menu::ui API: included only by Menu.cpp and its
// siblings via a quoted "Menu_Internal.hpp", not from include/.

#include <sl/menu/ui/Menu.hpp>
#include <sys/stat.h>
#include <cstdio>

namespace sl::menu::ui {

    using gfx::FontSize;

    // ---- submenu row ids ---------------------------------------------------
    // Declared up here rather than beside each screen's handler because the
    // touch code near the top of the file has to know how many rows a screen
    // has; a stale hand-counted number there silently makes the last row
    // untappable.
    enum { TH_Themes = 0, TH_UiMode, TH_TextPos, TH_ListIcons,
           TH_IconPack, TH_Antialias, TH_ShelfVert, TH_TileCols, TH_TileRows,
           TH_TdbRegion, TH_Sgdb, TH_SgdbKey, TH_FlowSet, TH_Wrap,
           TH_Hints, TH_Counter, TH_RetroArch, TH_Shortcuts, TH_Fonts,
           TH_Language, TH_Music,
           TH_Widgets, TH_Entries,
           TH_Welcome, TH_Updates,
           TH_About, TH_Back, TH_Count };

    enum { EF_Background = 0, EF_Wallpaper, EF_WallpaperDim, EF_WallpaperBlur,
           EF_WallpaperBlurRadius, EF_WallpaperSnow,
           EF_Top, EF_Bottom, EF_Text,
           EF_Accent, EF_Secondary, EF_Title, EF_IconFg, EF_IconBg, EF_IconBgAlpha,
           EF_FxA, EF_FxB,
           EF_RibbonLines, EF_RibbonThickness, EF_RibbonAmplitude,
           EF_RibbonSeed, EF_RibbonLayers, EF_RibbonYCenter, EF_FxX, EF_FxCam,
           EF_Rename, EF_Save, EF_Delete, EF_Count };

    // GameTDB art regions, offered in Theming; the first is the default.
    inline constexpr const char *kTdbRegions[] = { "US", "EN", "JA", "FR", "DE",
                                                   "ES", "IT", "AU", "KO", "ZH", "RU" };
    inline constexpr int kTdbRegionCount = (int)(sizeof(kTdbRegions) / sizeof(kTdbRegions[0]));

    // Music player layout, shared by DrawMusic and its touch handler.
    inline constexpr int kMuArtX = 80, kMuArtY = 110, kMuArt = 300;
    inline constexpr int kMuListX = 460, kMuListY = 100, kMuRowH = 44, kMuRows = 10;
    inline constexpr int kMuBarY = 612;

    // Return true when row *r* belongs to the wallpaper-effects block.
    inline bool IsEffectRow(int r) {
        return r >= EF_WallpaperDim && r <= EF_WallpaperSnow;
    }
    // Return true when row *r* is the blur-radius setting (hidden when blur off).
    inline bool IsBlurRadiusRow(int r) { return r == EF_WallpaperBlurRadius; }
    // Return true when row *r* belongs to the ribbon block.
    // Ribbon, Ribbon glow and Grid all steer off the same six numbers - the
    // editor just calls them different things (see the label override in
    // DrawThemeEditor). Gradient, Stars and Aurora have nothing to tune.
    inline bool StyleHasParams(int style) {
        return style != BackgroundStyle_Gradient;
    }

    // Only the grid (its sun) and the ocean (its reflection) have anything to
    // place horizontally; showing the row for the others would be a knob
    // that does nothing.
    inline bool StyleHasFxX(int style) {
        return style == BackgroundStyle_Grid || style == BackgroundStyle_Ocean;
    }

    // Same story for the camera: only a style with a projection has one.
    inline bool StyleHasFxCam(int style) { return style == BackgroundStyle_Grid; }

    // The two effect colours only mean something to a style that draws with
    // them, which is the same set.
    inline bool IsFxColourRow(int r) { return r == EF_FxA || r == EF_FxB; }

    inline bool IsRibbonRow(int r) {
        return r >= EF_RibbonLines && r <= EF_FxCam;
    }
    // Return true when the wallpaper path names a video file (played back by
    // gfx::VideoPlayer) rather than a still image. v1 scope is H.264 in an
    // .mp4 container - see scripts/build-ffmpeg.sh.
    inline bool IsVideoPath(const char *path) {
        if (!path || !path[0]) return false;
        size_t len = strlen(path);
        return len >= 4 && strcasecmp(path + len - 4, ".mp4") == 0;
    }

    // The entries the user can hide from the main menu. Theming is
    // deliberately absent: it has to stay or these toggles become
    // unreachable.
    struct SysEntry { ItemKind kind; const char *name; };
    inline const SysEntry kSysEntries[] = {
        { ItemKind::RandomGame,   "Random game"   },
        { ItemKind::Controllers,  "Controllers"   },
        { ItemKind::Album,        "Album"         },
        { ItemKind::MusicPlayer,  "Music"         },
        { ItemKind::UserPage,     "User Page"     },
        { ItemKind::WebBrowser,   "Web Browser"   },
        { ItemKind::MiiEdit,      "Mii Edit"      },
        { ItemKind::Wifi,         "Network"       },
        { ItemKind::Power,        "Power"         },
        { ItemKind::HomebrewMenu, "Homebrew menu" },
        { ItemKind::FileManager,  "Files"         },
    };
    inline constexpr int kSysEntryN = (int)(sizeof(kSysEntries) / sizeof(kSysEntries[0]));

    // Rows of the Network screen. Declared here rather than beside the screen
    // itself because the touch router needs the row count, and that runs
    // above where the screen is implemented.
    enum { NET_Status = 0, NET_Name, NET_Signal, NET_Ip, NET_Wifi, NET_Open,
           NET_Count };

    // Languages offered under Theming. "auto" follows the console's own
    // setting, which is what the menu did before this was selectable; the
    // rest are the translations shipped in assets/lang.
    //
    // The names are deliberately plain ASCII English rather than endonyms:
    // the project is ASCII-only outside the locale files themselves, and a
    // Latin-script language name stays recognisable whatever the menu is
    // currently rendering in. They are not run through T() for the same
    // reason - "Russian" should read the same however you got here.
    struct LangOption { const char *code; const char *name; };
    inline const LangOption kLangs[] = {
        { "auto", nullptr     },   // name filled in from T("Automatic")
        { "en",   "English"   },
        { "de",   "German"    },
        { "es",   "Spanish"   },
        { "fr",   "French"    },
        { "ru",   "Russian"   },
        { "ja",   "Japanese"  },
        { "zh",   "Chinese"   },
    };
    inline constexpr int kLangN = (int)(sizeof(kLangs) / sizeof(kLangs[0]));

    // Home screen. The row is one wide tile - the game you last played -
    // followed by the rest of the library as upright tiles of exactly the
    // same height, so the row reads as one band rather than a picture with
    // a strip of small covers parked next to it.
    inline constexpr int kDeckRowX  = 28,  kDeckRowY   = 60;
    // The games row is the marquee of the home screen - the game you're on,
    // plus your library - so it reads bigger than the news strip below it.
    // Grown by 60px total from the original 230 (see kDeckCardArtH's
    // comment - the news cards gave up the same 60px): every gap around
    // both blocks - subtitle-to-tabs, tabs-to-cards, cards-to-hint - stays
    // exactly what it was. DrawMainDeck clips the row to kDeckRowRight's
    // safe width now, so growing this further no longer risks an unselected
    // trailing tile bleeding past the true screen edge the way it did
    // before that clip existed - the limit here is card legibility, not
    // the margin.
    inline constexpr int kDeckRowH  = 290;                 // hero and tiles alike
    // True 16:9 at the row height, kept in step with kDeckRowH so the hero
    // never drifts off ratio - the row's height is shared with the upright
    // cover tiles beside it (see above), and growing only the hero would
    // break the "one band" read.
    inline constexpr int kDeckHeroW = 516;   // 516x290 ~= 16:9
    // Narrowed with the row, in the same proportion, so a cover tile stays
    // the ~2:3 a box front actually is instead of going squarer as the row
    // got shorter under it.
    inline constexpr int kDeckTileW = 190;
    inline constexpr int kDeckRowGap = 16;
    inline constexpr int kDeckRowRight = 24;               // margin the row scrolls to
    // The game title (row bottom + 10) and the played-time line (row bottom +
    // 50) both sit above this - see kDeckRowH's own comment for where the
    // room to move this up again came from.
    inline constexpr int kDeckTabY  = 434, kDeckTabH   = 34;
    // 4 cards; the art gave up the 60px the row above grew by (kicker/date +
    // 2-line-title block underneath is unchanged), so it's a wider crop than
    // a true 16:9 now rather than the marquee tile above it - that's the
    // point, the games row is meant to read as the bigger of the two.
    inline constexpr int kDeckCardX = 30,  kDeckCardY  = 484;
    inline constexpr int kDeckCardW = 274, kDeckCardH  = 172;
    inline constexpr int kDeckCardGap = 22;
    inline constexpr int kDeckCardsVisible = 4;
    inline constexpr int kDeckCardArtH = 94;   // was 154; 60px went to the row above

    // Library grid.
    inline constexpr int kDeckLibX = 138, kDeckLibTop = 146;
    inline constexpr int kDeckLibW = 180, kDeckLibH = 270;
    inline constexpr int kDeckLibGapX = 26, kDeckLibGapY = 20;
    inline constexpr int kDeckLibCols = 5;
    inline constexpr int kDeckLibBot = 668;          // grid is clipped to here

    // Side menu.
    inline constexpr int kDeckMenuW = 372;

    enum { DeckTab_Game = 0, DeckTab_Nintendo, DeckTab_Widgets, DeckTab_Count };
    enum { DeckLib_All = 0, DeckLib_Favourites, DeckLib_Recent,
           DeckLib_Gamecard, DeckLib_Homebrew, DeckLib_Count };

    // Which entries the Flow and Deck side menus list: everything that is
    // not already on the shelf.
    inline bool FlowMenuItem(const MenuItem &it) {
        return it.kind != ItemKind::Game && it.kind != ItemKind::Homebrew;
    }

    // Layout (1280x720)
    inline constexpr int kTopBarH   = 56;
    inline constexpr int kListX      = 120;
    inline constexpr int kListTop    = 150;
    inline constexpr int kRowH       = 54;
    inline constexpr int kListW       = 1040;
    inline constexpr int kHintY       = 682;

    // Universal touch "Back" corner: the bottom-left, on every non-Main
    // screen, tap-equivalent to B. DrawBackTap and OnTouch's hit-test both
    // read these, which is what keeps the drawn label and the tappable area
    // the same rectangle. Deliberately not tied to m_show_hints - it is the
    // only way back on a screen a touch-only session can otherwise get stuck
    // on (the cover picker, Network - neither has a tappable row of its own
    // that means "back"), so turning hints off must not remove it.
    inline constexpr int kBackTapX = 16, kBackTapY = kHintY - 28;
    inline constexpr int kBackTapW = 150, kBackTapH = 56;

    // On-screen distance between neighbouring entries, per layout. Touch drag
    // divides by these to move the content at the same rate as the finger, and
    // the hit-tests invert them, so they have to be the numbers the renderers
    // actually use.
    inline constexpr int kListSpacing = 48;    // List/Cover: row to row
    inline constexpr int kListCenterY = 360;   // vertical centre of the carousel
    inline constexpr int kLinePitch   = 210;   // Line: cover centre to cover centre
    // Tiles, on the unit grid a Windows 8 / Windows Phone start screen uses: one
    // square unit, a fixed gap, and wider tiles built from whole units so every
    // edge lines up however they are mixed.
    inline constexpr int kGridGap     = 5;     // Windows 10 Mobile packs tiles tight
    // The band the wall is laid out inside. The side margin matches the one the
    // top bar clock already uses, and the bottom stops clear of the hint line;
    // nine columns at a fixed 130px unit would have reached within 23px of the
    // screen edge, which is inside where a TV can overscan.
    inline constexpr int kWallTop     = 104;
    inline constexpr int kWallBot     = 652;
    inline constexpr int kWallMargin  = 40;
    // The unit is derived from the counts rather than fixed, so the wall always
    // fills the band whatever shape the user asks for. The limits are where the
    // unit stops being usable: past 12 across a tile is under 90px, and past 6
    // down it is under 85.
    inline constexpr int kTileColsMin = 4,  kTileColsMax = 12;
    inline constexpr int kTileRowsMin = 2,  kTileRowsMax = 6;
    // The width home widgets are authored against (Widgets.cpp uses the same
    // number for the floating layout). Keeping the two equal also means a
    // widget that is on the wall and on another layout's home screen renders at
    // one width, so its own cached texture is never reallocated between them.
    inline constexpr int kTileWidgetW = 340;
    inline constexpr int kTilePicMs   = 4000;  // picture tile: hold per image
    inline constexpr int kTileFadeMs  = 600;   // ...and cross-fade over this

    // Shelf mode geometry (Xbox-360 "My Games" style): uniform covers in a row,
    // the selected one anchored near the left. Shared by draw, hit-test and
    // touch scrolling (OnTouch).
    // Tile size and pitch now depend on whether the shelf is drawing square or
    // portrait tiles, so they live in Menu::ShelfTileW/H/Pitch rather than here -
    // the renderer, the drag handler and the touch hit-test all read them from
    // there, which is what keeps the three agreeing about where a tile is.
    inline constexpr int kShelfGap     = 20;
    inline constexpr int kShelfAnchorX = 88;    // left edge of the selected cover
    inline constexpr int kShelfTop     = 150;   // top edge of the cover row

    // XMB geometry, matching RetroArch's XMB "PS3" layout.
    //
    // RetroArch derives every value from a scale factor of
    // (menu_scale_factor * surface_width) / 1920, so its constants are written
    // against a 1920-wide reference. Our surface is a fixed 1280x720, giving a
    // factor of exactly 2/3; each value below is the RetroArch figure times 2/3,
    // with the original in the comment so the two can be diffed by eye.
    //
    // The layout is left-anchored, not centred: the category row and the entry
    // column share one x anchor, so the selected category sits directly above
    // the column it opened. That single alignment is what makes XMB read as a
    // cross rather than as two stacked lists.
    inline constexpr int   kXmbIcon       = 85;   // icon_size,               128
    inline constexpr int   kXmbSpacingH   = 128;  // icon_spacing_horizontal, 192
    inline constexpr float kXmbSpacingV   = 42.67f; // icon_spacing_vertical,  64
    inline constexpr int   kXmbMarginTop  = 181;  // margins_screen_top,      272
    inline constexpr int   kXmbMarginLeft = 224;  // margins_screen_left,     336
    inline constexpr int   kXmbLabelLeft  = 57;   // margins_label_left,       85
    inline constexpr int   kXmbSettingLeft= 440;  // margins_setting_left,    660
    inline constexpr int   kXmbTitleLeft  = 43;   // margins_title_left
    inline constexpr int   kXmbTitleTop   = 34;   // margins_title_top

    // Centre X shared by the category row and the entry column, and centre Y of
    // the category row. RetroArch parks the active category at
    // margins_screen_left + icon_spacing_horizontal by animating categories_x_pos
    // to -icon_spacing_horizontal * selected; the result is this fixed anchor.
    inline constexpr int kXmbAnchorX = kXmbMarginLeft + kXmbSpacingH;  // 352
    inline constexpr int kXmbTabY    = kXmbMarginTop + kXmbIcon / 2;   // 223

    // The vertical placement curve, verbatim from RetroArch's xmb_item_y(): rows
    // above the cursor, the cursor itself, and rows below it each get their own
    // offset in units of icon_spacing_vertical. The asymmetry is deliberate and
    // is the single most recognisable thing about XMB - the active row is pushed
    // three spacings down to clear the category bar, and the first row under it
    // sits five spacings further on to leave a band for the sublabel.
    inline constexpr float kXmbAboveItem  = -1.0f;  // above_item_offset
    inline constexpr float kXmbActiveItem =  3.0f;  // active_item_factor
    inline constexpr float kXmbUnderItem  =  5.0f;  // under_item_offset

    // Selection is carried by size and brightness alone - XMB never boxes or
    // outlines the current row.
    inline constexpr float kXmbZoomActive   = 1.0f;   // items_active_zoom
    inline constexpr float kXmbZoomPassive  = 0.5f;   // items_passive_zoom
    inline constexpr float kXmbAlphaActive  = 1.0f;   // items_active_alpha
    inline constexpr float kXmbAlphaPassive = 0.75f;  // items_passive_alpha

    inline constexpr int kXmbItemPitch  = (int)kXmbSpacingV;  // passive row spacing
    inline constexpr int kXmbAbove      = 4;    // rows kept above the selection
    inline constexpr int kXmbBelow      = 8;    // ...and below it

    // Rows above the cursor do not stop at the category row - the above_item
    // offset puts the first of them at y=138, clear of the category icons and
    // level with the title. They keep climbing and fade out as they go, which is
    // RetroArch's menu_xmb_vertical_fade_factor and is what tells you at a glance
    // which way you have scrolled. Fading them out at the category row instead
    // would hide every one of them.
    inline constexpr int kXmbFadeEnd   = 60;             // fully gone at/above
    inline constexpr int kXmbFadeStart = kXmbMarginTop;  // fully lit at/below

    // The bottom of the column does the same thing in reverse. It used to stop
    // at a fixed line a little short of the screen edge, so the last row did not
    // leave - it was there on one frame and gone on the next, in clear space
    // well inside the panel, which is far more noticeable than a row sliding off
    // an edge. The band is the same depth as the one at the top and is anchored
    // to the screen edge, so a row reaches zero exactly as it runs out of room.
    inline constexpr int kXmbFadeBotEnd   = gfx::Gfx::Height;   // fully gone at/below
    inline constexpr int kXmbFadeBotStart =                     // fully lit at/above
        kXmbFadeBotEnd - (kXmbFadeStart - kXmbFadeEnd);

    // Fractional row offset for a cursor sitting between two entries. The three
    // branches of xmb_item_y() are exact only at whole positions, so the gaps are
    // bridged linearly; at every integer cursor this reproduces RetroArch's
    // layout unchanged, and between them the column slides in one smooth move.
    inline float XmbRowOffset(float d) {
        const float active = kXmbSpacingV * kXmbActiveItem;
        if (d <= -1.0f) return kXmbSpacingV * (d + kXmbAboveItem);
        if (d >=  1.0f) return kXmbSpacingV * (d + kXmbUnderItem);
        if (d < 0.0f) {
            const float edge = kXmbSpacingV * (-1.0f + kXmbAboveItem);
            return active + (edge - active) * (-d);
        }
        const float edge = kXmbSpacingV * (1.0f + kXmbUnderItem);
        return active + (edge - active) * d;
    }

    // On-screen distance between the centre cover and its neighbour in Flow, in
    // pixels. Shares the fate of the other layout pitches: the drag handler and
    // the renderer must agree on it, so it lives up here with them rather than
    // being re-derived in either.
    inline constexpr int kFlowPitchPx = 228;

    // Momentum, in items per second.
    //
    // Drag is how quickly a thrown row loses speed: it keeps exp(-drag * t) of
    // its velocity, so 1.1 leaves about a third of it after a second - a long,
    // slow glide rather than a short skid. Min is the speed below which a
    // release counts as placing the row rather than throwing it, and Stop is
    // where a glide is slow enough to hand over to the settle.
    inline constexpr float kFlowFlingDrag = 1.1f;
    inline constexpr float kFlowFlingMin  = 0.8f;
    inline constexpr float kFlowFlingStop = 0.15f;

    inline SDL_Color WithAlpha(SDL_Color c, Uint8 a) { return SDL_Color{ c.r, c.g, c.b, a }; }

    // The plate behind a system icon. Its opacity is the theme's setting scaled
    // by whatever alpha the row is currently animating at, so a fading or
    // zoomed-out row fades its plate along with its artwork instead of leaving a
    // solid square floating behind a ghost icon.
    inline SDL_Color IconPlate(const Theme &t, Uint8 item_alpha) {
        return WithAlpha(t.icon_bg, (Uint8)((int)item_alpha * t.icon_bg_alpha / 255));
    }

    // The artwork on that plate. Same idea, one step simpler: the icon's own
    // alpha carries the shape, so the row's fade is the only thing scaling it.
    // Pass it to DrawImageTinted, which multiplies this alpha by its own.
    inline SDL_Color IconTint(const Theme &t, Uint8 item_alpha) {
        return WithAlpha(t.icon_fg, item_alpha);
    }

    // Editor palette.
    inline const SDL_Color kPalette[] = {
        {255,255,255,255},{200,200,200,255},{120,120,120,255},{  0,  0,  0,255},
        { 90,170,255,255},{ 60,120,220,255},{200,120,255,255},{160, 90,220,255},
        {255,190, 90,255},{255,140, 60,255},{255, 90, 90,255},{180,240,120,255},
        { 90,220,140,255},{ 90,210,210,255},{240,220,120,255},{ 30, 40, 70,255},
    };
    inline constexpr int kPaletteCount = (int)(sizeof(kPalette) / sizeof(kPalette[0]));

    // =========================================================================
    // ---- start-up profiling -------------------------------------------------
    //
    // main.cpp already stamps the coarse milestones into boot.log; these are the
    // phases inside them, which is where the remaining time to first frame
    // actually goes. Marks are buffered and written once, after the first frame
    // is on screen: an fopen per mark costs several milliseconds on the SD card
    // and would have measured itself as much as the work.
    struct PhaseMark { const char *what; unsigned ms; };
    inline PhaseMark g_phases[24];
    inline int       g_phase_n    = 0;
    inline u64       g_phase_tick = 0;
    inline bool      g_phase_done = false;

    // Cold loads during start-up, counted separately: these are PNG decodes
    // plus a per-pixel alpha pass, and unlike the entry icons they are not
    // budgeted, so a whole XMB category row can land on frame one.
    inline int g_sysicon_n  = 0;
    inline unsigned g_sysicon_ms = 0;

    // Cover loads, split by whether the decoded-pixel cache had them. This
    // is the number that says how long the shelf takes to fill, and it is
    // written a few seconds in rather than with the phases, because covers
    // load from frame two onward.
    inline int      g_cover_hits = 0, g_cover_miss = 0;
    inline unsigned g_cover_ms   = 0;
    inline u64      g_frame1_tick = 0;
    inline bool     g_cover_logged = false;

    inline void CoverStatsFlush() {
        if (g_cover_logged) return;
        g_cover_logged = true;
        FILE *fp = fopen("sdmc:/slaunch/boot.log", "a");
        if (!fp) return;
        fprintf(fp, "    covers: %d from cache, %d decoded, %ums total\n",
                g_cover_hits, g_cover_miss, g_cover_ms);
        fclose(fp);
    }

    inline void PhaseReset() { g_phase_tick = armGetSystemTick(); g_phase_n = 0; }

    inline void Phase(const char *what) {
        if (g_phase_done || g_phase_n >= (int)(sizeof(g_phases) / sizeof(g_phases[0])))
            return;
        const u64 now = armGetSystemTick();
        const u64 ms  = g_phase_tick
                      ? (now - g_phase_tick) * 1000 / armGetSystemTickFreq() : 0;
        g_phase_tick = now;
        g_phases[g_phase_n++] = { what, (unsigned)ms };
    }

    inline void PhaseFlush() {
        if (g_phase_done) return;
        g_phase_done = true;
        FILE *fp = fopen("sdmc:/slaunch/boot.log", "a");
        if (!fp) return;
        unsigned total = 0;
        for (int i = 0; i < g_phase_n; i++) {
            fprintf(fp, "    phase %-20s %4ums\n", g_phases[i].what, g_phases[i].ms);
            total += g_phases[i].ms;
        }
        fprintf(fp, "    phase %-20s %4ums\n", "TOTAL", total);
        fprintf(fp, "    sys icons cold-loaded %3d  (%ums)\n",
                g_sysicon_n, g_sysicon_ms);
        fclose(fp);
        // Covers load from frame two onward, so their tally is written a few
        // seconds after this point rather than alongside the phases.
        g_frame1_tick = armGetSystemTick();
    }

    // =========================================================================
    // ---- everything below was scattered through the middle of the old
    // Menu.cpp, one small anonymous namespace per screen/feature, sitting next
    // to whichever function used it. Same reason as everything above: split
    // across Menu_*.cpp files, a screen's helpers are no longer necessarily in
    // the same translation unit as the code that calls them.
    // =========================================================================

    // ---- once-per-boot welcome bookkeeping ---------------------------------
        inline constexpr const char *kWelcomedPath = "sdmc:/slaunch/cache/welcomed.txt";
        inline long long BootId() {   // approximate console boot time, in epoch seconds
            const u64 up = armGetSystemTick() / armGetSystemTickFreq();
            return (long long)time(nullptr) - (long long)up;
        }

    // ---- On-screen keyboard (rename) ---------------------------------------
        // 4 character rows + a special bottom row (Shift/Space/Back/Clear/Done).
        inline const char *kKbRows[4] = {
            "1234567890",
            "qwertyuiop",
            "asdfghjkl",
            "zxcvbnm",
        };
        inline const char *kKbSpecial[5] = { "Shift", "Space", "Back", "Clear", "Done" };
        inline constexpr int kKbSpecialRow  = 4;
        inline constexpr int kKbSpecialCols = 5;

        inline int KbRowLen(int row) {
            if (row < 4) return (int)strlen(kKbRows[row]);
            return kKbSpecialCols; // special row
        }

    // ---- X "Options" overlay ------------------------------------------------
    enum { OptFav = 0, OptRename, OptMove, OptUnpinHb, OptSetDonor,
                       OptSort, OptCloseGame, OptPickCover, OptDismiss,
                       OptTileSize, OptTileColor, OptTileReset,
                       OptAddWidget, OptAddWidgetMenu, OptSubBack,
                       OptRemoveWidget }; 

    // ---- online update check (opt-out) -------------------------------------
        // Pluck a "key":"value" string field out of the GitHub release JSON.
        // Pull a string value out of a JSON body, honouring backslash escapes.
        //
        // The naive version stopped at the first quote and returned the raw
        // bytes, which is fine for something like a GitHub tag but wrong for a
        // URL: PHP encoders escape forward slashes by default, so SteamGridDB
        // hands back "https:\/\/cdn2.steamgriddb.com\/grid\/x.png" and passing
        // that to curl verbatim fails every time. It also truncated on any value
        // containing an escaped quote.
        // Every string value for `key`, in document order, up to `max`.
        //
        // The grids endpoint returns an array and the cover picker wants all of
        // it, so this is the general form and JsonStr below is the first-match
        // case of it. One decoder rather than two: the escape handling here is
        // what a plain find() got wrong before (SteamGridDB escapes its forward
        // slashes, so an undecoded url was a 404).
        inline std::vector<std::string> JsonStrAll(const std::string &j, const char *key,
                                            size_t max) {
            std::vector<std::string> out;
            const std::string pat = std::string("\"") + key + "\"";
            size_t p = 0;
            while (out.size() < max) {
                p = j.find(pat, p);
                if (p == std::string::npos) break;
                p += pat.size();
                const size_t colon = j.find(':', p);
                if (colon == std::string::npos) break;
                const size_t s = j.find('"', colon);
                if (s == std::string::npos) break;

                std::string v;
                size_t i = s + 1;
                for (; i < j.size(); i++) {
                    const char c = j[i];
                    if (c == '"') break;              // unescaped quote ends it
                    if (c != '\\') { v += c; continue; }
                    if (++i >= j.size()) break;
                    switch (j[i]) {
                        case 'n': v += '\n'; break;
                        case 't': v += '\t'; break;
                        case 'r': v += '\r'; break;
                        case 'b': v += '\b'; break;
                        case 'f': v += '\f'; break;
                        case 'u': i += 4; break;      // \uXXXX: not needed here
                        default:  v += j[i]; break;   // \/ \\ \" and anything else
                    }
                }
                out.push_back(std::move(v));
                p = i;
            }
            return out;
        }

        inline std::string JsonStr(const std::string &j, const char *key) {
            const std::vector<std::string> v = JsonStrAll(j, key, 1);
            return v.empty() ? std::string() : v[0];
        }
        inline int CmpVer(const char *a, const char *b) {   // >0 if a newer than b; skips a 'v'
            auto parse = [](const char *s, int v[3]) {
                v[0] = v[1] = v[2] = 0;
                if (s && (*s == 'v' || *s == 'V')) s++;
                if (s) sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
            };
            int va[3], vb[3]; parse(a, va); parse(b, vb);
            for (int i = 0; i < 3; i++) if (va[i] != vb[i]) return va[i] - vb[i];
            return 0;
        }

    // ---- Welcome (after setup / once per boot) -----------------------------
        inline constexpr u64 kWelcomeMs   = 4600;   // ~the opening jingle's length
        inline constexpr u64 kWelcomeFade = 520;    // in at the start, out at the end

        // The greeting itself is the only varying text, so the screen stays a
        // greeting and a name rather than a greeting, a name and a slogan.
        inline const char *kWelcomeMsgs[] = {
            "Welcome home",
            "Good to see you",
            "Ready when you are",
            "Let's play",
        };
        inline constexpr int kWelcomeMsgN = (int)(sizeof(kWelcomeMsgs) / sizeof(kWelcomeMsgs[0]));

    // ---- launch animation ---------------------------------------------------
    //
    // Dispatching a launch exits the applet, so an animation cannot simply be
    // played "on the way out" - there are no frames after the action is
    // returned. The action is therefore held here, the menu keeps rendering,
    // and the host collects it through TakePendingAction once the bounce is
    // done. Flow only: it is the one layout with a single object big enough on
    // screen for the movement to read.
        inline constexpr u64 kLaunchMs = 380;      // whole bounce
        inline constexpr float kLaunchDip  = 0.88f;  // anticipation, before the spring
        inline constexpr float kLaunchPeak = 1.34f;  // how far it comes toward you
        inline constexpr float kLaunchDipEnd = 0.28f; // fraction spent dipping
        inline constexpr float kLaunchFadeAt = 0.45f; // fade to black starts here

        // Dip, then spring. The dip is a quarter sine so it eases into the low
        // point; the spring is an ease-out cubic so it leaves fast and settles.
        inline float LaunchScale(float t) {
            if (t < kLaunchDipEnd) {
                const float u = t / kLaunchDipEnd;
                return 1.0f - (1.0f - kLaunchDip) * sinf(u * 1.5707963f);
            }
            const float u = (t - kLaunchDipEnd) / (1.0f - kLaunchDipEnd);
            const float e = 1.0f - powf(1.0f - u, 3.0f);
            return kLaunchDip + (kLaunchPeak - kLaunchDip) * e;
        }

    // ---- cover picker -------------------------------------------------------
    //
    // The automatic fetch takes the top-ranked grid for whatever the name search
    // matched, which is right often enough to be worth doing and wrong often
    // enough to be worth overriding. This lists everything SteamGridDB has at
    // case-front proportions and lets you take the one you want.
    //
    // Thumbnails go to a scratch directory rather than into covers/, so nothing
    // here can disturb the art already on the card until a choice is confirmed.
        inline constexpr const char *kPickDir = "sdmc:/slaunch/cache/covertmp";
        inline constexpr int kPickMax  = 24;   // grids listed; two full screens
        inline constexpr int kPickCols = 6;
        inline constexpr int kPickRows = 2;    // visible at once
        inline constexpr int kPickCellW = 170;
        inline constexpr int kPickCellH = 255;
        inline constexpr int kPickGapX  = 20;
        inline constexpr int kPickGapY  = 22;
        inline constexpr int kPickTop   = 138;

        inline int PickCellX(int col) {
            const int total = kPickCols * kPickCellW + (kPickCols - 1) * kPickGapX;
            return (gfx::Gfx::Width - total) / 2 + col * (kPickCellW + kPickGapX);
        }

    // ---- Power screen -------------------------------------------------------
    // The daemon is the one that can actually sleep/reboot/shut the console down,
    // so every row here ends up as an SMI command; the menu only picks which one
    // and asks for confirmation first.
        enum { PW_Sleep = 0, PW_Restart, PW_Shutdown, PW_Payload, PW_Back };

        // Where payloads live on a normal Atmosphere/hekate card.
        inline const char *kPayloadDirs[] = { "sdmc:/payloads", "sdmc:/bootloader/payloads" };
        inline constexpr const char *kDefaultPayload = "sdmc:/atmosphere/reboot_payload.bin";

    // ---- Play statistics ----------------------------------------------------
    // Last run's numbers, kept next to the app-list cache. Without them a
    // play-ordered menu would come up title-ordered and visibly reshuffle a few
    // frames later, when the pdm worker lands.
    inline constexpr const char *kPlayCachePath = "sdmc:/slaunch/cache/playstats.txt";


    // ---- About + changelog -------------------------------------------------
        struct LogLine { bool head; const char *text; };
        // Newest first. Headers are version tags; the rest are one-line summaries.
        inline const LogLine kChangelog[] = {
            { true,  "v1.4.1" },
            { false, "Much better loading speed for Flow and the other modes" },
            { false, "Fixed homebrew launching and logging (SD card file access)" },
            { true,  "v1.4.0" },
            { false, "USB file transfer - plug into a computer on the menu to copy files, with progress shown" },
            { false, "File manager: browse the SD card, copy, move, rename and delete" },
            { false, "Album plays video clips, with a full photo viewer and slideshow" },
            { false, "Music has a full player: album art, track list, seeking, repeat, shuffle, volume" },
            { false, "Flow is real 3D - solid boxes with lighting, reflections and printed box scans" },
            { false, "Box art from GameTDB, full box scans included, with a region choice" },
            { false, "SteamGridDB is optional now; box art downloads show their progress" },
            { false, "Box art loads in the background - no more stutter landing on a game" },
            { false, "Grid looks like Windows 10 Mobile tiles; Shelf like the Xbox 360" },
            { false, "New Ocean background, Retrowave hills, and every background runs on the GPU" },
            { false, "Simpler first-run setup, with a gallery to pick your layout" },
            { false, "The theme editor saves when you close it" },
            { false, "Fixed: red and blue swapped, crashes on bad images and big ROM libraries, choppy clip audio" },
            { false, "Fixed: some games showing another game's Steam news" },
            { true,  "v1.3.0" },
            { false, "Settings are per user now - theme, layout, favourites and the rest follow the account" },
            { false, "Your existing settings carry over to the first account that opens the menu" },
            { false, "Homebrew can hand over to other homebrew, and arguments now survive the launch" },
            { false, "Homebrew chainloads can run with full RAM (see the README)" },
            { false, "Optional content filter for covers and news, off unless you turn it on" },
            { false, "NetSurf is filed under Network in XMB, next to the browser" },
            { true,  "v1.2.0" },
            { false, "Deck mode - a SteamOS-style layout: your library, news cards, a library grid and a side menu" },
            { false, "Every button hint is now a real button icon, not text" },
            { false, "HOME button opens the Deck side menu" },
            { false, "Smoother fade between screens" },
            { false, "Fixed touches that got silently swallowed or left a screen stranded" },
            { true,  "v1.1.0" },
            { false, "Anti-aliasing option (Theming) - smoother edges everywhere" },
            { false, "Flow layout presets: Coverflow, Flat, Arc, Wall, Showcase, Spiral" },
            { false, "Flow boxes are rounder - faces are drawn in finer strips" },
            { false, "French translation" },
            { false, "Works on older firmware - the menu finds its applet slot" },
            { true,  "v1.0.0" },
            { false, "Grid is now a wall of tiles, in three sizes" },
            { false, "Any tile can be given a colour of its own" },
            { false, "Home widgets can be placed on the wall as live tiles" },
            { false, "Wall columns and rows are adjustable (Theming)" },
            { false, "Every screen is now fully translated" },
            { true,  "v0.9.1" },
            { false, "Menu opens much faster - art loads after it appears" },
            { false, "Flow scrolls smoothly; covers load ahead of the row" },
            { false, "Shelf can show vertical box art (Theming > Vertical covers)" },
            { true,  "v0.9.0" },
            { false, "Flow mode - a 3D shelf of game boxes you can turn around" },
            { false, "Box art fetched automatically (Theming > SteamGridDB key)" },
            { false, "XMB rebuilt to match the real thing, and is now the default" },
            { false, "Every submenu now wears the XMB look too" },
            { false, "Homebrew category lists everything on your card, not just pins" },
            { false, "Media category: screenshot viewer + menu music in one place" },
            { false, "Language can be set by hand (Theming > Language)" },
            { false, "Icon background opacity slider; Minimal is the default pack" },
            { false, "New icons: gear, music, games, homebrew, media" },
            { false, "Ribbon reworked - real depth, up to 12 layers, free placement" },
            { false, "Confirm / click / back sounds (drop them in slaunch/sounds)" },
            { false, "Menu is usable straight away - no more freeze after it appears" },
            { false, "Fixed: fully transparent colours rendered fully opaque" },
            { true,  "v0.8.0" },
            { false, "XMB mode - the PSP cross-media bar, with touch" },
            { false, "Closing a homebrew returns to the menu instead of relaunching it" },
            { false, "Reboot and shutdown fixed (no more half-asleep console)" },
            { false, "Icon packs (Theming > Icon pack) + bundled Minimal pack" },
            { false, "Translations: Russian, Japanese, German, Spanish, Chinese" },
            { false, "Korean/Chinese consoles now use their own system font" },
            { false, "Theme colour for the icon background; drag scrolls the content" },
            { true,  "v0.7.0" },
            { false, "Welcome screen that greets you by name on boot and after setup" },
            { false, "Show or hide any system entry (Theming > Menu entries)" },
            { false, "Random game entry - rolls through your library and picks one" },
            { false, "Setup restyled to match the rest of the menu" },
            { true,  "v0.6.0" },
            { false, "About screen with this changelog" },
            { false, "Optional update check on startup (opt-out in setup / Theming)" },
            { false, "Installer: check + install updates online, reboot button" },
            { false, "Installer: full uninstall, keeps your settings on reinstall" },
            { true,  "v0.5.0" },
            { false, "Homebrew runs as a full application via a donor game (crash fixed)" },
            { false, "Favourite homebrew - it sits up top with your favourite games" },
            { false, "Homebrew icons cached + loaded instantly, no more menu hang" },
            { false, "Fixed the ~2s freeze on every menu appearance" },
            { false, "Fixed widget flickering; Web Browser opens Google" },
            { true,  "v0.4.0" },
            { false, "Background music from the SD, resumes where it left off" },
            { false, "UI sounds; homebrew browser (pin, launch as applet or app)" },
            { false, "Reorder any entry, more sorting options, reworked setup" },
            { true,  "v0.3.1" },
            { false, "Localization, faster boot/suspend, Shelf UI mode" },
            { true,  "v0.2.0" },
            { false, "Lua widgets, app icons, extra UI modes" },
        };
        inline constexpr int kChangelogN = (int)(sizeof(kChangelog) / sizeof(kChangelog[0]));
        inline constexpr int kAboutTop = 300, kAboutRowH = 34, kAboutVisible = 9;

    // Theme-editor rows (EF_*) and their visibility predicates live near the top
    // of the file; the touch code needs them. This is just the colour lookup.
    inline SDL_Color *EditorColor(Theme &c, int row) {
        switch (row) {
            case EF_Top:       return &c.bg_top;
            case EF_Bottom:    return &c.bg_bottom;
            case EF_Text:      return &c.fg;
            case EF_Accent:    return &c.accent;
            case EF_Secondary: return &c.dim;
            case EF_Title:     return &c.title;
            case EF_IconFg:    return &c.icon_fg;
            case EF_IconBg:    return &c.icon_bg;
            case EF_FxA:       return &c.fx_a;
            case EF_FxB:       return &c.fx_b;
            default:           return nullptr;
        }
    }

        // =========================================================================
        // PS3 XMB-style ribbon background: translucent ribbons flowing across the
        // screen.
        //
        // Each ribbon is a *surface*, not a line. Per column we evaluate the wave
        // at x and at x+step, and the slope between them gives the foreshortening
        // term 1/sqrt(1+slope^2) - the cosine of the angle the surface makes with
        // the screen. That single number drives both the band's thickness and its
        // brightness, so a ribbon turning edge-on narrows and dims exactly as a
        // real twisting sheet would, and flattens out wide and bright as it comes
        // back round. That is what reads as a ribbon rather than as a stripe.
        //
        // Two superimposed sine waves at different spatial and temporal rates keep
        // the flow from looking like a metronome.
        //
        // Drawn on the GPU as one triangle strip per ribbon (kRibbonVs). The
        // rect-per-column version below it is the fallback for when GL is not
        // available, and costs a filled rect per column per ribbon on the CPU.
        // =========================================================================

        // GPU ribbon: one triangle strip per ribbon, the wave evaluated per
        // vertex from the same per-ribbon numbers the rect version uses. The
        // strip joins column to column, so steep sections stay one sheet
        // without the rect version's gap-filling spans.
        inline constexpr const char *kRibbonVs = R"(
uniform vec4 uW1;    // spatial freq, temporal freq, phase, amplitude
uniform vec4 uW2;    // spatial freq 2, temporal freq 2, base y, flat height
uniform vec4 uCol;
uniform float uStep;
uniform float uGlow;
varying vec4 vCol;
varying float vAdd, vEdge, vHi;
float Wave(float nx) {
    float w1 = sin((nx * uW1.x + uTime * uW1.y + uW1.z) * TAU);
    float w2 = sin((nx * uW2.x - uTime * uW2.y + uW1.z * 1.7) * TAU);
    return uW2.z + (w1 + 0.45 * w2) * uW1.w;
}
void main() {
    float x  = aV.x * uStep;
    float y0 = Wave(x / 1280.0), y1 = Wave((x + uStep) / 1280.0);
    float slope = (y1 - y0) / uStep;
    float face  = inversesqrt(1.0 + slope * slope);   // 1 flat on, 0 edge on
    float h     = floor(uW2.w * (0.28 + 0.72 * face)) + 1.0;
    float half_ = h * 0.5 + abs(y1 - y0) * 0.5;
    vHi = 0.0;
    if (uGlow > 0.5) {                                 // additive bloom pass
        half_ += h;
        vCol = vec4(uCol.rgb, 0.0);
        vAdd = uCol.a * 0.35 * face;
    } else {
        vCol = vec4(uCol.rgb, uCol.a * (0.30 + 0.70 * face * face));
        vAdd = 0.0;
        float sheen = face * face * face;
        if (sheen > 0.35) vHi = min(1.0, uCol.a * sheen * 1.6);
    }
    vEdge = aV.y * 2.0 * half_;                        // px below the top edge
    gl_Position = Clip(vec2(x, y0 - half_ + vEdge));
}
)";
        inline constexpr const char *kRibbonFs = R"(
varying vec4 vCol;
varying float vAdd, vEdge, vHi;
void main() {
    float a = vCol.a;
    if (vEdge < 1.0) a = vHi + a * (1.0 - vHi);        // leading-edge sheen
    gl_FragColor = vec4(vCol.rgb * (a + vAdd), a);
}
)";

        inline void DrawRibbonBackground(gfx::Gfx *gfx, const Theme &t, bool glow = false) {
            const int W = gfx::Gfx::Width;
            const int H = gfx::Gfx::Height;
            const float elapsed = (float)armGetSystemTick() / (float)armGetSystemTickFreq();

            // Pre-compute the three palette colors.
            SDL_Color colors[3];
            colors[0] = t.fx_a;  // primary
            colors[1] = SDL_Color{255, 255, 255, 0}; // white wash
            colors[2] = t.fx_b;   // secondary

            // Clamp theme parameters to safe ranges.
            int numLines   = t.Fx().lines;
            int thickness  = t.Fx().thickness;
            int amplitude  = t.Fx().amplitude;
            int seed       = t.Fx().seed;
            int layers     = t.Fx().layers;
            int y_center    = t.Fx().y;
            if (numLines   <  1) numLines   =  1;
            if (numLines   > 40) numLines   = 40;
            if (thickness  <  1) thickness  =  1;
            if (thickness  >  20) thickness  =  20;
            if (amplitude  <  1) amplitude  =  1;
            if (amplitude  > 60) amplitude  = 60;
            if (seed       <  0) seed       =  0;
            if (seed      > 99) seed       = 99;
            if (layers     <  1) layers     =  1;
            if (layers     > 12) layers     = 12;
            // Y is deliberately allowed off-screen at both ends: the ribbons
            // spread a long way either side of this point, so parking it above
            // or below the panel is a legitimate way to show only the top or
            // bottom of the field. The bound is just far enough out to keep the
            // arithmetic sane.
            if (y_center  < -400) y_center  = -400;
            if (y_center  > 1120) y_center  = 1120;

            // The glow variant trades draw calls for detail: half the column
            // width, so the surface is sampled twice as finely and the curl
            // reads as a curve rather than as a staircase.
            const int step = glow ? 2 : 4;
            const float margin = 40.0f;
            const float usableH = (float)(H - 2 * margin);

            // Multi-layer rendering: each layer adds a full pass of ribbon lines
            // with a phase offset derived from the seed, creating XMB-style depth.
            const float layerAlphaDiv = 1.0f / (float)layers;

            // Deterministic per-layer jitter. Layers used to differ only by a
            // fixed phase offset, so they slid across in lockstep at one speed
            // and one amplitude and read as one thick ribbon rather than as
            // several at different distances. Each layer now gets its own speed,
            // amplitude and drift, hashed from its index and the theme seed, so
            // the field keeps rearranging itself without ever repeating.
            auto hashf = [](float v) {
                const float h = sinf(v) * 43758.5453f;
                return h - floorf(h);
            };

            const bool gpu = gfx->FxBegin(gfx->FxProgram(kRibbonVs, kRibbonFs));
            if (gpu) gfx->FxSet("uStep", (float)step);

            for (int L = 0; L < layers; L++) {
                const float lh1 = hashf((float)L * 12.9898f + (float)seed * 3.17f);
                const float lh2 = hashf((float)L * 78.2330f + (float)seed * 7.31f);
                const float lh3 = hashf((float)L * 45.1640f + (float)seed * 1.73f);

                const float layerTime  = 0.55f + 0.90f * lh1;   // speed
                const float layerAmp   = 0.70f + 0.60f * lh2;   // travel
                const float layerDrift = lh3 * 6.2831853f;      // starting phase

                // Draw order is back to front, so the last layer is the near one.
                const float depth = (layers > 1) ? (float)L / (float)(layers - 1)
                                                 : 1.0f;

                float layerPhase = (float)L * 1.618f + (float)seed * 0.37f + layerDrift;

                for (int li = 0; li < numLines; li++) {
                // Which color layer: 0=accent, 1=white-wash, 2=dim
                // Distribute: ~55% accent, ~30% white-wash, ~15% dim
                int colorIdx;
                if (li < (numLines * 55 + 49) / 100)       colorIdx = 0;
                else if (li < (numLines * 85 + 49) / 100)  colorIdx = 1;
                else                                       colorIdx = 2;

                SDL_Color c = colors[colorIdx];

                // Depth tint. Everything behind the front layer is pulled toward
                // the background colour, which is what aerial perspective does to
                // anything at distance and is the cheapest way to make the layers
                // separate instead of merging into one bright mass. The front
                // layer keeps the palette colour untouched.
                if (depth < 1.0f) {
                    const float k = (1.0f - depth) * 0.70f;
                    c.r = (Uint8)(c.r + ((int)t.bg_top.r - (int)c.r) * k);
                    c.g = (Uint8)(c.g + ((int)t.bg_top.g - (int)c.g) * k);
                    c.b = (Uint8)(c.b + ((int)t.bg_top.b - (int)c.b) * k);
                }

                // Base alpha divided by layer count to avoid washout when stacking.
                // These are the alpha a ribbon reaches when it is square on to
                // the screen; the facing term below scales down from here, so
                // they sit above the old flat-line values to keep the average
                // brightness of the field about where it was.
                float baseAlpha;
                switch (colorIdx) {
                    case 0: baseAlpha = 72.0f; break;
                    case 1: baseAlpha = 30.0f; break;
                    default: baseAlpha = 20.0f; break;
                }
                baseAlpha *= layerAlphaDiv;
                // ...and again by depth, so the far layers sit back behind the
                // near one rather than competing with it.
                baseAlpha *= 0.40f + 0.60f * depth;

                // Slow alpha breathing per line.
                float pulseFreq = 0.04f + (li % 5) * 0.015f;
                baseAlpha *= (0.7f + 0.3f * sinf((elapsed * pulseFreq + li * 0.7f) * 6.2831853f));
                c.a = (Uint8)baseAlpha;

                // Vertical position: pseudo-random spread around y_center.
                // Uses a deterministic hash so the layout is stable per frame
                // but lines don't cluster evenly by index.
                float hash = sinf(li * 127.1f + seed * 317.0f) * 43758.5453f;
                hash = hash - (int)hash; // fraction 0..1
                float spread = usableH * 0.45f; // half-spread around center
                float baseY = (float)y_center + (hash - 0.5f) * 2.0f * spread;

                // Seed perturbs spatial frequency so different seeds look distinct.
                float seedSpatial = (float)seed * 0.05f;
                float spatialFreq = 0.8f + 0.6f * sinf(li * 1.3f + 0.5f + seedSpatial);

                // Seed perturbs temporal frequency too.
                float seedTemporal = (float)seed * 0.002f;
                float temporalFreq;
                switch (colorIdx) {
                    case 0: temporalFreq = 0.06f + 0.03f * sinf(li * 0.9f) + seedTemporal; break;
                    case 1: temporalFreq = 0.08f + 0.04f * sinf(li * 1.1f) + seedTemporal; break;
                    default: temporalFreq = 0.04f + 0.02f * sinf(li * 0.7f) + seedTemporal; break;
                }
                if (temporalFreq < 0.03f) temporalFreq = 0.03f;
                temporalFreq *= layerTime;   // each layer travels at its own rate

                // Second wave, deliberately not a harmonic of the first and
                // travelling the other way, so the two never lock into a
                // repeating pattern the eye can latch onto.
                const float spatialFreq2  = spatialFreq * 1.87f + 0.31f;
                const float temporalFreq2 = temporalFreq * 0.63f;

                // Phase offset includes layer offset for depth.
                float phase = (float)li / (float)numLines + 0.1f + layerPhase;

                // Line-specific amplitude variation.
                float lineAmp = (float)amplitude * (0.7f + 0.3f * sinf(li * 1.7f + 2.0f))
                              * layerAmp;   // ...and travels its own distance

                // Height of the ribbon when it faces the screen square on. The
                // theme's thickness stays the knob that controls it.
                const float flatH = (float)thickness * 1.9f + 1.0f;

                // Wave height at a normalised x, as the sum of the two waves.
                auto waveAt = [&](float nx) {
                    const float w1 = sinf((nx * spatialFreq  + elapsed * temporalFreq  + phase) * 6.2831853f);
                    const float w2 = sinf((nx * spatialFreq2 - elapsed * temporalFreq2 + phase * 1.7f) * 6.2831853f);
                    return baseY + (w1 + 0.45f * w2) * lineAmp;
                };

                if (gpu) {
                    gfx->FxSet("uW1", spatialFreq, temporalFreq, phase, lineAmp);
                    gfx->FxSet("uW2", spatialFreq2, temporalFreq2, baseY, flatH);
                    gfx->FxSet("uCol", c);
                    if (glow) {
                        gfx->FxSet("uGlow", 1.0f);
                        gfx->FxStrip(W / step + 1);
                    }
                    gfx->FxSet("uGlow", 0.0f);
                    gfx->FxStrip(W / step + 1);
                    continue;
                }

                // One filled column per step, spanning the ribbon's thickness at
                // that point. Consecutive columns share edges, so the result is a
                // continuous surface rather than a dotted line.
                const float invStep = 1.0f / (float)step;
                float y0 = waveAt(0.0f);
                for (int x = 0; x < W; x += step) {
                    const float y1 = waveAt((float)(x + step) / (float)W);

                    // Foreshortening: 1 when the surface is flat to the screen,
                    // heading to 0 as it turns edge-on.
                    const float slope = (y1 - y0) * invStep;
                    const float face  = 1.0f / sqrtf(1.0f + slope * slope);

                    // Thin and dim it as it turns away. Squaring the brightness
                    // term tightens the highlight into something that reads as a
                    // sheen travelling along the ribbon.
                    const int h = (int)(flatH * (0.28f + 0.72f * face)) + 1;
                    SDL_Color cc = c;
                    cc.a = (Uint8)((float)c.a * (0.30f + 0.70f * face * face));

                    // Span the column from this sample to the next, not just the
                    // thickness at this one. Foreshortening thins the band
                    // exactly where the wave is steepest, which is where
                    // consecutive samples are furthest apart - so a fixed-height
                    // rect left the steep sections as a row of disconnected
                    // dashes. Covering the gap is what makes it read as one
                    // continuous sheet instead of a dotted line.
                    const float top = std::min(y0, y1) - h * 0.5f;
                    const float bot = std::max(y0, y1) + h * 0.5f;

                    // Bloom: the same column, spread wider and added rather than
                    // blended, under the ribbon itself. Additive is what makes
                    // it read as the ribbon emitting light instead of as a
                    // second, fatter ribbon behind it.
                    if (glow) {
                        const float bh = (bot - top) + h * 2.0f;
                        SDL_Color gl = c;
                        gl.a = (Uint8)((float)c.a * 0.35f * face);
                        gfx->FillRectAdd(x, (int)(top - h), step, (int)bh + 1, gl);
                    }

                    gfx->FillRect(x, (int)top, step, (int)(bot - top) + 1, cc);

                    // A brighter hairline along the leading edge, strongest where
                    // the ribbon faces us. This is the highlight that separates
                    // one ribbon from the one behind it.
                    const float sheen = face * face * face;
                    if (sheen > 0.35f) {
                        SDL_Color hi = cc;
                        hi.a = (Uint8)std::min(255.0f, (float)c.a * sheen * 1.6f);
                        gfx->FillRect(x, (int)top, step, 1, hi);
                    }

                    y0 = y1;
                }
            }
            }
            if (gpu) gfx->FxEnd();
        }

        inline const char *BackgroundName(int style) {
            switch (style) {
                case BackgroundStyle_Ribbon: return T("Ribbon");
                case BackgroundStyle_Stars:  return T("Stars");
                case BackgroundStyle_Aurora: return T("Aurora");
                case BackgroundStyle_Grid:   return T("Retrowave");
                case BackgroundStyle_RibbonHD: return T("Ribbon glow");
                case BackgroundStyle_Ocean:  return T("Ocean");
                default:                     return T("Gradient");
            }
        }

        // ---- other background styles ----------------------------------------
        // All three are the same deal as the ribbon: theme colours, no assets,
        // filled rects only, animated off the system tick. Anything needing a
        // per-pixel pass (plasma, real noise) is out - this runs every frame
        // under everything else the menu draws.

        inline float BgHash(float v) {           // 0..1, deterministic
            const float h = sinf(v) * 43758.5453f;
            return h - floorf(h);
        }

        // Starfield. Sizes spread wide enough to read as depth, and the
        // brightest few get a cross flare - one wide rect and one tall one
        // through the centre, which is what a bright point looks like through
        // any real lens and the cheapest way to say "this one is close".
        //   line_count -> how many       thickness -> largest size
        //   amplitude  -> twinkle speed   seed      -> which sky
        //   layers     -> flare size      y_center  -> horizon (none below it)
        // GPU stars: one quad per star, every property hashed from its index in
        // the vertex shader; the fragment shader draws core, cross flare and
        // halo inside the quad.
        inline constexpr const char *kStarsVs = R"(
uniform vec4 uA, uB;
uniform vec4 uP;        // max size, twinkle speed, seed, flare
uniform float uFloor;   // horizon: no stars below it
varying vec2 vL;        // px from the star's top-left corner
varying vec4 vC;
varying vec3 vS;        // size, flare length, halo spread
void main() {
    float fi  = aV.z + uP.z;
    vec2  pos = floor(vec2(Hash(fi * 1.37) * 1280.0, Hash(fi * 3.71) * uFloor));
    float mag = Hash(fi * 5.1);
    float tw  = 0.55 + 0.45 * sin(uTime * (0.2 + Hash(fi * 7.3) * uP.y) + fi);
    float sz  = 1.0 + floor(mag * uP.x);
    vec4  c   = mag > 0.82 ? uA : uB;
    vC = vec4(c.rgb, (0.18 + 0.82 * mag * mag) * tw);
    bool  flare = mag > 0.93 && uP.w > 0.5;
    float fl  = flare ? floor(sz * uP.w) : 0.0;
    float spr = flare ? sz * 3.0 : 0.0;
    float ext = max(fl, spr);
    vS = vec3(sz, fl, spr);
    vL = Corner() * (sz + 2.0 * ext) - ext;
    gl_Position = Clip(pos + vL);
}
)";
        inline constexpr const char *kStarsFs = R"(
varying vec2 vL;
varying vec4 vC;
varying vec3 vS;
void main() {
    float sz = vS.x, fl = vS.y;
    float a = 0.0;
    if (vL.x >= 0.0 && vL.y >= 0.0 && vL.x < sz && vL.y < sz) {
        a = vC.a;
    } else if (fl > 0.0) {
        float mid = floor(sz * 0.5);
        bool h = floor(vL.y) == mid && vL.x >= -fl && vL.x < sz + fl;
        bool v = floor(vL.x) == mid && vL.y >= -fl && vL.y < sz + fl;
        if (h || v) a = vC.a * 0.35;
    }
    vec3 add = vec3(0.0);
    if (vS.z > 0.0) {                          // halo rings, added
        vec2  d  = max(max(-vL, vL - sz), 0.0);
        float dd = ceil(max(d.x, d.y));
        if (dd > 0.0 && dd <= vS.z) {
            float f = 1.0 - dd / vS.z;
            add = vC.rgb * vC.a * f * f * 0.45;
        }
    }
    gl_FragColor = vec4(vC.rgb * a + add, a);
}
)";

        inline void DrawStarsBackground(gfx::Gfx *gfx, const Theme &t) {
            const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
            const float now = (float)armGetSystemTick() / (float)armGetSystemTickFreq();

            int   count = t.Fx().lines * 12;  if (count > 480) count = 480;
            if (count < 12) count = 12;
            const float maxSz  = 1.0f + (float)t.Fx().thickness * 0.25f;
            const float tspeed = (float)t.Fx().amplitude * 0.05f;
            const float seed   = (float)t.Fx().seed * 4.7f + 1.0f;
            const float flare  = (float)t.Fx().layers * 0.6f;
            float floorY = (float)t.Fx().y;
            if (floorY < 40.0f) floorY = 40.0f;
            if (floorY > (float)H) floorY = (float)H;

            if (gfx->FxBegin(gfx->FxProgram(kStarsVs, kStarsFs))) {
                gfx->FxSet("uA", t.fx_a);
                gfx->FxSet("uB", t.fx_b);
                gfx->FxSet("uP", maxSz, tspeed, seed, flare);
                gfx->FxSet("uFloor", floorY);
                gfx->FxQuads(count);
                gfx->FxEnd();
                return;
            }

            for (int i = 0; i < count; i++) {
                const float fi = (float)i + seed;
                const int x = (int)(BgHash(fi * 1.37f) * W);
                const int y = (int)(BgHash(fi * 3.71f) * floorY);
                const float mag = BgHash(fi * 5.1f);               // 0 faint .. 1 bright
                const float tw  = 0.55f + 0.45f * sinf(now * (0.2f + BgHash(fi * 7.3f) * tspeed) + fi);
                const int   sz  = 1 + (int)(mag * maxSz);
                SDL_Color c = (mag > 0.82f) ? t.fx_a : t.fx_b;
                c.a = (Uint8)(255.0f * (0.18f + 0.82f * mag * mag) * tw);

                if (mag > 0.93f && flare > 0.5f) {
                    // Close star: core, cross flare, and a soft halo.
                    const int fl = (int)((float)sz * flare);
                    SDL_Color f = c; f.a = (Uint8)(c.a * 0.35f);
                    gfx->FillRect(x - fl, y + sz / 2, fl * 2 + sz, 1, f);
                    gfx->FillRect(x + sz / 2, y - fl, 1, fl * 2 + sz, f);
                    gfx->GlowRect(x, y, sz, sz, c, sz * 3);
                }
                gfx->FillRect(x, y, sz, sz, c);
            }
        }

        // Aurora. Curtains, not columns: each band is a hanging sheet whose
        // bottom edge waves, lit from the bottom and fading out toward the top,
        // with vertical striations running through it - the rays are what make
        // it read as aurora rather than as a smear. Colour crossfades between
        // the accent and the title colour along its length.
        // GPU aurora: one strip per curtain, a column every 3px from the top of
        // the curtain (aV.y = 0) to its hem (1); the slice fade is quantised in
        // the fragment shader so "softness" keeps its meaning.
        inline constexpr const char *kAuroraVs = R"(
uniform vec4 uBand;   // index, drift, centre, reach
uniform vec4 uG;      // hem y, slices, brightness norm, seed
varying float vK, vI;
void main() {
    float x = aV.x * 3.0, nx = x / 1280.0, fb = uBand.x, drift = uBand.y;
    float edge = 1.0 - abs(x - uBand.z) / uBand.w;
    float env  = edge > 0.0 ? edge * (0.45 + 0.55 * edge) : 0.0;
    float hem  = uG.x * (0.55 + 0.30 * fb)
               + sin((nx * 2.3 + drift) * TAU) * 70.0
               + sin((nx * 5.1 - drift * 1.7) * TAU) * 26.0;
    float top  = hem - (720.0 * 0.42 + 60.0 * Hash(fb * 4.4 + nx + uG.w));
    float rs   = Hash(floor(nx * 90.0) + fb * 13.0);
    float ray  = 0.35 + 0.65 * (0.5 + 0.5 * sin(uTime * (0.35 + rs * 0.5) + rs * TAU));
    vI = 110.0 / 255.0 * ray * env * uG.z;
    vK = aV.y;
    gl_Position = Clip(vec2(x, mix(top, hem, aV.y)));
}
)";
        inline constexpr const char *kAuroraFs = R"(
uniform vec4 uA, uB, uG;
varying float vK, vI;
void main() {
    float k = min(floor(vK * uG.y), uG.y - 1.0) / (uG.y - 1.0);
    float a = k * k * (1.0 - 0.25 * k) * vI;
    if (a < 3.0 / 255.0) discard;
    gl_FragColor = vec4(mix(uB.rgb, uA.rgb, k) * a, 0.0);   // additive
}
)";

        inline void DrawAuroraBackground(gfx::Gfx *gfx, const Theme &t) {
            const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
            const float now = (float)armGetSystemTick() / (float)armGetSystemTickFreq();
            //   line_count -> curtains      thickness -> width
            //   amplitude  -> sway           seed      -> which sky
            //   layers     -> softness       y_center  -> where the hem hangs
            const int colStep = 3;
            int slices = 6 + t.Fx().layers;              // vertical steps in the fade
            if (slices < 4) slices = 4;
            // One curtain per unit, so the number in the editor is the number
            // on screen. Capped because past a handful they stop being separate
            // sheets and start being a fog.
            int bands = t.Fx().lines;
            if (bands < 1) bands = 1;
            if (bands > 8) bands = 8;
            const float sway   = (float)t.Fx().amplitude * 2.4f;
            const float wide   = 0.10f + (float)t.Fx().thickness * 0.035f;
            const float sseed  = (float)t.Fx().seed * 0.31f;
            // More curtains must not mean a brighter sky: without this, turning
            // the count up adds light everywhere they overlap and the screen
            // washes out instead of filling with separate sheets.
            const float bandNorm = 1.0f / sqrtf((float)bands);
            const float hemY   = (float)t.Fx().y;

            const bool gpu = gfx->FxBegin(gfx->FxProgram(kAuroraVs, kAuroraFs));
            if (gpu) {
                gfx->FxSet("uA", t.fx_a);
                gfx->FxSet("uB", t.fx_b);
                gfx->FxSet("uG", hemY, (float)slices, bandNorm, sseed);
            }

            for (int b = 0; b < bands; b++) {
                const float fb = (float)b;
                const float drift = now * (0.035f + 0.02f * BgHash(fb * 2.7f)) + fb * 2.1f;

                // Each curtain hangs over part of the sky and fades out at both
                // ends. Without this every band spans the full width, they all
                // add together everywhere, and the result is a wash over the
                // whole screen rather than lights in it.
                const float centre = W * (0.18f + 0.64f * (bands > 1 ? fb / (float)(bands - 1) : 0.5f))
                                   + sinf(drift * 2.0f + sseed) * sway;
                const float reach  = W * (wide + 0.10f * BgHash(fb * 6.1f + sseed));
                if (gpu) {
                    gfx->FxSet("uBand", fb, drift, centre, reach);
                    gfx->FxStrip(W / colStep + 1);
                    continue;
                }

                for (int x = 0; x < W; x += colStep) {
                    const float nx = (float)x / (float)W;
                    const float edge = 1.0f - fabsf((float)x - centre) / reach;
                    if (edge <= 0.0f) continue;
                    const float env = edge * (0.45f + 0.55f * edge);  // soft ends

                    // Where this column of the curtain hangs from and to. Two
                    // unrelated waves so the hem never repeats across the width.
                    const float hem = hemY * (0.55f + 0.30f * fb)
                                    + sinf((nx * 2.3f + drift) * 6.2831853f) * 70.0f
                                    + sinf((nx * 5.1f - drift * 1.7f) * 6.2831853f) * 26.0f;
                    const float top = hem - (H * 0.42f + 60.0f * BgHash(fb * 4.4f + nx + sseed));

                    // Rays: a fast, shallow noise across x that brightens and
                    // dims neighbouring columns independently.
                    // Rays brighten and dim on their own clock. This used to
                    // hash floorf(now * 0.6f), which is a step function: every
                    // column re-hashed on the same tick and the whole curtain
                    // changed at once, every second and a bit. Each column now
                    // has a fixed phase and rides a sine, so they drift past
                    // each other instead of switching together.
                    const float rseed = BgHash(floorf(nx * 90.0f) + fb * 13.0f);
                    const float ray = 0.35f + 0.65f *
                        (0.5f + 0.5f * sinf(now * (0.35f + rseed * 0.5f)
                                            + rseed * 6.2831853f));

                    const float sliceH = (hem - top) / (float)slices;
                    if (sliceH < 0.5f) continue;

                    for (int s2 = 0; s2 < slices; s2++) {
                        const float k = (float)s2 / (float)(slices - 1);   // 0 top .. 1 hem
                        // Brightest just above the hem, gone at the top.
                        const float lum = k * k * (1.0f - 0.25f * k);
                        SDL_Color c;
                        c.r = (Uint8)(t.fx_b.r + ((int)t.fx_a.r - (int)t.fx_b.r) * k);
                        c.g = (Uint8)(t.fx_b.g + ((int)t.fx_a.g - (int)t.fx_b.g) * k);
                        c.b = (Uint8)(t.fx_b.b + ((int)t.fx_a.b - (int)t.fx_b.b) * k);
                        // Per slice: slices stack vertically, they do not
                        // overlap, so this is the alpha that actually lands.
                        c.a = (Uint8)(110.0f * lum * ray * env * bandNorm);
                        if (c.a < 3) continue;
                        gfx->FillRectAdd(x, (int)(top + sliceH * s2), colStep,
                                         (int)sliceH + 1, c);
                    }
                }
            }
            if (gpu) gfx->FxEnd();
        }

        // Retrowave grid: a scrolling perspective floor under a banded sun,
        // with a hill silhouette along the horizon. Every parameter is a theme
        // field - the editor shows them under grid-specific labels, so the same
        // six knobs serve this and the ribbon.
        //
        //   line_count -> verticals        thickness -> line width
        //   amplitude  -> scroll speed     seed      -> hills (0 = none)
        //   layers     -> sun size         y_center  -> horizon
        // GPU retrowave, one program for the whole scene so it costs a single
        // hand-over from SDL per frame (each one flushes SDL's batch):
        //   mode 0  a row of the floor, across the screen at depth wz
        //   mode 1  a vertical, out along z at a fixed wx
        //   mode 3  the sun, one quad cut into its bands per pixel
        // The terrain is HeightAt's (C++ copy in DrawGridBackground, which
        // must match): random hills on the grid's own points, either side of
        // a flat strip down the middle. Wireframe only - nothing is filled.
        inline constexpr const char *kGridVs = R"(
uniform vec4 uTerr;  // height, seed, distance travelled, grid spacing in x
uniform vec4 uCam;   // eye, focal length, vanishing x, horizon
uniform vec4 uLine;  // mode, wz / wx / far wz, width, steps / near wz
uniform vec4 uSun;   // centre x, centre y, radius, horizon
varying vec2 vP;
varying float vA;
// Every grid point gets its own height: smooth value noise over a few cells
// makes the hills, a little per-point jitter keeps them rocky, and the rows
// and verticals run straight between points, so the hills come out faceted.
// The strip down the middle stays flat. Must match GridPointH in C++.
float PointH(float ix, float iz) {
    vec2 c = vec2(ix, iz) / 3.0, i = floor(c), f = fract(c);
    f = f * f * (3.0 - 2.0 * f);
    float s = uTerr.y;
    float n = mix(mix(Hash(i.x * 12.9898 + i.y * 78.233 + s), Hash((i.x + 1.0) * 12.9898 + i.y * 78.233 + s), f.x),
                  mix(Hash(i.x * 12.9898 + (i.y + 1.0) * 78.233 + s), Hash((i.x + 1.0) * 12.9898 + (i.y + 1.0) * 78.233 + s), f.x), f.y);
    float jit = Hash(ix * 3.17 + iz * 11.3 + s * 0.5);
    float side = clamp((abs(ix * uTerr.w) - 0.5) / 0.6, 0.0, 1.0);
    return uTerr.x * side * (n * n * 5.4 + 0.75 * jit);
}
float HeightAt(float wx, float wz) {
    if (uTerr.x <= 0.0) return 0.0;
    vec2 g = vec2(wx / uTerr.w, wz + uTerr.z), i = floor(g), f = g - i;
    return mix(mix(PointH(i.x, i.y), PointH(i.x + 1.0, i.y), f.x),
               mix(PointH(i.x, i.y + 1.0), PointH(i.x + 1.0, i.y + 1.0), f.x), f.y);
}
float ProjY(float wx, float wz) { return uCam.w + (uCam.x - HeightAt(wx, wz)) / wz; }
// Lines climbing a hill glow brighter the higher they get, so the hills read
// at a distance where the plain's own rows have faded out.
float Lift(float wx, float wz) { return min(HeightAt(wx, wz) / wz / 90.0, 1.0) * 0.8; }
void main() {
    float mode = uLine.x;
    vec2 p;
    vA = 0.0;
    if (mode > 2.5) {
        p = uSun.xy - uSun.z + Corner() * 2.0 * uSun.z;
    } else if (mode > 0.5) {
        float t = aV.x / uLine.w, wz = 0.26 + (17.0 - 0.26) * t * t;
        p = vec2(uCam.z + uLine.y * uCam.y / wz + (aV.y - 0.5) * uLine.z, ProjY(uLine.y, wz));
        vA = 150.0 / (0.6 + wz * 0.6) / 255.0 + Lift(uLine.y, wz);
    } else {
        float X = aV.x * 4.0, wz = uLine.y, wx = (X - uCam.z) * wz / uCam.y;
        p = vec2(X, ProjY(wx, wz) + aV.y * uLine.z);
        vA = 190.0 / (0.6 + wz * 0.9) / 255.0 + Lift(wx, wz);
    }
    vP = p;
    gl_Position = Clip(p);
}
)";
        inline constexpr const char *kGridFs = R"(
uniform vec4 uLine, uSun, uA, uB;
varying vec2 vP;
varying float vA;
void main() {
    if (uLine.x > 2.5) {                             // sun
        float r = uSun.z, top = uSun.y - r;
        float yy = top + floor((vP.y - top) / 2.0) * 2.0;
        float dy = yy - uSun.y;
        if (yy > uSun.w || abs(vP.x - uSun.x) > sqrt(max(0.0, r * r - dy * dy))) discard;
        if (dy > r * 0.10) {                         // slots, widening downward
            float band = 9.0 + dy / r * 20.0;
            if (mod(dy, band) > band * 0.52) discard;
        }
        float k = (min(floor((yy - top) / (2.0 * r) * 8.0), 7.0) + 0.5) / 8.0;
        gl_FragColor = vec4(mix(uB.rgb, uA.rgb, k) * (210.0 / 255.0), 210.0 / 255.0);
    } else {
        float a = min(vA, 1.0);
        gl_FragColor = vec4(uA.rgb * a, a);
    }
}
)";

        inline void DrawGridBackground(gfx::Gfx *gfx, const Theme &t) {
            const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
            const float now = (float)armGetSystemTick() / (float)armGetSystemTickFreq();

            int cols   = t.Fx().lines;  if (cols < 2)  cols = 2;   if (cols > 40) cols = 40;
            int width  = t.Fx().thickness;   if (width < 1) width = 1;  if (width > 20) width = 20;
            int speed  = t.Fx().amplitude;   if (speed < 1) speed = 1;  if (speed > 60) speed = 60;
            int hills  = t.Fx().seed;        if (hills < 0) hills = 0;  if (hills > 99) hills = 99;
            int sun    = t.Fx().layers;      if (sun < 1)   sun = 1;    if (sun > 12) sun = 12;
            float horizon = (float)t.Fx().y;
            if (horizon < 80.0f)          horizon = 80.0f;
            if (horizon > (float)H - 40)  horizon = (float)H - 40.0f;

            const float lw  = 0.5f + (float)width * 0.25f;
            const float vx  = W * 0.5f;
            const float run = now * ((float)speed * 0.03f);
            int cam = t.Fx().cam; if (cam < 5) cam = 5; if (cam > 200) cam = 200;
            const float eye   = ((float)H - horizon) * 0.01f * (float)cam;
            const float fx    = (float)W * 0.60f;
            const float terr  = (float)hills * 3.4f;   // screen lift is terr/wz, so far hills stay gentle
            const float hseed = (float)hills * 0.37f;
            const float spacing = 6.4f / (float)cols;   // between verticals, world x

            if (gfx->FxBegin(gfx->FxProgram(kGridVs, kGridFs))) {
                gfx->FxSet("uTerr", terr, hseed, run, spacing);
                gfx->FxSet("uCam", eye, fx, vx, horizon);
                gfx->FxSet("uA", t.fx_a);
                gfx->FxSet("uB", t.fx_b);
                if (sun > 1) {
                    const float r = (float)sun * 16.0f;
                    gfx->FxSet("uSun", (float)W * (float)t.Fx().x * 0.01f,
                               horizon - r * 0.30f, r, horizon);
                    gfx->FxSet("uLine", 3.0f, 0.0f, 0.0f, 0.0f);
                    gfx->FxQuads(1);
                }
                const float phase = fmodf(run, 1.0f);
                for (int i = 17; i >= 0; i--) {
                    const float wz = (float)i - phase;
                    if (wz < 0.26f) continue;
                    gfx->FxSet("uLine", 0.0f, wz, lw, 0.0f);
                    gfx->FxStrip(W / 4 + 1);
                }
                // Verticals run out across everything visible (|wx| < 0.83 wz
                // at the far row), so the hills out to the sides have both
                // sets of lines on them, not rows alone.
                const int steps = 40;
                const int reach = std::max(cols / 2, (int)ceilf(14.5f / spacing));
                for (int i = -reach; i <= reach; i++) {
                    gfx->FxSet("uLine", 1.0f, (float)i * spacing, lw, (float)steps);
                    gfx->FxStrip(steps + 1);
                }
                gfx->FxEnd();
                SDL_Color hz = t.fx_a; hz.a = 90;
                gfx->FillRect(0, (int)horizon, W, 1, hz);
                return;
            }

            // ---- sun: bands that thin out toward the bottom -----------------
            if (sun > 1) {
                const float r   = (float)sun * 16.0f;
                const float cy  = horizon - r * 0.30f;
                const float sx  = (float)W * (float)t.Fx().x * 0.01f;   // sun, not the vanishing point
                for (int q = 0; q < 8; q++) {          // 8 colour steps, 8 batches
                    alignas(16) SDL_Rect rows[64];
                    int n = 0;
                    const float k0 = (float)q / 8.0f, k1 = (float)(q + 1) / 8.0f;
                    for (float yy = cy - r + r * 2.0f * k0; yy < cy - r + r * 2.0f * k1; yy += 2.0f) {
                        if (yy > horizon) break;
                        const float dy = yy - cy;
                        const float hw = sqrtf(fmaxf(0.0f, r * r - dy * dy));
                        // Gaps widen toward the bottom of the disc, which is the
                        // whole look - a solid circle is just a circle.
                        // Slots, cut only below the middle and widening as they
                        // go down. Measured from the centre rather than from
                        // screen y, or the pattern slides with the horizon and
                        // the disc reads as striped noise instead of as a sun.
                        if (dy > r * 0.10f) {
                            const float g    = dy / r;                 // 0 .. 1
                            const float band = 9.0f + g * 20.0f;       // slot pitch
                            if (fmodf(dy, band) > band * 0.52f) continue;
                        }
                        if (n < 64) rows[n++] = SDL_Rect{ (int)(sx - hw), (int)yy, (int)(hw * 2), 2 };
                    }
                    const float k = (k0 + k1) * 0.5f;
                    SDL_Color c;                        // fx_b at the top -> fx_a at the base
                    c.r = (Uint8)(t.fx_b.r + ((int)t.fx_a.r - (int)t.fx_b.r) * k);
                    c.g = (Uint8)(t.fx_b.g + ((int)t.fx_a.g - (int)t.fx_b.g) * k);
                    c.b = (Uint8)(t.fx_b.b + ((int)t.fx_a.b - (int)t.fx_b.b) * k);
                    c.a = 210;
                    gfx->FillRects(rows, n, c);
                }
            }

            // ---- the floor, as a displaced wireframe -------------------------
            // Hills are the mesh, not a cardboard cut-out behind it: every
            // vertex is lifted by a height field, so the rows bend over the
            // rises and the verticals climb them. A silhouette pasted on the
            // horizon reads as a backdrop the moment anything moves; this
            // scrolls through the landscape because it is the landscape.
            //
            // Camera at the origin looking down +z. A point (wx, wz, height)
            // lands at vx + wx*f/wz across, and horizon + (eye - height)/wz
            // down - so height lifts a vertex toward the horizon, exactly as
            // distance flattens it.
            // Eye height above the ground. Low puts you down among the hills
            // with the rows stretched wide; high looks down on the grid and
            // tightens it. It scales the whole projection, so it is the one
            // number that changes the shape of the scene rather than its
            // contents.

            // Mirrors PointH / HeightAt in kGridVs: random hills on the grid's
            // own points (one per vertical, one per row, in world z so they
            // come toward you), flat down the middle, straight lines between.
            auto pointH = [&](float ix, float iz) -> float {
                const float cx = ix / 3.0f, cz = iz / 3.0f;
                const float i0 = floorf(cx), k0 = floorf(cz);
                float fx = cx - i0, fz = cz - k0;
                fx = fx * fx * (3.0f - 2.0f * fx);
                fz = fz * fz * (3.0f - 2.0f * fz);
                auto h = [&](float a, float b) { return BgHash(a * 12.9898f + b * 78.233f + hseed); };
                const float n0 = h(i0, k0) + (h(i0 + 1, k0) - h(i0, k0)) * fx;
                const float n1 = h(i0, k0 + 1) + (h(i0 + 1, k0 + 1) - h(i0, k0 + 1)) * fx;
                const float n  = n0 + (n1 - n0) * fz;
                const float jit  = BgHash(ix * 3.17f + iz * 11.3f + hseed * 0.5f);
                const float side = std::clamp((fabsf(ix * spacing) - 0.5f) / 0.6f, 0.0f, 1.0f);
                return terr * side * (n * n * 5.4f + 0.75f * jit);
            };
            auto heightAt = [&](float wx, float wz) -> float {
                if (terr <= 0.0f) return 0.0f;
                const float gx = wx / spacing, gz = wz + run;
                const float ix = floorf(gx), iz = floorf(gz);
                const float fx = gx - ix, fz = gz - iz;
                const float a = pointH(ix, iz)     + (pointH(ix + 1, iz)     - pointH(ix, iz))     * fx;
                const float b = pointH(ix, iz + 1) + (pointH(ix + 1, iz + 1) - pointH(ix, iz + 1)) * fx;
                return a + (b - a) * fz;
            };
            auto projY = [&](float h, float wz) { return horizon + (eye - h) / wz; };
            auto projX = [&](float wx, float wz) { return vx + wx * fx / wz; };

            // Rows: sampled across the screen rather than across the world.
            // Stepping in world x puts the near samples hundreds of pixels
            // apart, and a span between two far-apart points is a filled
            // bounding box - which is why the near rows came out as blocks.
            // Four pixels at a time keeps every span short enough to be a line.
            {
                const int STEP = 4;
                const int MAXN  = gfx::Gfx::Width / 4 + 2;
                alignas(16) SDL_Rect seg[MAXN];
                // Starts at i = 0, so the nearest row is under the bottom edge
                // rather than stopping short of it and leaving a bare strip.
                for (int i = 0; i <= 17; i++) {
                    const float wz = (float)i - fmodf(run, 1.0f);
                    if (wz < 0.26f) continue;

                    int n = 0;
                    float py = projY(heightAt((0.0f - vx) * wz / fx, wz), wz);
                    for (int x = STEP; x <= W && n < MAXN; x += STEP) {
                        const float wx = ((float)x - vx) * wz / fx;   // unproject
                        const float Y  = projY(heightAt(wx, wz), wz);
                        const int y0 = (int)fminf(py, Y), y1 = (int)fmaxf(py, Y);
                        seg[n++] = SDL_Rect{ x - STEP, y0, STEP, (y1 - y0) + (int)lw };
                        py = Y;
                    }
                    SDL_Color c = t.fx_a;
                    c.a = (Uint8)(190.0f / (0.6f + wz * 0.9f));
                    gfx->FillRects(seg, n, c);
                }
            }

            // Verticals: constant wx, walked out in z. Segmented so they follow
            // the ground rather than cutting through it, and drawn with the
            // antialiased line so the near ones do not stair-step.
            {
                const int STEPS = 10;
                constexpr float kNear = 0.26f;   // under the bottom edge, as above
                for (int i = -cols / 2; i <= cols / 2; i++) {
                    const float wx = (float)i * (6.4f / (float)cols);
                    float px = projX(wx, kNear);
                    float py = projY(heightAt(wx, kNear), kNear);
                    for (int s2 = 1; s2 <= STEPS; s2++) {
                        const float wz = kNear + (17.0f - kNear) * ((float)s2 / (float)STEPS)
                                                                 * ((float)s2 / (float)STEPS);
                        const float X = projX(wx, wz);
                        const float Y = projY(heightAt(wx, wz), wz);
                        SDL_Color c = t.fx_a;
                        c.a = (Uint8)(150.0f / (0.6f + wz * 0.6f));
                        gfx->LineAA(px, py, X, Y, c, lw);
                        px = X; py = Y;
                    }
                }
            }

            // Where the ground meets the sky. Bright enough to read as an edge,
            // not so bright that it becomes the brightest thing on screen - it
            // is the horizon, not a light source.
            SDL_Color hz = t.fx_a; hz.a = 90;
            gfx->FillRect(0, (int)horizon, W, 1, hz);
        }

        // Wallpaper snow: one quad per flake, placed in the vertex shader.
        inline constexpr const char *kSnowVs = R"(
varying float vA;
void main() {
    float seed = aV.z * 7.77;
    float sz  = 1.0 + floor((sin(seed * 1.7) * 0.5 + 0.5) * 3.0);
    float y   = floor(mod(uTime * (15.0 + sin(seed * 2.3) * 10.0) + seed * 100.0, 760.0) - 20.0);
    float x   = floor((sin(seed * 3.1) * 0.5 + 0.5) * 1280.0)
              + floor(sin(uTime * 0.5 + seed) * sin(seed * 5.1) * 30.0);
    vA = (60.0 + floor((sin(seed * 4.3) * 0.5 + 0.5) * 140.0)) / 255.0;
    gl_Position = Clip(vec2(x, y) + Corner() * sz);
}
)";
        inline constexpr const char *kSnowFs = R"(
varying float vA;
void main() { gl_FragColor = vec4(vA, vA, vA, vA); }
)";

        // Ocean, Hotline Miami 2 style: flat neon water in chunky pixels. Each
        // wave is a thin bright stripe across the screen; the stripes pack
        // tighter toward the horizon (spacing follows 1/distance), bend and
        // shimmer along their length, and drift toward the viewer. A column of
        // reflected light, broken into dashes by the same waves, wobbles down
        // from the horizon. Above the horizon nothing is drawn, so the theme's
        // own gradient is the sky. Nothing at all is drawn without the GPU
        // path; the gradient stands in.
        //
        //   thickness -> swell (how far stripes bend)   amplitude -> speed
        //   layers    -> wave density                   y -> horizon
        //   x         -> where the reflection falls, percent of width
        inline constexpr const char *kOceanVs = R"(
varying vec2 vP;
void main() {
    vP = aV.xy * vec2(1280.0, 720.0);
    gl_Position = Clip(vP);
}
)";
        inline constexpr const char *kOceanFs = R"(
uniform vec4 uDeep, uCrest;
uniform vec4 uSea;    // horizon y, swell, speed, density
uniform vec4 uSea2;   // reflection x, pixel size
varying vec2 vP;
void main() {
    float px = uSea2.y;
    vec2  p  = floor(vP / px) * px + px * 0.5;          // chunky pixels
    float hz = uSea.x;
    if (p.y < hz) discard;
    float d  = (p.y - hz) / max(720.0 - hz, 1.0);       // 0 horizon .. 1 bottom
    float t  = uTime * uSea.z;
    float z  = 1.0 / (d + 0.04);                        // distance out to sea

    float bend = sin(p.x * 0.011 + t * 1.3 + z * 0.7) * (0.25 + d)
               + sin(p.x * 0.029 - t * 2.3 + z * 1.9) * 0.45 * d;
    float wave = z * uSea.w * 2.2 + bend * uSea.y - t * 0.9;
    float band = fract(wave);
    float crest = step(0.80 - 0.10 * d, band);          // thicker up close

    float rw   = 10.0 + d * 150.0;
    float rx   = abs(p.x - uSea2.x + sin(p.y * 0.21 + t * 3.1) * d * 16.0);
    float dash = step(0.40, fract(wave * 2.0 + 0.25));
    float refl = step(rx, rw * (0.55 + 0.45 * sin(z * 2.7 + t * 4.0))) * dash;

    vec3 col = mix(uDeep.rgb * 0.45, uDeep.rgb, d);     // darker toward the horizon
    col = mix(col, uCrest.rgb, crest * (0.30 + 0.70 * d));
    col = mix(col, min(uCrest.rgb * 1.3 + 0.15, 1.0), refl * (1.0 - 0.45 * d));
    gl_FragColor = vec4(col, 1.0);
}
)";
        inline void DrawOceanBackground(gfx::Gfx *gfx, const Theme &t) {
            float horizon = (float)t.Fx().y;
            if (horizon < 40.0f)  horizon = 40.0f;
            if (horizon > 680.0f) horizon = 680.0f;
            if (!gfx->FxBegin(gfx->FxProgram(kOceanVs, kOceanFs))) return;
            gfx->FxSet("uDeep", t.fx_a);
            gfx->FxSet("uCrest", t.fx_b);
            gfx->FxSet("uSea", horizon,
                       0.15f + (float)t.Fx().thickness * 0.06f,     // swell
                       0.25f + (float)t.Fx().amplitude * 0.035f,    // speed
                       0.5f + (float)t.Fx().layers * 0.25f);        // density
            gfx->FxSet("uSea2", (float)gfx::Gfx::Width * (float)t.Fx().x * 0.01f, 3.0f, 0.0f, 0.0f);
            gfx->FxStrip(2);
            gfx->FxEnd();
        }

    // ---- per-entry tile config ----------------------------------------------
    //
    // One line per customised entry:  <ItemKey>=<w>x<h>,<rrggbb|->
    // Entries the user has not touched are simply absent, so the file stays
    // small and a default that changes later still reaches everyone.
    inline constexpr const char *kTileCfgFile = "tiles.txt";   // in the account config folder


    // ---- Flow: 3D coverflow -------------------------------------------------
    //
    // A fixed camera looking along +z at a row of quads rotated about y. The
    // centre item faces the camera and sits nearest; everything either side
    // swings away, drops back and slides outward. There is no scene graph and no
    // z-buffer - the row is drawn outside-in, so nearer covers simply paint over
    // farther ones.
    //
    // Distances are in arbitrary world units where a cover is one unit tall; the
    // camera focal length in Gfx turns those into pixels.
        // A Switch case is roughly 2:3, so the box is taller than it is wide.
        // The old square extent was what made the row read as rotated icons
        // rather than as boxes on a shelf.
        inline constexpr float kFlowHalf     = 0.50f;  // half-height
        inline float gFlowHalfW    = 0.34f;  // half-width (2:3 of the height)
        // Spacing has to clear the box's *on-screen* width, which depends on the
        // swing: at 34 degrees a box covers 2*halfW*cos(34) = 0.561, against
        // 0.36 at the old 58. Dropping the angle and the spacing together is
        // what made the covers touch - 0.46 spacing overlapped by 0.10.
        inline float gFlowSpacing  = 0.70f;  // centre-to-centre along the row
        inline float gFlowSideStep = 0.14f;  // extra shove away from centre
        // Beyond the first neighbour each step recedes a little further, so the
        // row falls away instead of standing as a flat wall at one depth - and
        // the nearest neighbour is unambiguously the largest of them.
        // Recession past the first neighbour, and a matching extra turn.
        //
        // Depth alone could not do this: it shrinks the on-screen spacing faster
        // than it shrinks the boxes, so anything strong enough to be visible
        // closed the gaps and overlapped the row. At 0.04 the second neighbour
        // was 3px narrower than the first - invisible, so the row read as one
        // flat plane and nothing looked like it was behind anything.
        //
        // Turning each further box a little more edge-on is what makes it work:
        // it narrows them, which buys back the spacing the recession costs, and
        // it reads as depth in its own right. Together they give 185 / 154 / 128
        // px across the first three neighbours while holding a positive gap.
        inline float gFlowZStep    = 0.30f;
        inline float gFlowAStep    = 0.10f;
        inline float gFlowZBase    = 2.35f;  // centre cover's distance
        inline float gFlowZBack    = 0.38f;  // how far the sides recede
        // Max swing. At the old ~58 degrees the neighbours were nearly edge-on,
        // so the row read as a wall of spines with one cover in it; at ~34 they
        // stay legible as boxes and you can see what is coming next.
        inline float gFlowAngle    = 0.60f;  // radians (~34 deg)
        inline constexpr float kFlowFloor    = -kFlowHalf;   // reflection plane
        inline float gFlowY        = 0.12f;  // row lifted a little off centre
        inline float gFlowDepth    = 0.030f; // half-thickness of the case
        inline constexpr int   kFlowVisible  = 6;      // items drawn either side
        // Covers held either side of the drawn range, and how many may be
        // decoded per frame while filling that margin.
        inline constexpr int   kFlowPreload  = 20;
        inline constexpr int   kFlowPrefetchPerFrame = 3;
        // Radians per second for the running game's idle turn. A shade under one
        // revolution every ten seconds: enough to catch the eye, slow enough not
        // to be a distraction while you read the row.
        inline constexpr float kFlowRunSpin  = 0.62f;

        // Everything above is a judgement call about how a shelf should look,
        // and the person looking at it is better placed to make it than a
        // constant in a source file. This table drives the settings screen, the
        // load and the save, so adding a knob means adding one row here.
        //
        // Angles are stored in radians and shown in degrees: nobody tunes a
        // shelf in radians.
        struct FlowParam {
            const char *label;
            float      *value;
            float       lo, hi, step;
            bool        degrees;
        };
        inline const FlowParam kFlowParams[] = {
            { "Box width",       &gFlowHalfW,    0.18f, 0.50f, 0.01f,  false },
            { "Box thickness",   &gFlowDepth,    0.005f,0.12f, 0.005f, false },
            { "Spacing",         &gFlowSpacing,  0.30f, 1.30f, 0.02f,  false },
            { "Centre gap",      &gFlowSideStep, 0.00f, 0.60f, 0.02f,  false },
            { "Turn",            &gFlowAngle,    0.00f, 1.40f, 0.02f,  true  },
            { "Turn per step",   &gFlowAStep,    0.00f, 0.30f, 0.01f,  true  },
            { "Camera distance", &gFlowZBase,    1.40f, 4.50f, 0.05f,  false },
            { "Depth drop",      &gFlowZBack,    0.00f, 1.50f, 0.02f,  false },
            { "Depth per step",  &gFlowZStep,    0.00f, 0.60f, 0.02f,  false },
            { "Row height",      &gFlowY,        -0.40f,0.40f, 0.02f,  false },
        };
        inline constexpr int kFlowParamN = (int)(sizeof(kFlowParams) / sizeof(kFlowParams[0]));

        // Config keys, and the defaults to fall back to on Reset. Kept in the
        // same order as the table above.
        inline const char *kFlowKeys[kFlowParamN] = {
            "flow_w", "flow_d", "flow_sp", "flow_gap", "flow_turn",
            "flow_turnstep", "flow_cam", "flow_drop", "flow_dropstep", "flow_y",
        };
        inline const float kFlowDefaults[kFlowParamN] = {
            0.34f, 0.030f, 0.70f, 0.14f, 0.60f, 0.10f, 2.35f, 0.38f, 0.30f, 0.12f,
        };

        // Named arrangements of the ten numbers above, in the spirit of the
        // Aurora coverflow layout packs. Everything a layout can express is
        // already in that table - how far apart the cases sit, how hard they
        // turn, how fast they fall away from the camera - so a layout is a row
        // of values rather than a second renderer. Tuning one afterwards still
        // works; the row simply reads Custom once it no longer matches any.
        struct FlowLayout { const char *name; float v[kFlowParamN]; };
        inline const FlowLayout kFlowLayouts[] = {
            // name          w      d       sp     gap    turn   tstep  cam    drop   dstep  y
            { "Coverflow", { 0.34f, 0.030f, 0.70f, 0.14f, 0.60f, 0.10f, 2.35f, 0.38f, 0.30f, 0.12f } },
            // Face-on, evenly spaced, nothing turned: a plain shelf.
            { "Flat",      { 0.34f, 0.030f, 0.75f, 0.00f, 0.00f, 0.00f, 2.35f, 0.00f, 0.00f, 0.12f } },
            // Barely turned, but each step falls sharply back, so the row bows.
            { "Arc",       { 0.34f, 0.030f, 0.62f, 0.10f, 0.25f, 0.04f, 2.60f, 0.10f, 0.55f, 0.12f } },
            // Small and tight - as many cases on screen as the row will carry.
            { "Wall",      { 0.26f, 0.020f, 0.48f, 0.06f, 0.35f, 0.06f, 2.10f, 0.20f, 0.12f, 0.10f } },
            // One big case held out front, the rest turned hard out of the way.
            { "Showcase",  { 0.42f, 0.040f, 0.95f, 0.34f, 1.05f, 0.16f, 2.60f, 0.55f, 0.34f, 0.14f } },
            // Turn keeps accumulating along the row, so it winds away from you.
            { "Spiral",    { 0.32f, 0.030f, 0.66f, 0.12f, 0.50f, 0.28f, 2.45f, 0.40f, 0.40f, 0.12f } },
        };
        inline constexpr int kFlowLayoutN = (int)(sizeof(kFlowLayouts) / sizeof(kFlowLayouts[0]));

        // Which layout the current numbers correspond to, or -1 once they have
        // been tuned away from all of them.
        inline int FlowLayoutMatch() {
            for (int i = 0; i < kFlowLayoutN; i++) {
                bool same = true;
                for (int k = 0; k < kFlowParamN && same; k++)
                    if (fabsf(*kFlowParams[k].value - kFlowLayouts[i].v[k]) > 0.0005f)
                        same = false;
                if (same) return i;
            }
            return -1;
        }

        // Wrap a row index into the list. With an endless row the drawn range
        // runs past both ends and every index is folded back, which is what lets
        // the shelf carry on into a second lap instead of stopping at the last
        // game.
        inline int FlowWrap(int i, int n) {
            if (n <= 0) return 0;
            i %= n;
            return (i < 0) ? i + n : i;
        }

        // Place one cover: how far it has swung, receded and slid aside, all
        // driven by its signed distance from the centre and all saturating at
        // one item out, which is what gives coverflow its "wall either side of a
        // single face" look rather than a smooth arc.
        inline void FlowPlace(float p, float &x, float &z, float &angle) {
            const float t = (p < -1.0f) ? -1.0f : (p > 1.0f ? 1.0f : p);
            angle = -t * gFlowAngle;
            x     = p * gFlowSpacing + t * gFlowSideStep;
            // The swing and the initial fall-back saturate one item out - that
            // is what gives coverflow its single upright face against a wall
            // either side. Depth then keeps creeping beyond that, so the wall
            // recedes rather than sitting flat.
            const float beyond = std::max(0.0f, std::abs(p) - 1.0f);
            z = gFlowZBase + std::abs(t) * gFlowZBack + beyond * gFlowZStep;
            // ...and each one past the first turns a little further away, which
            // is what keeps them from merging into a flat wall.
            if (beyond > 0.0f)
                angle += (t < 0.0f ? 1.0f : -1.0f) * beyond * gFlowAStep;
        }

        // Corners of a cover at (x, z) swung by angle, in view space, ordered
        // top-left, top-right, bottom-right, bottom-left.
        inline void FlowCorners(float x, float z, float angle, float halfW, float halfH,
                         float out[4][3]) {
            const float ca = cosf(angle), sa = sinf(angle);
            const float lx[4] = { -halfW,  halfW,  halfW, -halfW };
            const float ly[4] = {  halfH,  halfH, -halfH, -halfH };
            for (int i = 0; i < 4; i++) {
                out[i][0] = x + lx[i] * ca;
                out[i][1] = gFlowY + ly[i];
                out[i][2] = z - lx[i] * sa;
            }
        }

        // A face of the box, given in the box's own space: local x runs across
        // the face, local z is its depth offset. Lets the front and the spine be
        // built by the same code, differing only in which plane they lie in.
        inline void FlowFace(float x, float z, float angle,
                      float lx0, float lz0, float lx1, float lz1,
                      float halfH, float out[4][3]) {
            const float ca = cosf(angle), sa = sinf(angle);
            auto put = [&](int i, float lx, float lz, float ly) {
                out[i][0] = x + lx * ca + lz * sa;
                out[i][1] = gFlowY + ly;
                out[i][2] = z - lx * sa + lz * ca;
            };
            put(0, lx0, lz0,  halfH);   // TL
            put(1, lx1, lz1,  halfH);   // TR
            put(2, lx1, lz1, -halfH);   // BR
            put(3, lx0, lz0, -halfH);   // BL
        }

        // An arbitrary rectangle on one of the box's planes, in the box's own
        // space. FlowFace always spans the full height; the screenshots on the
        // back of a case do not, so they need this.
        inline void FlowPanel(float x, float z, float angle,
                       float lx0, float lx1, float ly0, float ly1, float lz,
                       float out[4][3]) {
            const float ca = cosf(angle), sa = sinf(angle);
            auto put = [&](int i, float lx, float ly) {
                out[i][0] = x + lx * ca + lz * sa;
                out[i][1] = gFlowY + ly;
                out[i][2] = z - lx * sa + lz * ca;
            };
            put(0, lx0, ly1);   // TL
            put(1, lx1, ly1);   // TR
            put(2, lx1, ly0);   // BR
            put(3, lx0, ly0);   // BL
        }

        // Panel boundaries within the box wrap, as fractions of its width. The
        // template is back | red spine | front, so the spine is the thin strip
        // between them.
        inline constexpr float kWrapSpine0 = 0.478f;
        inline constexpr float kWrapSpine1 = 0.518f;
        // Front panel: everything right of the spine. Its printed furniture (the
        // Switch logo, the rating block) is opaque and the middle is clear, so
        // laying it over the art is what turns a cover into a boxed game.
        inline constexpr float kWrapFront0 = 0.518f;
        inline constexpr float kWrapFront1 = 1.0f;

    // ---- decoded-cover cache -------------------------------------------------
    //
    // SteamGridDB serves its grids as PNG, and they are saved here with a .jpg
    // name because SDL sniffs the real format. That matters more than it looks:
    // PNG is zlib inflate plus a per-row unfilter, all on the CPU, and a row of
    // thirteen 600x900 covers is around seven megapixels of it. Redoing that on
    // every launch is what made the shelf take a second or two to fill in.
    //
    // So the finished, already-downscaled pixels are kept, exactly as the blur
    // cache keeps its result: loading one becomes a read and an upload with no
    // decode at all.
    //
    // Stored as RGB565 rather than RGBA. Box art is opaque, so the alpha channel
    // is dead weight, and halving the file halves the read - which is the whole
    // cost once the decode is gone. The banding that costs is not visible at the
    // size a case is drawn.
        inline constexpr const char *kCoverTexDir = "sdmc:/slaunch/cache/covertex";
        inline constexpr u32 kCoverTexMagic   = 0x31565343;   // 'CSV1'
        inline constexpr u32 kCoverTexVersion = 1;
        // Matches what the decode path produced before, so nothing about how a
        // case looks changes - only how quickly it gets there.
        inline constexpr int kCoverTexW = 480;
        inline constexpr int kCoverTexH = 720;

        struct CoverTexHeader {
            u32 magic, version, w, h;
            u64 src_size, src_mtime;
        };

        // Covers are 480x720; the back panels are screenshots, drawn a couple
        // of hundred pixels wide, so the same treatment applies to both.
        inline constexpr int kShotTexW = 640;
        inline constexpr int kShotTexH = 207;

        inline std::string TexCachePath(const char *key) {
            char buf[112];
            snprintf(buf, sizeof(buf), "%s/%s.ctx", kCoverTexDir, key);
            return std::string(buf);
        }

        // Any mismatch is a miss, and a miss just means the decode runs as it
        // always did. The source's size and mtime are checked, so replacing a
        // cover - by hand or through the picker - rebuilds this entry.
        // Reads the header and says whether it describes this source at this
        // size. Leaves `f` just past the header, where the pixels start.
        inline bool CoverHeaderMatches(FILE *f, const struct stat &src, int tw, int th) {
            CoverTexHeader h {};
            return fread(&h, sizeof(h), 1, f) == 1 &&
                   h.magic == kCoverTexMagic && h.version == kCoverTexVersion &&
                   h.w == (u32)tw && h.h == (u32)th &&
                   h.src_size  == (u64)src.st_size &&
                   h.src_mtime == (u64)src.st_mtime;
        }
        // Whether the cache already holds this source: the header only, for
        // the warm-up pass, which has no use for the pixels.
        inline bool CoverCacheFresh(const char *cpath, const struct stat &src, int tw, int th) {
            FILE *f = fopen(cpath, "rb");
            if (!f) return false;
            const bool ok = CoverHeaderMatches(f, src, tw, th);
            fclose(f);
            return ok;
        }

        // A surface, not a texture, so this can run on the art worker.
        inline SDL_Surface *ReadCoverSurf(const char *cpath, const struct stat &src,
                                          int tw, int th) {
            FILE *f = fopen(cpath, "rb");
            if (!f) return nullptr;
            if (!CoverHeaderMatches(f, src, tw, th)) { fclose(f); return nullptr; }

            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 16,
                                                               SDL_PIXELFORMAT_RGB565);
            if (!surf) { fclose(f); return nullptr; }
            // One read for the whole image when rows are packed (they are for
            // every size we cache): each read is a round trip to the card's
            // filesystem, and 720 of them cost more than the data does.
            bool ok = true;
            if (surf->pitch == tw * 2)
                ok = fread(surf->pixels, 1, (size_t)tw * th * 2, f) == (size_t)tw * th * 2;
            else
                for (int y = 0; ok && y < th; y++)
                    ok = fread((u8 *)surf->pixels + (size_t)y * surf->pitch,
                               1, (size_t)tw * 2, f) == (size_t)tw * 2;
            fclose(f);
            if (!ok) { SDL_FreeSurface(surf); return nullptr; }   // truncated; rebuild
            return surf;
        }

        inline void WriteCoverTex(const char *cpath, const struct stat &src,
                           SDL_Surface *surf) {
            if (!surf) return;
            const int tw = surf->w, th = surf->h;

            mkdir("sdmc:/slaunch", 0777);
            mkdir("sdmc:/slaunch/cache", 0777);
            mkdir(kCoverTexDir, 0777);

            // Written beside and renamed in, so a power cut mid-write cannot
            // leave a full-length file with a stale tail that reads back as
            // valid.
            const std::string tmp = std::string(cpath) + ".tmp";
            FILE *f = fopen(tmp.c_str(), "wb");
            if (!f) return;

            CoverTexHeader h {};
            h.magic     = kCoverTexMagic;
            h.version   = kCoverTexVersion;
            h.w         = (u32)tw;
            h.h         = (u32)th;
            h.src_size  = (u64)src.st_size;
            h.src_mtime = (u64)src.st_mtime;

            bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
            // Row by row: a surface's pitch can carry padding past the last
            // pixel of a row, and writing it would shear the image on read-back.
            for (int y = 0; ok && y < th; y++)
                ok = fwrite((const u8 *)surf->pixels + (size_t)y * surf->pitch,
                            1, (size_t)tw * 2, f) == (size_t)tw * 2;
            fclose(f);
            if (!ok) { remove(tmp.c_str()); return; }

            remove(cpath);                    // FAT rename will not overwrite
            rename(tmp.c_str(), cpath);
        }

        // Decode and downscale to exactly the cached shape, as a surface, so the
        // pixels can be written out before they are handed to the GPU.
        inline SDL_Surface *DecodeCoverSurface(const char *path, int tw, int th) {
            SDL_Surface *raw = IMG_Load(path);
            if (!raw) return nullptr;

            SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(
                    0, tw, th, 16, SDL_PIXELFORMAT_RGB565);
            if (!dst) { SDL_FreeSurface(raw); return nullptr; }

            SDL_BlitScaled(raw, nullptr, dst, nullptr);
            SDL_FreeSurface(raw);
            return dst;
        }

        // Same, but scale-to-cover + centre-crop instead of stretch - see
        // Gfx::LoadImageCropped, which this mirrors exactly except for
        // returning a cacheable surface instead of a texture. Used for hero
        // art (1920x620 at source, nowhere near the 16:9 box it is shown in).
        inline SDL_Surface *DecodeCoverSurfaceCropped(const char *path, int tw, int th,
                                                      float biasY) {
            SDL_Surface *raw = IMG_Load(path);
            if (!raw) return nullptr;

            const float sx = (float)tw / (float)raw->w;
            const float sy = (float)th / (float)raw->h;
            const float scale = sx > sy ? sx : sy;
            int cw = (int)((float)tw / scale + 0.5f);
            int ch = (int)((float)th / scale + 0.5f);
            if (cw > raw->w) cw = raw->w;
            if (ch > raw->h) ch = raw->h;
            SDL_Rect crop { (raw->w - cw) / 2, (int)((float)(raw->h - ch) * biasY), cw, ch };

            SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(
                    0, tw, th, 16, SDL_PIXELFORMAT_RGB565);
            if (!dst) { SDL_FreeSurface(raw); return nullptr; }

            SDL_BlitScaled(raw, &crop, dst, nullptr);
            SDL_FreeSurface(raw);
            return dst;
        }

} // namespace sl::menu::ui
