#include <sl/menu/ui/TextUI.hpp>
#include <cstdio>
#include <cstring>
#include <algorithm>

// ANSI escape helpers — libnx console supports these
#define ESC "\x1b["
#define RESET    ESC "0m"
#define BOLD     ESC "1m"
#define DIM      ESC "2m"
#define REVERSE  ESC "7m"

// Colors (foreground)
#define FG_BLACK   ESC "30m"
#define FG_RED     ESC "31m"
#define FG_GREEN   ESC "32m"
#define FG_YELLOW  ESC "33m"
#define FG_BLUE    ESC "34m"
#define FG_MAGENTA ESC "35m"
#define FG_CYAN    ESC "36m"
#define FG_WHITE   ESC "37m"
#define FG_BRIGHT_BLACK   ESC "90m"
#define FG_BRIGHT_RED     ESC "91m"
#define FG_BRIGHT_GREEN   ESC "92m"
#define FG_BRIGHT_YELLOW  ESC "93m"
#define FG_BRIGHT_BLUE    ESC "94m"
#define FG_BRIGHT_WHITE   ESC "97m"

// Background colors
#define BG_BLACK  ESC "40m"
#define BG_BLUE   ESC "44m"
#define BG_WHITE  ESC "47m"

// Clear screen / cursor control
#define CLEAR_SCREEN ESC "2J" ESC "H"
#define CLEAR_LINE   ESC "2K"

namespace sl::menu::ui {

    // -------------------------------------------------------------------------
    // Helpers

    void TextUI::MoveTo(int col, int row) {
        // ANSI: ESC[row;colH  (1-based)
        printf(ESC "%d;%dH", row + 1, col + 1);
    }

    void TextUI::ClearLine(int row) {
        MoveTo(0, row);
        printf(CLEAR_LINE);
    }

    // -------------------------------------------------------------------------

