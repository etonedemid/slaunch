#include <sl/menu/ui/Menu.hpp>
#include <sl/menu/ui/Locale.hpp>
#include <sl/smi/Protocol.hpp>
#include <SDL2/SDL_image.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "Menu_Internal.hpp"

// File manager: browse the SD card, open what can be opened (homebrew runs,
// pictures are shown), and copy / move / rename / delete / make folders.
//
// Copies and deletes can be large - a game dump, a whole folder of ROMs - so
// they run on a worker with a progress bar and can be cancelled; everything
// else is instant and stays on the main thread. Sizes are read only for the
// rows on screen, so a folder of thousands of files opens at once.

namespace sl::menu::ui {

    namespace {
        constexpr int kFmRowH = 44, kFmTop = 118, kFmRows = 11;

        bool IsImage(const std::string &n) {
            const char *e = strrchr(n.c_str(), '.');
            return e && (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") ||
                         !strcasecmp(e, ".png") || !strcasecmp(e, ".bmp"));
        }
        bool IsNro(const std::string &n) {
            const char *e = strrchr(n.c_str(), '.');
            return e && !strcasecmp(e, ".nro");
        }
        std::string Join(const std::string &dir, const std::string &name) {
            return (!dir.empty() && dir.back() == '/') ? dir + name : dir + "/" + name;
        }
        std::string Parent(const std::string &path) {
            const size_t s = path.find_last_of('/');
            if (s == std::string::npos || path.compare(0, 6, "sdmc:/") != 0 || path.size() <= 6)
                return "sdmc:/";
            return s <= 5 ? "sdmc:/" : path.substr(0, s);
        }
        std::string BaseName(const std::string &path) {
            const size_t s = path.find_last_of('/');
            return s == std::string::npos ? path : path.substr(s + 1);
        }
        std::string HumanSize(u64 b) {
            char buf[32];
            if      (b >= (1ull << 30)) snprintf(buf, sizeof(buf), "%.1f GB", b / 1073741824.0);
            else if (b >= (1ull << 20)) snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
            else if (b >= (1ull << 10)) snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
            else                        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
            return buf;
        }
        bool IsDir(const std::string &p) {
            struct stat st {};
            return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        bool Exists(const std::string &p) {
            struct stat st {};
            return stat(p.c_str(), &st) == 0;
        }
        // "name (copy)", "name (2)", ... - whichever is free, keeping the extension.
        std::string FreeName(const std::string &dir, const std::string &name) {
            if (!Exists(Join(dir, name))) return Join(dir, name);
            const size_t dot = name.find_last_of('.');
            const bool ext = dot != std::string::npos && dot > 0;
            const std::string stem = ext ? name.substr(0, dot) : name;
            const std::string tail = ext ? name.substr(dot) : std::string();
            for (int i = 1; i < 1000; i++) {
                const std::string cand = stem + (i == 1 ? std::string(" (copy)")
                                                        : " (" + std::to_string(i) + ")") + tail;
                if (!Exists(Join(dir, cand))) return Join(dir, cand);
            }
            return Join(dir, name + ".copy");
        }
    }

    // ---- listing ----------------------------------------------------------------
    void Menu::FmScan() {
        m_fm_list.clear();
        if (DIR *d = opendir(m_fm_path.c_str())) {
            while (struct dirent *e = readdir(d)) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                FmEntry fe;
                fe.name = e->d_name;
                fe.dir  = (e->d_type == DT_DIR) ||
                          (e->d_type == DT_UNKNOWN && IsDir(Join(m_fm_path, fe.name)));
                m_fm_list.push_back(std::move(fe));
            }
            closedir(d);
        }
        std::sort(m_fm_list.begin(), m_fm_list.end(), [](const FmEntry &a, const FmEntry &b) {
            if (a.dir != b.dir) return a.dir;
            return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        if (m_fm_cursor >= (int)m_fm_list.size()) m_fm_cursor = std::max(0, (int)m_fm_list.size() - 1);
    }
    void Menu::OpenFileManager() {
        m_screen = Screen::Files;
        if (m_fm_path.empty()) m_fm_path = "sdmc:/";
        m_fm_cursor = 0;
        m_fm_scroll = 0.0f;
        m_fm_menu_open = false;
        FmScan();
    }
    // Enter a folder, or go up to `path` keeping the cursor on where we came from.
    void Menu::FmGo(const std::string &path, const std::string &select) {
        m_fm_path = path;
        m_fm_cursor = 0;
        FmScan();
        for (int i = 0; i < (int)m_fm_list.size(); i++)
            if (m_fm_list[i].name == select) { m_fm_cursor = i; break; }
        m_fm_scroll = (float)std::max(0, m_fm_cursor - kFmRows / 2);
    }

    // ---- background copy / move / delete ---------------------------------------
    namespace {
        u64 TreeBytes(const std::string &p) {
            struct stat st {};
            if (stat(p.c_str(), &st) != 0) return 0;
            if (!S_ISDIR(st.st_mode)) return (u64)st.st_size;
            u64 total = 0;
            if (DIR *d = opendir(p.c_str())) {
                while (struct dirent *e = readdir(d))
                    if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                        total += TreeBytes(Join(p, e->d_name));
                closedir(d);
            }
            return total;
        }
    }
    bool Menu::FmCopyTree(const std::string &src, const std::string &dst) {
        if (m_fm_cancel.load()) return false;
        if (IsDir(src)) {
            if (mkdir(dst.c_str(), 0777) != 0 && !IsDir(dst)) return false;
            bool ok = true;
            if (DIR *d = opendir(src.c_str())) {
                while (struct dirent *e = readdir(d)) {
                    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                    if (!FmCopyTree(Join(src, e->d_name), Join(dst, e->d_name))) { ok = false; break; }
                }
                closedir(d);
            }
            return ok;
        }
        FILE *in = fopen(src.c_str(), "rb");
        if (!in) return false;
        FILE *out = fopen(dst.c_str(), "wb");
        if (!out) { fclose(in); return false; }
        std::vector<char> buf(1 << 20);
        bool ok = true;
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), in)) > 0) {
            if (m_fm_cancel.load() || fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
            m_fm_done_bytes.fetch_add(n);
        }
        fclose(in);
        fclose(out);
        if (!ok) remove(dst.c_str());   // no half files left behind
        return ok;
    }
    bool Menu::FmDeleteTree(const std::string &p) {
        if (m_fm_cancel.load()) return false;
        if (IsDir(p)) {
            if (DIR *d = opendir(p.c_str())) {
                std::vector<std::string> kids;
                while (struct dirent *e = readdir(d))
                    if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                        kids.push_back(Join(p, e->d_name));
                closedir(d);
                for (const auto &k : kids) if (!FmDeleteTree(k)) return false;
            }
            return rmdir(p.c_str()) == 0;
        }
        struct stat st {};
        if (stat(p.c_str(), &st) == 0) m_fm_done_bytes.fetch_add((u64)st.st_size);
        return remove(p.c_str()) == 0;
    }
    void Menu::FmWorkerTrampoline(void *self) {
        Menu *m = static_cast<Menu *>(self);
        bool ok = true;
        m->m_fm_total_bytes.store(TreeBytes(m->m_fm_src));
        switch (m->m_fm_op) {
            case FmOp::Copy:
                ok = m->FmCopyTree(m->m_fm_src, m->m_fm_dst);
                break;
            case FmOp::Move:
                // Same card, so a rename is instant; only fall back to copying
                // (then deleting) if the file system refuses it.
                if (::rename(m->m_fm_src.c_str(), m->m_fm_dst.c_str()) != 0) {
                    ok = m->FmCopyTree(m->m_fm_src, m->m_fm_dst) && m->FmDeleteTree(m->m_fm_src);
                }
                break;
            case FmOp::Delete:
                ok = m->FmDeleteTree(m->m_fm_src);
                break;
            default: break;
        }
        m->m_fm_ok = ok;
        m->m_fm_done.store(true, std::memory_order_release);
    }
    void Menu::FmStart(FmOp op, const std::string &src, const std::string &dst) {
        if (m_fm_running) return;
        m_fm_op = op; m_fm_src = src; m_fm_dst = dst;
        m_fm_done_bytes.store(0); m_fm_total_bytes.store(0);
        m_fm_cancel.store(false); m_fm_done.store(false);
        if (R_SUCCEEDED(threadCreate(&m_fm_thread, &Menu::FmWorkerTrampoline, this,
                                     nullptr, 0x10000, 0x3B, -2))) {
            threadStart(&m_fm_thread);
            m_fm_running = true;
        } else {
            SetStatus("Could not start");
        }
    }
    void Menu::FmPoll() {
        if (!m_fm_running || !m_fm_done.load(std::memory_order_acquire)) return;
        threadWaitForExit(&m_fm_thread);
        threadClose(&m_fm_thread);
        m_fm_running = false;
        if (m_fm_cancel.load())  SetStatus("Cancelled");
        else if (!m_fm_ok)       SetStatus("Something went wrong - nothing was lost");
        else                     SetStatus("Done");
        if (m_fm_op == FmOp::Move && m_fm_ok) m_fm_clip.clear();
        FmGo(m_fm_path, BaseName(m_fm_dst.empty() ? m_fm_src : m_fm_dst));
    }

    // ---- actions ----------------------------------------------------------------
    std::vector<Menu::FmAction> Menu::FmActions() const {
        std::vector<FmAction> v;
        const bool have = m_fm_cursor >= 0 && m_fm_cursor < (int)m_fm_list.size();
        if (have) { v.push_back(FmAction::Copy); v.push_back(FmAction::Cut); }
        if (!m_fm_clip.empty()) v.push_back(FmAction::Paste);
        if (have) { v.push_back(FmAction::Rename); v.push_back(FmAction::Delete); }
        v.push_back(FmAction::NewFolder);
        return v;
    }
    const char *Menu::FmActionName(FmAction a) {
        switch (a) {
            case FmAction::Copy:      return "Copy";
            case FmAction::Cut:       return "Cut";
            case FmAction::Paste:     return "Paste here";
            case FmAction::Rename:    return "Rename";
            case FmAction::Delete:    return "Delete";
            case FmAction::NewFolder: return "New folder";
        }
        return "";
    }
    void Menu::FmDo(FmAction a) {
        const bool have = m_fm_cursor >= 0 && m_fm_cursor < (int)m_fm_list.size();
        const std::string sel = have ? Join(m_fm_path, m_fm_list[m_fm_cursor].name) : std::string();
        switch (a) {
            case FmAction::Copy: m_fm_clip = sel; m_fm_clip_cut = false; SetStatus("Copied"); break;
            case FmAction::Cut:  m_fm_clip = sel; m_fm_clip_cut = true;  SetStatus("Cut"); break;
            case FmAction::Paste: {
                if (!Exists(m_fm_clip)) { m_fm_clip.clear(); SetStatus("That is gone"); break; }
                // A folder cannot go inside itself.
                const std::string src = m_fm_clip;
                if (m_fm_path == src || m_fm_path.compare(0, src.size() + 1, src + "/") == 0) {
                    SetStatus("A folder cannot go inside itself");
                    break;
                }
                if (m_fm_clip_cut && Parent(src) == m_fm_path) { m_fm_clip.clear(); break; }
                FmStart(m_fm_clip_cut ? FmOp::Move : FmOp::Copy, src,
                        FreeName(m_fm_path, BaseName(src)));
                break;
            }
            case FmAction::Rename:
                m_kb_purpose = sl::smi::Kb_FileRename;
                m_kb_text = m_fm_list[m_fm_cursor].name;
                m_kb_row = m_kb_col = 0; m_kb_upper = false;
                m_screen = Screen::Keyboard;
                break;
            case FmAction::NewFolder:
                m_kb_purpose = sl::smi::Kb_NewFolder;
                m_kb_text.clear();
                m_kb_row = m_kb_col = 0; m_kb_upper = true;
                m_screen = Screen::Keyboard;
                break;
            case FmAction::Delete:
                m_dialog        = Dialog::ConfirmDelete;
                m_dialog_cursor = 1;   // "No" first
                m_dialog_title  = T("Delete this?");
                m_dialog_note   = m_fm_list[m_fm_cursor].name +
                                  (m_fm_list[m_fm_cursor].dir ? std::string("  ") + T("and everything in it") : "");
                break;
        }
    }
    // Keyboard results (see OnButtonKeyboard's commit).
    void Menu::FmKeyboardDone(bool is_rename, const std::string &text) {
        m_screen = Screen::Files;
        if (text.empty() || text.find('/') != std::string::npos) { SetStatus("Not a valid name"); return; }
        if (is_rename) {
            const std::string from = Join(m_fm_path, m_fm_list[m_fm_cursor].name);
            const std::string to   = Join(m_fm_path, text);
            if (Exists(to))                               SetStatus("That name is taken");
            else if (::rename(from.c_str(), to.c_str()) == 0) { SetStatus("Renamed"); FmGo(m_fm_path, text); }
            else                                          SetStatus("Could not rename");
        } else {
            const std::string p = Join(m_fm_path, text);
            if (Exists(p))                         SetStatus("That name is taken");
            else if (mkdir(p.c_str(), 0777) == 0) { FmGo(m_fm_path, text); }
            else                                   SetStatus("Could not create the folder");
        }
    }
    void Menu::FmConfirmDelete() {
        if (m_fm_cursor < 0 || m_fm_cursor >= (int)m_fm_list.size()) return;
        FmStart(FmOp::Delete, Join(m_fm_path, m_fm_list[m_fm_cursor].name), std::string());
    }

    // ---- input ------------------------------------------------------------------
    Menu::Action Menu::OnButtonFiles(Btn b) {
        if (m_fm_view) {                                 // picture on screen
            if (b == Btn::B || b == Btn::A) { m_gfx->FreeImage(m_fm_view); m_fm_view = nullptr; }
            return Action::None;
        }
        if (m_fm_running) {                              // only cancel while busy
            if (b == Btn::B) m_fm_cancel.store(true);
            return Action::None;
        }
        if (m_fm_menu_open) {
            const auto acts = FmActions();
            const int n = (int)acts.size();
            if (b == Btn::Down) m_fm_menu_cursor = (m_fm_menu_cursor + 1) % n;
            if (b == Btn::Up)   m_fm_menu_cursor = (m_fm_menu_cursor + n - 1) % n;
            if (b == Btn::B || b == Btn::X) m_fm_menu_open = false;
            if (b == Btn::A) { m_fm_menu_open = false; FmDo(acts[m_fm_menu_cursor]); }
            return Action::None;
        }
        const int n = (int)m_fm_list.size();
        if (b == Btn::B) {
            if (m_fm_path == "sdmc:/") { m_screen = BackTarget(); return Action::None; }
            FmGo(Parent(m_fm_path), BaseName(m_fm_path));
            return Action::None;
        }
        if (b == Btn::X) { m_fm_menu_open = true; m_fm_menu_cursor = 0; return Action::None; }
        if (n == 0) return Action::None;
        if (b == Btn::Down) m_fm_cursor = (m_fm_cursor + 1) % n;
        if (b == Btn::Up)   m_fm_cursor = (m_fm_cursor + n - 1) % n;
        if (b == Btn::R || b == Btn::Right) m_fm_cursor = std::min(n - 1, m_fm_cursor + kFmRows);
        if (b == Btn::L || b == Btn::Left)  m_fm_cursor = std::max(0, m_fm_cursor - kFmRows);
        if (b == Btn::A) {
            const FmEntry &e = m_fm_list[m_fm_cursor];
            const std::string p = Join(m_fm_path, e.name);
            if (e.dir) {
                FmGo(p, std::string());
            } else if (IsNro(e.name)) {
                m_hb_launch_path = p;
                m_hb_launch_argv.clear();
                return m_hb_donor ? Action::LaunchHomebrewApp : Action::LaunchHomebrew;
            } else if (IsImage(e.name)) {
                m_fm_view = m_gfx->LoadImage(p.c_str());
                if (!m_fm_view) SetStatus("Could not open that picture");
            } else {
                SetStatus("Nothing here opens this kind of file");
            }
        }
        return Action::None;
    }
    void Menu::OnTouchFiles(int x, int y) {
        (void)x;
        if (m_fm_view || m_fm_running || m_fm_menu_open) return;
        if (y < kFmTop || y >= kFmTop + kFmRows * kFmRowH) return;
        const int i = (int)lroundf(m_fm_scroll) + (y - kFmTop) / kFmRowH;
        if (i < 0 || i >= (int)m_fm_list.size()) return;
        if (i == m_fm_cursor) OnButtonFiles(Btn::A);
        else                  m_fm_cursor = i;
    }

    // ---- drawing ------------------------------------------------------------------
    void Menu::DrawFiles() {
        const Theme &t = m_theme.Current();
        const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
        FmPoll();

        if (m_fm_view) {
            m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, 255 });
            m_gfx->DrawCover(m_fm_view, 255);
            DrawHint({ {{"b"}, "Back"} });
            return;
        }

        DrawTopBar("Files");
        // Where we are, trimmed from the left so the end of a long path shows.
        {
            std::string shown = m_fm_path;
            const int maxw = W - 80;
            while (shown.size() > 8 && m_gfx->TextWidth(FontSize::Small, shown.c_str()) > maxw)
                shown = "..." + shown.substr(4);
            m_gfx->Text(FontSize::Small, 40, 80, t.dim, shown.c_str());
        }

        const int n = (int)m_fm_list.size();
        if (n == 0)
            m_gfx->TextCentered(FontSize::Normal, W / 2, 320, t.dim, T("This folder is empty"));

        const float top = (float)std::clamp(m_fm_cursor - kFmRows / 2, 0, std::max(0, n - kFmRows));
        m_fm_scroll += (top - m_fm_scroll) * 0.30f;
        if (std::abs(top - m_fm_scroll) < 0.01f) m_fm_scroll = top;

        static SDL_Texture *folder = nullptr;
        static bool folder_tried = false;
        if (!folder_tried) { folder_tried = true; folder = m_gfx->LoadImage("sdmc:/slaunch/icons/fm_folder.png"); }

        const int first = std::max(0, (int)m_fm_scroll - 1);
        const int last  = std::min(n - 1, (int)m_fm_scroll + kFmRows + 1);
        const int lh = m_gfx->LineHeight(FontSize::Normal);
        for (int i = first; i <= last; i++) {
            const int y = kFmTop + (int)(((float)i - m_fm_scroll) * kFmRowH);
            if (y < kFmTop - kFmRowH / 2 || y > kFmTop + (kFmRows - 1) * kFmRowH + kFmRowH / 2) continue;
            FmEntry &e = m_fm_list[i];
            const bool sel = (i == m_fm_cursor);
            if (sel) m_gfx->FillRect(28, y, W - 56, kFmRowH - 4, WithAlpha(t.accent, 44));

            // Icon: the folder picture, or a page with the file's extension.
            const int ix = 44, iy = y + 5, is = kFmRowH - 14;
            if (e.dir) {
                if (folder) m_gfx->DrawImage(folder, ix - 2, iy + 2, is + 4, (is + 4) * 100 / 128, 255);
                else        m_gfx->FillRect(ix, iy + 4, is, is - 8, SDL_Color{ 240, 190, 30, 255 });
            } else {
                m_gfx->FillRect(ix + 4, iy, is - 8, is, WithAlpha(t.fg, 200));
                m_gfx->FillTriangle(ix + is - 12, iy, ix + is - 4, iy + 8, ix + is - 12, iy + 8,
                                    WithAlpha(t.bg_bottom, 255));
            }

            // Size, read the first time a file is on screen.
            std::string right;
            if (!e.dir) {
                if (e.size < 0) {
                    struct stat st {};
                    e.size = stat(Join(m_fm_path, e.name).c_str(), &st) == 0 ? (long long)st.st_size : 0;
                }
                right = HumanSize((u64)e.size);
            }
            const int rw = right.empty() ? 0 : m_gfx->TextWidth(FontSize::Small, right.c_str());
            const bool clipped = (!m_fm_clip.empty() && Join(m_fm_path, e.name) == m_fm_clip);
            m_gfx->Text(FontSize::Normal, ix + is + 18, y + (kFmRowH - 4 - lh) / 2,
                        WithAlpha(clipped ? t.accent : (sel ? t.title : t.fg), 255),
                        Ellipsize(e.name, W - 56 - (ix + is + 18) - rw - 40, FontSize::Normal).c_str());
            if (!right.empty())
                m_gfx->Text(FontSize::Small, W - 48 - rw,
                            y + (kFmRowH - 4 - m_gfx->LineHeight(FontSize::Small)) / 2, t.dim, right.c_str());
        }

        // What is on the clipboard, above the hints.
        if (!m_fm_clip.empty()) {
            const std::string c = std::string(m_fm_clip_cut ? T("Moving") : T("Copying")) + ": " + BaseName(m_fm_clip);
            m_gfx->Text(FontSize::Small, 40, kHintY - 50, t.accent,
                        Ellipsize(c, W - 80, FontSize::Small).c_str());
        }

        // X menu.
        if (m_fm_menu_open) {
            const auto acts = FmActions();
            const int pw = 320, ph = (int)acts.size() * 46 + 20, px = W - pw - 60, py = 140;
            m_gfx->FillRect(px, py, pw, ph, WithAlpha(t.bg_bottom, 245));
            m_gfx->FillRect(px, py, pw, 3, t.accent);
            for (int i = 0; i < (int)acts.size(); i++) {
                const int ry = py + 10 + i * 46;
                if (i == m_fm_menu_cursor) m_gfx->FillRect(px + 8, ry, pw - 16, 42, WithAlpha(t.accent, 50));
                m_gfx->Text(FontSize::Normal, px + 24, ry + (42 - lh) / 2,
                            i == m_fm_menu_cursor ? t.title : t.fg, T(FmActionName(acts[i])));
            }
        }

        // Copy / move / delete in progress.
        if (m_fm_running) {
            const int pw = 620, ph = 150, px = (W - pw) / 2, py = (H - ph) / 2;
            m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, 140 });
            m_gfx->FillRect(px, py, pw, ph, WithAlpha(t.bg_bottom, 250));
            m_gfx->FillRect(px, py, pw, 3, t.accent);
            const char *what = m_fm_op == FmOp::Delete ? T("Deleting") :
                               m_fm_op == FmOp::Move   ? T("Moving")   : T("Copying");
            m_gfx->Text(FontSize::Normal, px + 24, py + 22, t.title,
                        Ellipsize(std::string(what) + " " + BaseName(m_fm_src), pw - 48, FontSize::Normal).c_str());
            const u64 total = m_fm_total_bytes.load(), done = m_fm_done_bytes.load();
            const float f = total ? std::min(1.0f, (float)done / (float)total) : 0.0f;
            m_gfx->FillRect(px + 24, py + 80, pw - 48, 8, WithAlpha(t.dim, 70));
            m_gfx->FillRect(px + 24, py + 80, (int)((pw - 48) * f), 8, t.accent);
            const std::string prog = HumanSize(done) + " / " + HumanSize(total);
            m_gfx->Text(FontSize::Small, px + 24, py + 100, t.dim, prog.c_str());
            DrawHint({ {{"b"}, "Cancel"} });
            return;
        }

        if (m_fm_menu_open)
            DrawHint({ {{"up","down"}, "Choose"}, {{"a"}, "Confirm"}, {{"b"}, "Cancel"} });
        else
            DrawStatusHint({ {{"a"}, "Open"}, {{"b"}, "Back"}, {{"x"}, "Actions"},
                             {{"l","r"}, "Page"} });
    }


    // ---- USB file transfer status -------------------------------------------
    void Menu::PollUsb() {
        usb::MtpGetStatus(m_usb);
        const u64 now = armGetSystemTick(), freq = armGetSystemTickFreq();
        if (m_usb.op != usb::MtpOp::None) { m_usb_active_tick = now; m_usb_last_op = m_usb.op; }

        // Rate over roughly half-second windows, smoothed so the number is
        // readable rather than jumping every time a buffer lands.
        if (m_usb_rate_tick == 0) { m_usb_rate_tick = now; m_usb_rate_bytes = m_usb.bytes; }
        const u64 dt = now - m_usb_rate_tick;
        if (dt >= freq / 2) {
            const double r = (double)(m_usb.bytes - m_usb_rate_bytes) * freq / dt;
            m_usb_rate = m_usb_rate <= 0.0 ? r : m_usb_rate * 0.6 + r * 0.4;
            m_usb_rate_tick = now;
            m_usb_rate_bytes = m_usb.bytes;
        }

        // Something arrived or went: the folder on screen may be out of date.
        const u32 changes = m_usb.received + m_usb.deleted;
        if (changes != m_usb_changes) {
            m_usb_changes = changes;
            if (m_screen == Screen::Files) FmScan();
        }
    }
    void Menu::DrawUsbTag(int right_x, int y) {
        if (!m_usb.connected) return;
        const Theme &t = m_theme.Current();
        const int w = m_gfx->TextWidth(FontSize::Small, "USB");
        m_gfx->Text(FontSize::Small, right_x - w, y, t.accent, "USB");
    }
    void Menu::DrawUsbStatus() {
        PollUsb();
        const u64 now = armGetSystemTick(), freq = armGetSystemTickFreq();
        const bool moving = m_usb.op != usb::MtpOp::None;
        // Held a moment after the last file so a run of small ones reads as
        // one transfer instead of a flickering card, then a summary, then gone.
        const u64 idle_ms = moving ? 0 : (now - m_usb_active_tick) * 1000 / freq;
        if (m_usb_active_tick == 0 || idle_ms > 4000) return;
        const float a = idle_ms > 3400 ? 1.0f - (idle_ms - 3400) / 600.0f : 1.0f;
        const Uint8 A = (Uint8)(255 * a);
        const bool show_file = moving || idle_ms < 800;

        const Theme &t = m_theme.Current();
        // Under the clock and battery, which XMB's header draws lower down.
        const int pw = 380, ph = 76, px = gfx::Gfx::Width - 40 - pw;
        const int py = m_ui_mode == UiMode::XMB
                     ? kXmbTitleTop + 10 + m_gfx->LineHeight(FontSize::Small) + 14 : 52;
        m_gfx->FillRect(px, py, pw, ph, WithAlpha(t.bg_bottom, (Uint8)(235 * a)));
        m_gfx->FillRect(px, py, pw, 3, WithAlpha(t.accent, A));
        const int tx = px + 16, lh = m_gfx->LineHeight(FontSize::Small);

        std::string l1, l2, rate;
        if (show_file) {
            static const char *kVerb[] = { "", "Receiving", "Sending", "Deleting" };
            const char *base = strrchr(m_usb.name, '/');
            base = base ? base + 1 : m_usb.name;
            l1 = std::string(T(kVerb[(int)m_usb_last_op])) + "  " + base;
            if (m_usb_last_op == usb::MtpOp::Delete) {
                l2.clear();
            } else if (m_usb.total > 0) {
                char pct[16];
                snprintf(pct, sizeof(pct), "%d%%  ", (int)(m_usb.done * 100 / m_usb.total));
                l2 = pct + HumanSize(m_usb.done) + " / " + HumanSize(m_usb.total);
            } else {
                l2 = HumanSize(m_usb.done);
            }
            if (moving && m_usb_last_op != usb::MtpOp::Delete && m_usb_rate > 1024.0)
                rate = HumanSize((u64)m_usb_rate) + "/s";
        } else {
            l1 = T("USB transfer done");
            char buf[96];
            snprintf(buf, sizeof(buf), T("%u received, %u sent, %u deleted"),
                     m_usb.received, m_usb.sent, m_usb.deleted);
            l2 = buf;
        }
        // The rate sits at the right end of the first line; the name gives way.
        const int rw = rate.empty() ? 0 : m_gfx->TextWidth(FontSize::Small, rate.c_str()) + 16;
        if (rw) m_gfx->Text(FontSize::Small, px + pw - 16 - (rw - 16), py + 12,
                            WithAlpha(t.accent, A), rate.c_str());
        m_gfx->Text(FontSize::Small, tx, py + 12, WithAlpha(t.fg, A),
                    Ellipsize(l1, pw - 32 - rw, FontSize::Small).c_str());
        m_gfx->Text(FontSize::Small, tx, py + 14 + lh, WithAlpha(t.dim, A),
                    Ellipsize(l2, pw - 32, FontSize::Small).c_str());

        // Progress under the text: real when the size is known, a sliding
        // block when it is not (files over 4 GB, deletes).
        if (!show_file || !moving) return;
        const int bx = tx, by = py + ph - 10, bw = pw - 32;
        m_gfx->FillRect(bx, by, bw, 3, WithAlpha(t.dim, (Uint8)(60 * a)));
        if (m_usb.total > 0) {
            m_gfx->FillRect(bx, by, (int)(bw * std::min(1.0, (double)m_usb.done / m_usb.total)), 3,
                            WithAlpha(t.accent, A));
        } else {
            const float phs = fmodf((float)now / freq * 0.8f, 1.0f);
            const int seg = bw / 4, sx = bx + (int)((bw + seg) * phs) - seg;
            const int x0 = std::max(bx, sx), x1 = std::min(bx + bw, sx + seg);
            if (x1 > x0) m_gfx->FillRect(x0, by, x1 - x0, 3, WithAlpha(t.accent, A));
        }
    }
} // namespace sl::menu::ui