    TextUI::TextUI() {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&m_pad);
    }

    void TextUI::Init(AccountUid selected_user, u64 suspended_app_id) {
        m_user      = selected_user;
        m_suspended = suspended_app_id;
        m_dirty     = true;

        // Get nickname from account system
        AccountProfile profile;
        AccountProfileBase base = {};
        if (R_SUCCEEDED(accountGetProfile(&profile, m_user))) {
            accountProfileGet(&profile, &base, nullptr);
            accountProfileClose(&profile);
            strncpy(m_nickname, base.nickname, 32);
            m_nickname[32] = '\0';
        }

        // Hide cursor and clear screen
        printf(ESC "?25l");   // hide cursor
        printf(CLEAR_SCREEN);
    }

    void TextUI::SetApps(std::vector<AppEntry> apps) {
        m_apps   = std::move(apps);
        m_cursor = 0;
        m_scroll = 0;
        m_dirty  = true;
    }

    void TextUI::SetSuspendedApp(u64 app_id) {
        m_suspended = app_id;
        m_dirty     = true;
    }

    void TextUI::ClearSuspendedApp() {
        m_suspended = 0;
        m_dirty     = true;
    }

    void TextUI::SetUser(AccountUid uid, const char *nickname) {
        m_user = uid;
        strncpy(m_nickname, nickname, 32);
        m_nickname[32] = '\0';
        m_dirty = true;
    }

    void TextUI::SetStatus(const char *msg) {
        strncpy(m_status, msg, 127);
        m_status[127] = '\0';
        m_status_tick = armGetSystemTick();
        m_dirty       = true;
    }

    // -------------------------------------------------------------------------
    // Drawing

    void TextUI::DrawHeader() {
        // Row 0: top border
        MoveTo(0, 0);
        printf(BG_BLUE BOLD FG_BRIGHT_WHITE);
        // Fill entire line with spaces first
        for (int i = 0; i < Cols; i++) putchar(' ');

        // Title (left)
        MoveTo(1, 0);
        printf("sLaunch");

        // User (right side)
        char right_buf[64];
        snprintf(right_buf, sizeof(right_buf), "User: %.20s", m_nickname);
        int rlen = (int)strlen(right_buf);
        MoveTo(Cols - rlen - 1, 0);
        printf("%s", right_buf);

        printf(RESET);

        // Row 1: separator + section label
        MoveTo(0, 1);
        printf(FG_BRIGHT_BLACK);
        for (int i = 0; i < Cols; i++) putchar('-');
        printf(RESET);

        MoveTo(1, 1);
        printf(BOLD FG_CYAN "INSTALLED GAMES");
        if (!m_apps.empty()) {
            printf(" (%zu)" RESET, m_apps.size());
        } else {
            printf(RESET FG_BRIGHT_BLACK " (none)" RESET);
        }

        if (m_suspended != 0) {
            // Show suspended indicator
            const char *tag = "  [game running - press HOME to resume]";
            MoveTo(Cols - (int)strlen(tag) - 1, 1);
            printf(FG_BRIGHT_YELLOW "%s" RESET, tag);
        }

        // Row 2: separator
        MoveTo(0, 2);
        printf(FG_BRIGHT_BLACK);
        for (int i = 0; i < Cols; i++) putchar('-');
        printf(RESET);
    }

    void TextUI::DrawAppList() {
        // List occupies rows 3 .. (3 + ListRows - 1)
        constexpr int ListStart = 3;

        for (int i = 0; i < ListRows; i++) {
            int idx = m_scroll + i;
            ClearLine(ListStart + i);
            MoveTo(0, ListStart + i);

            if (idx >= (int)m_apps.size()) {
                // Empty row
                continue;
            }

            const auto &app = m_apps[idx];
            bool selected   = (idx == m_cursor);
            bool suspended  = (app.app_id == m_suspended);

            // Cursor indicator
            if (selected) {
                printf(REVERSE FG_BRIGHT_WHITE " > ");
            } else {
                printf("   ");
            }

            // Running indicator
            if (suspended) {
                printf(FG_BRIGHT_GREEN "[RUN] " RESET);
            } else if (app.is_gamecard) {
                printf(FG_BRIGHT_BLUE "[CARD]" RESET " ");
            } else {
                printf("       ");
            }

            // Game name
            const std::string &name = app.name;
            constexpr int MaxNameLen = Cols - 20;
            if ((int)name.size() > MaxNameLen) {
                printf("%.*s...", MaxNameLen - 3, name.c_str());
            } else {
                printf("%-*s", MaxNameLen, name.c_str());
            }

            // Update badge
            if (app.needs_update) {
                printf(FG_BRIGHT_YELLOW " [UPD]" RESET);
            }

            if (selected) printf(RESET);
        }

        // Scroll indicator (right side, rows 3-ListRows)
        if ((int)m_apps.size() > ListRows) {
            // Simple percentage indicator
            int pct = (m_apps.size() <= 1) ? 0
                    : (m_scroll * 100 / (int)(m_apps.size() - ListRows));
            MoveTo(Cols - 5, ListStart + ListRows / 2);
            printf(FG_BRIGHT_BLACK "%3d%%" RESET, pct);
        }
    }

    void TextUI::DrawFooter() {
        int footer_row = Rows - 2;
        int status_row = Rows - 1;

        // Separator
        MoveTo(0, footer_row - 1);
        printf(FG_BRIGHT_BLACK);
        for (int i = 0; i < Cols; i++) putchar('-');
        printf(RESET);

        // Controls hint
        ClearLine(footer_row);
        MoveTo(0, footer_row);
        printf(DIM FG_WHITE);
        if (m_suspended != 0) {
            printf(" A:Launch  B:Options  HOME:Resume  X:Album  +:Power  -:Settings  "
                   "up/down:Nav  L/R:Page  Y:Homebrew");
        } else {
            printf(" A:Launch  B:Options  X:Album  +:Power  -:Settings  "
                   "up/down:Navigate  L/R:Page  Y:Homebrew");
        }
        printf(RESET);

        DrawStatusBar();
    }

    void TextUI::DrawStatusBar() {
        int row = Rows - 1;
        ClearLine(row);

        // Only show status message if it's fresh (< 3 seconds)
        if (m_status[0] != '\0') {
            u64 now  = armGetSystemTick();
            u64 freq = armGetSystemTickFreq();
            if ((now - m_status_tick) < 3 * freq) {
                MoveTo(0, row);
                printf(FG_BRIGHT_YELLOW " %s" RESET, m_status);
                return;
            }
            m_status[0] = '\0';
        }

        // Default: show app count + basic info
        MoveTo(0, row);
        printf(FG_BRIGHT_BLACK " %zu game(s) installed" RESET,
               m_apps.size());
    }

    // -------------------------------------------------------------------------
    // Input + update

    TextUI::Action TextUI::Update(u64 &out_app_id) {
        padUpdate(&m_pad);
        u64 down = padGetButtonsDown(&m_pad);
        out_app_id = 0;

        if (m_apps.empty()) {
            if (down & HidNpadButton_Plus) return Action::OpenPower;
            return Action::None;
        }

        bool moved = false;

        // Navigation
        if (down & HidNpadButton_AnyDown) {
            if (m_cursor < (int)m_apps.size() - 1) {
                m_cursor++;
                if (m_cursor >= m_scroll + ListRows)
                    m_scroll = m_cursor - ListRows + 1;
                moved = true;
            }
        }
        if (down & HidNpadButton_AnyUp) {
            if (m_cursor > 0) {
                m_cursor--;
                if (m_cursor < m_scroll)
                    m_scroll = m_cursor;
                moved = true;
            }
        }
        // Page down
        if (down & HidNpadButton_R) {
            m_cursor = std::min(m_cursor + ListRows, (int)m_apps.size() - 1);
            m_scroll = std::max(0, m_cursor - ListRows + 1);
            moved    = true;
        }
        // Page up
        if (down & HidNpadButton_L) {
            m_cursor = std::max(0, m_cursor - ListRows);
            m_scroll = std::max(0, m_cursor - ListRows + 1);
            moved    = true;
        }

        if (moved) { m_dirty = true; return Action::None; }

        // Launch (A button)
        if (down & HidNpadButton_A) {
            out_app_id = m_apps[m_cursor].app_id;
            return Action::LaunchApp;
        }

        // Options menu (B button) — for now: terminate if running, else nothing
        if (down & HidNpadButton_B) {
            if (m_suspended != 0 && m_apps[m_cursor].app_id == m_suspended) {
                return Action::TerminateApp;
            }
        }

        // HOME / resume (only shown when game is running)
        if (down & HidNpadButton_Minus) {
            if (m_suspended != 0)
                return Action::ResumeApp;
        }

        // Album (X button)
        if (down & HidNpadButton_X) {
            return Action::OpenAlbum;
        }

        // User page (Y button for now — future: homebrew browser)
        if (down & HidNpadButton_Y) {
            return Action::OpenUserPage;
        }

        // Power menu (+ button)
        if (down & HidNpadButton_Plus) {
            return Action::OpenPower;
        }

        return Action::None;
    }

    // -------------------------------------------------------------------------

    void TextUI::Render() {
        if (!m_dirty) return;
        m_dirty = false;

        DrawHeader();
        DrawAppList();
        DrawFooter();

        // Flush stdout — libnx console writes via printf to framebuffer
        fflush(stdout);

        // Present the framebuffer
        // (libnx console flushes automatically on vsync or we can call
        // framebufferPresent if using the raw FB API. With consoleUpdate it's explicit.)
        consoleUpdate(nullptr);
    }

} // namespace sl::menu::ui
