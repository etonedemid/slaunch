// Discord, on HomeNX's client (libs/discord): servers and DMs on the left, the chat
// on the right. It connects when this screen opens, or at start when an account is
// saved and notifications are on (banners for DMs and mentions), and stops whenever
// a game or applet takes over.
//
// Pictures are decoded on a worker; an animated one (GIF, WebP) becomes one sheet
// of frames, so it costs one texture.
// ponytail: an animation over its budget keeps every other frame (twice as long each).
#ifdef SL_QEXT
#include <sl/menu/net/Http.hpp>
#include <sl/menu/ui/Menu.hpp>
#include <sl/menu/ui/Locale.hpp>
#include <sl/smi/Protocol.hpp>
#include <SDL3_image/SDL_image.h>
#include <ctime>
#include <cmath>
#include <cstring>
#include "Menu_Internal.hpp"
#include <sl/menu/qext/GameToast.hpp>
#include "social/DiscordService.hpp"
#include "net/async.h"
#include "net/http.h"
#include "util/log.h"
#include "third_party/qrcodegen.hpp"
#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <memory>
#include <dirent.h>
#include <sys/stat.h>

namespace sl::menu::ui {

    using gfx::FontSize;

    namespace {
        constexpr int kDcRailX = 24, kDcRailW = 72, kDcRailRowH = 66, kDcTop = 84, kDcBottom = 660;
        constexpr int kDcListX = 112, kDcListRowH = 52;
        constexpr int kDcChatX = 40, kDcChatLineH = 30, kDcTextX = kDcChatX + 54;
        constexpr int kDcAvatar = 40, kDcEmoji = 26, kDcJumbo = 48;
        constexpr int kDcInputH = 46, kDcInputY = kDcBottom - kDcInputH;
        constexpr int kDcMediaW = 360, kDcMediaH = 220;
        constexpr size_t kDcMaxPictures = 200;
        constexpr long long kDcMaxUpload = 10ll << 20;   // Discord's limit without Nitro
        constexpr size_t kDcSheetBytes = 10u << 20;       // one animation's frames
        constexpr int kDcMaxFrames = 120, kDcSheetMax = 4096;

        // A downloaded picture, ready for the main thread to upload.
        struct Decoded {
            SDL_Surface *sheet = nullptr;
            int frames = 1, cols = 1, fw = 0, fh = 0;
            std::vector<int> ends;   // each frame's end, ms from the start
        };

        SDL_Surface *Scaled(SDL_Surface *src, int w, int h) {
            if (!src) return nullptr;
            SDL_Surface *rgba = SDL_ConvertSurface(src, SDL_PIXELFORMAT_RGBA32);
            if (!rgba) return nullptr;
            if (rgba->w == w && rgba->h == h) return rgba;
            SDL_Surface *dst = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
            if (dst) SDL_BlitSurfaceScaled(rgba, nullptr, dst, nullptr, SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(rgba);
            return dst;
        }

        // w x h shrunk to the picture's own shape (never enlarged).
        void FitBox(int sw, int sh, int &w, int &h) {
            if (sw <= 0 || sh <= 0) return;
            const float s = std::min(1.0f, std::min((float)w / sw, (float)h / sh));
            w = std::max(1, (int)(sw * s));
            h = std::max(1, (int)(sh * s));
        }

        // Worker thread. An animation comes back as a sheet of w x h frames; fit
        // keeps the picture's shape inside w x h instead of stretching it.
        Decoded Decode(const std::string &body, int w, int h, bool fit = false) {
            Decoded out;
            out.fw = w;
            out.fh = h;
            const char *type = nullptr;
            if (body.compare(0, 4, "GIF8") == 0) type = "GIF";
            else if (body.compare(0, 4, "RIFF") == 0 && body.compare(8, 4, "WEBP") == 0 &&
                     body.find("ANIM") < 128) type = "WEBP";
            if (type) {
                if (IMG_AnimationDecoder *dec = IMG_CreateAnimationDecoder_IO(
                        SDL_IOFromConstMem(body.data(), body.size()), true, type)) {
                    int cap = kDcMaxFrames;
                    std::vector<SDL_Surface *> frames;
                    std::vector<int> delays;
                    SDL_Surface *f = nullptr;
                    Uint64 ms = 0;
                    for (int n = 0; n < 600 && IMG_GetAnimationDecoderFrame(dec, &f, &ms) && f; n++) {
                        if (n == 0) {
                            if (fit) FitBox(f->w, f->h, w, h);
                            out.fw = w;
                            out.fh = h;
                            const size_t per = (size_t)w * h * 4;
                            cap = (int)std::clamp<size_t>(kDcSheetBytes / std::max<size_t>(per, 1), 8, kDcMaxFrames);
                        }
                        SDL_Surface *s = Scaled(f, w, h);
                        SDL_DestroySurface(f);
                        f = nullptr;
                        if (!s) break;
                        frames.push_back(s);
                        delays.push_back(ms <= 10 ? 100 : (int)std::min<Uint64>(ms, 10000));   // as browsers do
                        if ((int)frames.size() > cap) {   // over budget: every other frame
                            std::vector<SDL_Surface *> kf;
                            std::vector<int> kd;
                            for (size_t i = 0; i < frames.size(); i++) {
                                if (i % 2 == 0) { kf.push_back(frames[i]); kd.push_back(delays[i]); }
                                else { kd.back() += delays[i]; SDL_DestroySurface(frames[i]); }
                            }
                            frames.swap(kf);
                            delays.swap(kd);
                        }
                    }
                    IMG_CloseAnimationDecoder(dec);
                    if (frames.size() > 1) {
                        out.frames = (int)frames.size();
                        out.cols = std::max(1, std::min(out.frames, kDcSheetMax / w));
                        const int rows = (out.frames + out.cols - 1) / out.cols;
                        out.sheet = SDL_CreateSurface(out.cols * w, rows * h, SDL_PIXELFORMAT_RGBA32);
                        int t = 0;
                        for (int i = 0; i < out.frames; i++) {
                            if (out.sheet) {
                                SDL_Rect r{ (i % out.cols) * w, (i / out.cols) * h, w, h };
                                SDL_SetSurfaceBlendMode(frames[i], SDL_BLENDMODE_NONE);
                                SDL_BlitSurface(frames[i], nullptr, out.sheet, &r);
                            }
                            SDL_DestroySurface(frames[i]);
                            out.ends.push_back(t += delays[i]);
                        }
                        if (out.sheet) return out;
                        out.frames = 1;
                        out.ends.clear();
                    } else {
                        for (SDL_Surface *s : frames) { out.sheet = s; }   // one frame: a still
                        if (out.sheet) return out;
                    }
                }
            }
            SDL_Surface *raw = IMG_Load_IO(SDL_IOFromConstMem(body.data(), body.size()), true);
            if (raw && fit) FitBox(raw->w, raw->h, w, h);
            out.fw = w;
            out.fh = h;
            out.sheet = Scaled(raw, w, h);
            if (raw) SDL_DestroySurface(raw);
            return out;
        }

        std::string ReadFile(const std::string &path, size_t max) {
            std::string s;
            if (FILE *f = fopen(path.c_str(), "rb")) {
                char buf[64 * 1024];
                size_t n;
                while (s.size() <= max && (n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
                fclose(f);
            }
            return s.size() > max ? std::string() : s;
        }

        bool IsPictureFile(const std::string &p) {
            std::string e = p.substr(p.find_last_of('.') + 1);
            for (char &c : e) c = (char)tolower((unsigned char)c);
            return e == "jpg" || e == "jpeg" || e == "png" || e == "gif" || e == "webp" || e == "bmp";
        }

        std::string SizeText(long long b) {
            char s[32];
            if (b >= (1 << 20)) snprintf(s, sizeof(s), "%.1f MB", b / 1048576.0);
            else                snprintf(s, sizeof(s), "%.0f KB", b / 1024.0);
            return s;
        }

        enum HitKind { HRail, HList, HMedia, HInput, HPlus, HSend, HText, HCancel, HEmoji, HEmojiBtn, HSendDraft, HAudio };

        // The picker's emoji: what goes in the message, and its Twemoji file name.
        struct EmoEntry { const char *text, *name; };
        constexpr EmoEntry kEmo[] = {
            { "\U0001F602", "1f602" }, { "\U0001F923", "1f923" }, { "\U0001F62D", "1f62d" }, { "\U0001F60A", "1f60a" },
            { "\U0001F60D", "1f60d" }, { "\U0001F970", "1f970" }, { "\U0001F618", "1f618" }, { "\U0001F60E", "1f60e" },
            { "\U0001F914", "1f914" }, { "\U0001F644", "1f644" }, { "\U0001F60F", "1f60f" }, { "\U0001F605", "1f605" },
            { "\U0001F642", "1f642" }, { "\U0001F643", "1f643" }, { "\U0001F609", "1f609" }, { "\U0001F607", "1f607" },
            { "\U0001F929", "1f929" }, { "\U0001F973", "1f973" }, { "\U0001F97A", "1f97a" }, { "\U0001F634", "1f634" },
            { "\U0001F631", "1f631" }, { "\U0001F621", "1f621" }, { "\U0001F92F", "1f92f" }, { "\U0001F608", "1f608" },
            { "\U0001F480", "1f480" }, { "\U0001F47B", "1f47b" }, { "\U0001F916", "1f916" }, { "\U0001F4A9", "1f4a9" },
            { "\U0001F921", "1f921" }, { "\U0001F44D", "1f44d" }, { "\U0001F44E", "1f44e" }, { "\U0001F44F", "1f44f" },
            { "\U0001F64F", "1f64f" }, { "\U0001F4AA", "1f4aa" }, { "\U0001F44B", "1f44b" }, { "\U0001F91D", "1f91d" },
            { "\U0001F64C", "1f64c" }, { "\U0001F440", "1f440" }, { "\U0001F525", "1f525" }, { "\U0001F4AF", "1f4af" },
            { "\u2728", "2728" },          { "\U0001F389", "1f389" }, { "\U0001F381", "1f381" }, { "\U0001F3AE", "1f3ae" },
            { "\U0001F3C6", "1f3c6" }, { "\U0001F680", "1f680" }, { "\u2B50", "2b50" },          { "\U0001F4A5", "1f4a5" },
            { "\u2764\uFE0F", "2764" },    { "\U0001F9E1", "1f9e1" }, { "\U0001F49B", "1f49b" }, { "\U0001F49A", "1f49a" },
            { "\U0001F499", "1f499" }, { "\U0001F49C", "1f49c" }, { "\U0001F5A4", "1f5a4" }, { "\U0001F494", "1f494" },
            { "\U0001F355", "1f355" }, { "\U0001F37A", "1f37a" }, { "\U0001F431", "1f431" }, { "\U0001F436", "1f436" },
        };
        constexpr int kEmoN = (int)(sizeof(kEmo) / sizeof(kEmo[0])), kEmoCols = 12, kEmoCell = 56;
        std::string EmoUrl(int i) {
            return std::string("https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/") + kEmo[i].name + ".png";
        }
        // The picker emoji that starts s at i, or -1.
        int EmoAt(const std::string &s, size_t i) {
            for (int k = 0; k < kEmoN; k++)
                if (s.compare(i, strlen(kEmo[k].text), kEmo[k].text) == 0) return k;
            return -1;
        }

        struct Pic {
            SDL_Texture *tex = nullptr;   // nullptr: loading, or failed
            int frames = 1, cols = 1, fw = 0, fh = 0;
            std::vector<int> ends;
            bool failed = false;          // nothing to show (a symbol Twemoji has no picture for)
        };

        // The character a Twemoji picture stands for, from its file name
        // (".../2605.png" is U+2605): drawn as text when there is no picture.
        std::string EmojiChars(const std::string &url) {
            if (url.find("/twemoji") == std::string::npos) return std::string();
            const size_t a = url.find_last_of('/') + 1, b = url.rfind(".png");
            if (b == std::string::npos || b <= a) return std::string();
            std::string out;
            for (size_t i = a; i < b;) {
                const uint32_t cp = (uint32_t)strtoul(url.c_str() + i, nullptr, 16);
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
                       out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                const size_t dash = url.find('-', i);
                if (dash == std::string::npos || dash > b) break;
                i = dash + 1;
            }
            return out;
        }

        // Emoji reach the text as U+F0000 + i (bytes F3 B0..BF xx xx); -1 if not one.
        int EmojiAt(const std::string &s, size_t i) {
            if (i + 3 >= s.size() || (unsigned char)s[i] != 0xF3 || (unsigned char)s[i + 1] < 0xB0) return -1;
            const uint32_t cp = ((uint32_t)((unsigned char)s[i] & 7) << 18) | ((uint32_t)((unsigned char)s[i + 1] & 0x3F) << 12) |
                                ((uint32_t)((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
            return (int)(cp - 0xF0000);
        }

        // Where the picker opens: the newest day of the album, when it is on the card.
        std::string NewestAlbumDir() {
            std::string dir = "sdmc:/Nintendo/Album";
            struct stat st {};
            if (stat(dir.c_str(), &st) != 0) return "sdmc:/";
            for (int depth = 0; depth < 3; depth++) {   // year, month, day
                std::string best;
                if (DIR *d = opendir(dir.c_str())) {
                    while (struct dirent *e = readdir(d))
                        if (e->d_name[0] != '.' && std::string(e->d_name) > best) best = e->d_name;
                    closedir(d);
                }
                if (best.empty()) break;
                const std::string next = dir + "/" + best;
                if (stat(next.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) break;
                dir = next;
            }
            return dir;
        }

        // Emoji marks dropped, for names and banners.
        std::string Plain(const std::string &s) {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); i++) {
                const unsigned char c = (unsigned char)s[i], n = i + 1 < s.size() ? (unsigned char)s[i + 1] : 0;
                if (c == 0xF3 && n >= 0xB0) { i += 3; continue; }
                out += s[i];
            }
            return out;
        }
    }

    struct DiscordUi {
        struct Row { Snowflake id = 0; std::string text, avatar; bool category = false, voice = false, unread = false; int mentions = 0; };
        struct Piece { std::string text; int emoji = -1, x = 0; };   // emoji: index into urls
        struct ChatRow {
            enum Kind { Head, Text, Media, Reply, Audio, Video } kind = Text;   // Reply: text = who, time = what they said. Audio, Video: url = file, text = name
            std::vector<Piece> pieces;      // Text; Head uses text for the name
            std::string text, time, url, alt;   // Head: name, time, avatar. Media: the picture (alt: a still)
            std::string full;                   // Media: a bigger still for the viewer
            int ow = 0, oh = 0;                 // Media: the picture's own size
            int h = kDcChatLineH, w = 0, ph = 0, esize = kDcEmoji;
            SDL_Color color{};
        };

        DiscordService svc;
        std::shared_ptr<bool> alive = std::make_shared<bool>(true);
        bool in_chat = false;
        bool rail = false;              // the focus is on the server column
        int rail_sel = 0;               // 0 = Direct Messages, i = servers[i - 1]
        int list_sel = 0;
        std::vector<DiscordService::Server> servers;
        std::vector<Row> rows;
        uint64_t built = ~0ull;
        Snowflake built_guild = ~0ull, chat = 0;
        u64 next_build = 0;
        std::string chat_name;
        std::vector<ChatRow> chat_rows;
        std::vector<std::string> chat_emoji;   // the pictures Piece::emoji points at
        uint64_t chat_built = ~0ull;
        u64 chat_next_build = 0;
        std::string attach;             // a picked file waiting for A
        // The notification banner.
        std::string ban_title, ban_text, ban_avatar;
        Snowflake ban_chat = 0;
        std::string ban_name;           // the chat, as the panel titles it
        u64 ban_until = 0;
        int ban_x = 0, ban_y = 0, ban_w = 0, ban_h = 0;   // where it was last drawn
        int chat_up = 0;                // rows scrolled up from the newest
        float chat_pos = 0, list_pos = 0, rail_pos = 0;   // the same, eased
        bool reopen = false;            // a fresh session: open the chat again
        std::map<std::string, Pic> pictures;
        std::string attach_text;        // the message going with the picked file
        int vis_media = -1;             // the lowest picture on screen (Y opens it)
        // The picture viewer.
        bool view = false;
        int view_row = -1, view_w = 0, view_h = 0;
        std::string view_url, view_still;
        float zoom = 1, pan_x = 0, pan_y = 0;
        // Touch: last frame's targets, and the finger.
        struct Hit { SDL_Rect r; int kind, idx; };
        std::vector<Hit> hits;
        bool t_moved = false, t_pinch = false;
        int t_x0 = 0, t_y0 = 0, t_list0 = 0, t_rail0 = 0;
        float t_pos0 = 0, t_zoom0 = 1, t_dist0 = 1, t_panx0 = 0, t_pany0 = 0;
        u64 t_last_tap = 0;
        // Names with their emoji already found (text marked, pictures listed).
        struct Rich { std::string text; std::vector<std::string> urls; };
        std::unordered_map<std::string, Rich> rich;
        // Emoji: the picker, and the message it builds.
        bool picker = false;
        int pick_sel = 0;
        std::string draft;
        std::string qr_text;
        std::vector<bool> qr;
        int qr_size = 0;

        Snowflake Guild() const { return rail_sel > 0 && rail_sel <= (int)servers.size() ? servers[rail_sel - 1].id : 0; }
    };

    namespace {
        void DropPicture(DiscordUi &d, gfx::Gfx *g, const std::string &url) {
            auto it = d.pictures.find(url);
            if (it == d.pictures.end()) return;
            if (it->second.tex) g->FreeImage(it->second.tex);
            d.pictures.erase(it);   // a load still in flight finds nothing and frees its result
        }
        void ViewClose(DiscordUi &d, gfx::Gfx *g) {
            if (d.view && d.view_url != d.view_still) DropPicture(d, g, d.view_url);
            d.view = false;
            d.view_row = -1;
        }
        void ViewOpen(DiscordUi &d, gfx::Gfx *g, int row) {
            if (row < 0 || row >= (int)d.chat_rows.size() || d.chat_rows[row].kind != DiscordUi::ChatRow::Media) return;
            ViewClose(d, g);
            const DiscordUi::ChatRow &r = d.chat_rows[row];
            d.view = true;
            d.view_row = row;
            d.view_still = r.url;
            d.view_url = r.full.empty() ? r.url : r.full;
            d.view_w = r.ow > 0 ? r.ow : r.w;
            d.view_h = r.oh > 0 ? r.oh : r.ph;
            d.zoom = 1;
            d.pan_x = d.pan_y = 0;
        }
        // The next (dir 1) or previous (-1) picture in the chat.
        void ViewStep(DiscordUi &d, gfx::Gfx *g, int dir) {
            for (int i = d.view_row + dir; i >= 0 && i < (int)d.chat_rows.size(); i += dir)
                if (d.chat_rows[i].kind == DiscordUi::ChatRow::Media) { ViewOpen(d, g, i); return; }
        }
        void SetZoom(DiscordUi &d, float z) {
            const float nz = std::clamp(z, 1.0f, 6.0f);
            d.pan_x *= nz / d.zoom;   // keep the middle of the view where it is
            d.pan_y *= nz / d.zoom;
            d.zoom = nz;
        }
    }

    void Menu::OpenDiscord() {
        if (!m_dc) m_dc = new DiscordUi;
        m_dc->svc.activate();
        m_dc->built = ~0ull;
        m_screen = Screen::Discord;
    }

    void Menu::PollDiscord() {
        // Notifications and Rich Presence: connect in the background once the menu has settled.
        if (!m_dc && (m_discord_alerts || m_discord_rpc) && m_deferred_done && !m_dc_tried) {
            m_dc_tried = true;
            struct stat st {};
            if (stat("sdmc:/slaunch/config/discord.json", &st) == 0) {
                m_dc = new DiscordUi;
                m_dc->svc.activate();
            }
        }
        if (!m_dc) return;
        m_dc->svc.update();
        // Rich Presence follows the game in the background, and goes when it closes.
        // Not during a hand-off: the menu draws a few more frames after a launch,
        // before m_suspended knows the game, and this would clear what was just set.
        if (m_dc->svc.state() == DiscordService::State::Ready && !m_dc->svc.pausedForGame())
            DiscordSetGame(m_discord_rpc ? m_suspended : 0);
        DiscordService::Alert a;
        while (m_dc->svc.takeAlert(a)) {
            if (!m_discord_alerts) continue;
            // Already reading that chat: no banner.
            if (m_screen == Screen::Discord && m_dc->in_chat && m_dc->chat == a.channelId) continue;
            m_dc->ban_title = a.title;   // DiscordText draws their emoji
            m_dc->ban_text = a.text;
            m_dc->ban_avatar = a.avatarUrl;
            m_dc->ban_chat = a.channelId;
            m_dc->ban_name = a.chat;
            m_dc->ban_until = armGetSystemTick() + armGetSystemTickFreq() * 5;
            PlayNotifySound();
        }
        // A sound clip the trusted sender posted, or one the user tapped to play.
        // A video only plays if the chat it came from is still on screen.
        std::string clip;
        if (m_dc->svc.takeSound(clip)) {
            if (!IsVideoPath(clip.c_str())) m_sfx.PlayClip(clip.c_str());
            else if (m_screen == Screen::Discord && !PlayAlbumVideo(clip)) SetStatus(T("Could not play that clip"));
        }
        // However the Discord screen was left, its video does not play on.
        if (m_album_video_active && m_screen != Screen::Discord && m_screen != Screen::Album) StopAlbumVideo();
    }

    void Menu::DiscordPlay(int row) {
        const DiscordUi::ChatRow &r = m_dc->chat_rows[row];
        const bool video = r.kind == DiscordUi::ChatRow::Video;
        if (!m_dc->svc.requestSound(r.url, video)) { SetStatus(T("Still loading the last video...")); return; }
        SetStatus(video ? T("Loading video...") : T("Playing sound..."));
    }

    // A chat's video, full screen and fitted (a phone's upright clip is not
    // cropped), with the album viewer's transport below it.
    void Menu::DrawDiscordVideo() {
        const Theme &t = m_theme.Current();
        const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
        m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, 255 });
        m_album_video.Tick();
        if (SDL_Texture *vt = m_album_video.GetTexture()) {
            float tw = 1, th = 1;
            SDL_GetTextureSize(vt, &tw, &th);
            const float s = std::min(W / std::max(1.0f, tw), H / std::max(1.0f, th));
            const int w = (int)(tw * s), h = (int)(th * s);
            m_gfx->DrawImage(vt, (W - w) / 2, (H - h) / 2, w, h, 255);
        } else {
            m_gfx->TextCentered(FontSize::Normal, W / 2, H / 2 - 20, t.dim, T("Loading..."));
        }
        const double dur = m_album_video.Duration(), p = m_album_video.Position();
        const bool held = m_album_video.Paused() || m_album_video.Ended();
        const int bx = 40, bw = W - 80, by = H - 96;
        m_gfx->FillRect(0, by - 16, W, H - by + 16, SDL_Color{ 0, 0, 0, 140 });
        m_gfx->FillRect(bx, by, bw, 6, SDL_Color{ 255, 255, 255, 60 });
        if (dur > 0.0) m_gfx->FillRect(bx, by, (int)(bw * std::min(1.0, p / dur)), 6, t.accent);
        m_gfx->Text(FontSize::Small, bx, by + 12, t.fg, (FormatTime(p) + " / " + FormatTime(dur)).c_str());
        DrawStatusHint({ {{"a"}, held ? "Play" : "Pause"}, {{"left", "right"}, "Seek"}, {{"b"}, "Close"} });
    }

    // A notification in the style of the layout it shows over: Metro gets the
    // Xbox 360 achievement toast, XMB the PS3's pop-up, the rest a card in the
    // theme's own colours. kBanSecs long, timed from ban_until.
    void Menu::DrawDiscordBanner() {
        if (!m_dc || !m_dc->ban_until) return;
        DiscordUi &d = *m_dc;
        const u64 now = armGetSystemTick(), freq = armGetSystemTickFreq();
        if (now >= d.ban_until) { d.ban_until = 0; return; }
        constexpr float kBanSecs = 5.0f;
        const float left = (float)(d.ban_until - now) / (float)freq;
        const float e = std::max(0.0f, kBanSecs - left);              // since it appeared
        auto ease = [](float v) { v = std::clamp(v, 0.0f, 1.0f); return 1.0f - (1.0f - v) * (1.0f - v) * (1.0f - v); };
        const int W = gfx::Gfx::Width, H = gfx::Gfx::Height;
        const Theme &t = m_theme.Current();
        auto card = [&](SDL_Texture *tex, float x, float y, float w, float h, float r, float sh,
                        SDL_Color fill, Uint8 a) {
            gfx::Gfx::CardStyle cs;
            cs.radius = r; cs.shadow = sh; cs.fill = fill;
            if (!m_gfx->Card(tex, x, y, w, h, cs, a)) {
                if (fill.a) m_gfx->FillRect((int)x, (int)y, (int)w, (int)h, WithAlpha(fill, (Uint8)(fill.a * a / 255)));
                if (tex) m_gfx->DrawImage(tex, (int)x, (int)y, (int)w, (int)h, a);
            }
        };
        // The sender's picture, round when the card shader can do it.
        auto avatar = [&](float x, float y, float sz, Uint8 a) {
            SDL_Texture *av = DiscordPicture(d.ban_avatar, (int)sz, (int)sz);
            if (av) card(av, x, y, sz, sz, sz / 2, 0.0f, SDL_Color{ 0, 0, 0, 0 }, a);
            return av != nullptr;
        };

        if (m_ui_mode == UiMode::Metro || XboxOld()) {
            // The achievement: a disc pops up, the dark pill unrolls from it,
            // then the words; all of it backwards on the way out.
            const float in = std::min(e, left);                         // symmetric out
            const float pop = ease(in / 0.25f), grow = ease((in - 0.25f) / 0.35f);
            const float txt = std::clamp((in - 0.55f) / 0.2f, 0.0f, 1.0f);
            const float h = 84.0f, full = 600.0f;
            const float pw = h + (full - h) * grow;
            const float px = (W - pw) / 2.0f, py = H - 150.0f;
            d.ban_x = (int)px; d.ban_y = (int)py; d.ban_w = (int)pw; d.ban_h = (int)h;
            if (grow > 0.0f)
                card(nullptr, px, py, pw, h, h / 2, 16.0f, SDL_Color{ 38, 38, 40, 240 }, 255);
            const float ds = h * (0.6f + 0.4f * pop), dx = px + (h - ds) / 2, dy = py + (h - ds) / 2;
            if (pop > 0.0f) {
                card(nullptr, dx, dy, ds, ds, ds / 2, grow > 0.0f ? 0.0f : 12.0f,
                     WithAlpha(XboxOld() ? SDL_Color{ 104, 176, 38, 255 } : m_mt_accent, 255), (Uint8)(255 * pop));
                const float in_s = ds - 12.0f;
                if (!avatar(dx + 6, dy + 6, in_s, (Uint8)(255 * pop)))
                    if (SDL_Texture *ic = SystemIcon(ItemKind::Discord))
                        m_gfx->DrawImageTinted(ic, (int)(dx + ds * 0.25f), (int)(dy + ds * 0.25f), (int)(ds * 0.5f),
                                               (int)(ds * 0.5f), SDL_Color{ 255, 255, 255, 255 }, (Uint8)(255 * pop));
            }
            if (txt > 0.0f) {
                const Uint8 ta = (Uint8)(255 * txt);
                const int tx = (int)(px + h + 14), tw = (int)(pw - h - 40);
                DiscordText(FontSize::Small, tx, (int)py + 12, SDL_Color{ 255, 255, 255, ta }, d.ban_title, tw);
                DiscordText(FontSize::Small, tx, (int)py + 46, SDL_Color{ 190, 190, 190, ta }, d.ban_text, tw);
            }
            return;
        }

        const Uint8 a = (Uint8)(255 * std::min(1.0f, left / 0.4f));
        if (m_ui_mode == UiMode::XMB) {
            // The PS3's: a small smoked box at the top right that slides in.
            const int bw = 420, bh = 70;
            const float x = W - bw - 30 + (1.0f - ease(e / 0.3f)) * (bw + 40), y = 40;
            d.ban_x = (int)x; d.ban_y = (int)y; d.ban_w = bw; d.ban_h = bh;
            card(nullptr, x, y, bw, bh, 6.0f, 10.0f, SDL_Color{ 20, 20, 24, 215 }, a);
            m_gfx->FillRect((int)x + 6, (int)y, bw - 12, 1, SDL_Color{ 255, 255, 255, (Uint8)(60 * a / 255) });
            int tx = (int)x + 16;
            if (DiscordDraw(d.ban_avatar, (int)x + 12, (int)y + 11, 48, 48, a)) tx = (int)x + 72;
            const int tw = (int)x + bw - tx - 14;
            DiscordText(FontSize::Small, tx, (int)y + 8, WithAlpha(SDL_Color{ 255, 255, 255, 255 }, a), d.ban_title, tw);
            DiscordText(FontSize::Small, tx, (int)y + 38, WithAlpha(SDL_Color{ 180, 180, 180, 255 }, a), d.ban_text, tw);
            return;
        }

        // Every other layout: a card in the theme's colours, dropping in from the top.
        const int bw = 480, bh = 82;
        const float x = W - bw - 24, y = 20 - (1.0f - ease(e / 0.3f)) * (bh + 40);
        d.ban_x = (int)x; d.ban_y = (int)y; d.ban_w = bw; d.ban_h = bh;
        const SDL_Color fill{ (Uint8)((t.bg_top.r + t.bg_bottom.r) / 2), (Uint8)((t.bg_top.g + t.bg_bottom.g) / 2),
                             (Uint8)((t.bg_top.b + t.bg_bottom.b) / 2), 245 };
        card(nullptr, x, y, bw, bh, 14.0f, 18.0f, fill, a);
        card(nullptr, x + 8, y + 14, 5, bh - 28, 2.5f, 0.0f, t.accent, a);
        int tx = (int)x + 26;
        if (avatar(x + 24, y + 16, 50, a)) tx = (int)x + 88;
        const int tw = (int)x + bw - tx - 18;
        DiscordText(FontSize::Small, tx, (int)y + 12, WithAlpha(t.title, a), d.ban_title, tw);
        DiscordText(FontSize::Small, tx, (int)y + 44, WithAlpha(t.dim, a), d.ban_text, tw);
    }

    bool Menu::DiscordBannerTouch(int x, int y) {
        if (!m_dc || !m_dc->ban_until || !m_dc->ban_chat) return false;
        DiscordUi &d = *m_dc;
        if (x < d.ban_x || x >= d.ban_x + d.ban_w || y < d.ban_y || y >= d.ban_y + d.ban_h) return false;
        const Snowflake chat = d.ban_chat;
        d.ban_until = 0;
        m_options_open = false;
        OpenDiscord();
        d.view = false;
        d.rail = false;
        d.chat = chat;
        d.chat_name = d.ban_name;
        d.chat_up = 0;
        d.chat_pos = 0;
        d.chat_built = ~0ull;
        d.in_chat = true;
        d.svc.openChannel(chat);
        return true;
    }

    void Menu::DiscordAttach(const std::string &path) {
        m_screen = Screen::Discord;
        if (!m_dc || !m_dc->chat) return;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0) { SetStatus("Could not read that file"); return; }
        if ((long long)st.st_size > kDcMaxUpload) { SetStatus("Too big for Discord (10 MB at most)"); return; }
        m_dc->attach = path;
    }

    void Menu::DiscordSetGame(u64 app_id) {
        if (!m_dc || app_id == m_rpc_id) return;
        m_rpc_id = app_id;
        std::string name;
        // Every installed title: m_items lacks games in folders, or outside an
        // open folder or search. A rename on the menu wins.
        for (const AppEntry &a : m_apps)
            if (a.app_id == app_id) { name = a.name; break; }
        for (const MenuItem &it : m_items)
            if (it.kind == ItemKind::Game && it.app_id == app_id) { name = it.name; break; }
        for (const MenuItem &it : m_tucked)
            if (it.kind == ItemKind::Game && it.app_id == app_id) { name = it.name; break; }
        // The picture: one chosen in "Choose icon", else the eShop's.
        m_dc->svc.setGame(name, (int64_t)time(nullptr) * 1000, [app_id, name]() -> std::string {
            if (!app_id) return "";
            char p[64];
            snprintf(p, sizeof(p), "sdmc:/slaunch/icons_custom/%016llX.url", (unsigned long long)app_id);
            if (FILE *f = fopen(p, "r")) {
                char u[1024] = {};
                fgets(u, sizeof(u), f);
                fclose(f);
                u[strcspn(u, "\r\n")] = 0;
                if (!strncmp(u, "https://", 8)) return u;
            }
            return name.empty() ? std::string() : sl::menu::net::EshopIcon(name, 512);
        });
    }

    void Menu::DiscordSuspend(u64 game) {
        if (!m_dc) return;
        const u64 g = game ? game : m_suspended;
        // Still connecting is fine: the presence goes out with the identify.
        const auto st = m_dc->svc.state();
        const bool live = st == DiscordService::State::Ready || st == DiscordService::State::Connecting;
        // The gateway alone keeps going, for "Playing" and for pop-ups over the game.
        const bool toasts = m_discord_alerts && m_discord_game_alerts;
        if (g && live && (m_discord_rpc || toasts)) {
            if (m_discord_rpc) DiscordSetGame(g);
            m_dc->svc.pauseForGame();
#ifdef SL_QEXT
            if (toasts) GameToastStart();
#endif
            return;
        }
        m_dc->svc.beginShutdown();
    }

#ifdef SL_QEXT
    // Pop-ups over the game, in the style of the layout's banner (DrawDiscordBanner).
    // The callbacks run on the pop-up thread while the menu is stopped, and
    // toast::Stop() joins it before the menu runs again.
    void Menu::GameToastStart() {
        qext::toast::Look look;
        if (m_ui_mode == UiMode::Metro || XboxOld()) {
            look.style = qext::toast::Look::Achievement;
            look.disc = XboxOld() ? SDL_Color{ 104, 176, 38, 255 } : WithAlpha(m_mt_accent, 255);
        } else if (m_ui_mode == UiMode::XMB) {
            look.style = qext::toast::Look::Xmb;
        } else {
            const Theme &t = m_theme.Current();
            look.fill = SDL_Color{ (Uint8)((t.bg_top.r + t.bg_bottom.r) / 2), (Uint8)((t.bg_top.g + t.bg_bottom.g) / 2),
                                   (Uint8)((t.bg_top.b + t.bg_bottom.b) / 2), 245 };
            look.accent = t.accent;
            look.title = t.title;
            look.dim = t.dim;
        }
        DiscordUi *dc = m_dc;
        qext::toast::Start(look,
            [dc](qext::toast::Item &it) {
                DiscordService::Alert a;
                if (!dc->svc.takeGameAlert(a)) return false;
                it.title = a.title;
                it.text = a.text;
                it.avatarUrl = a.avatarUrl;
                return true;
            },
            [this] { PlayNotifySound(); });
    }
#endif

    void Menu::DiscordResume() {
#ifdef SL_QEXT
        qext::toast::Stop();   // before the service it reads from changes
#endif
        if (!m_dc) return;
        if (m_dc->svc.resumeFromGame()) {   // missed messages: a fresh session
            m_dc->reopen = m_dc->in_chat;
            m_dc->built = m_dc->chat_built = ~0ull;
        } else if (m_dc->svc.stopping()) {
            m_dc->svc.activate();           // stopped for an applet: on again
        }
    }

    void Menu::DiscordExit() {
#ifdef SL_QEXT
        qext::toast::Stop();
#endif
        if (!m_dc) return;
        *m_dc->alive = false;
        m_dc->svc.shutdown();     // also runs before the sockets go (see UnloadForApplet)
        for (auto &kv : m_dc->pictures) if (kv.second.tex) m_gfx->FreeImage(kv.second.tex);
        delete m_dc;
        m_dc = nullptr;
    }

    void Menu::DiscordKeyboardDone(bool ok) {
        m_screen = Screen::Discord;
        if (!m_dc || !ok) return;
        if (m_kb_purpose == sl::smi::Kb_DiscordMessage && !m_dc->attach.empty()) {
            m_dc->attach_text = m_kb_text;   // shown in the preview until A sends both
            return;
        }
        if (m_kb_text.empty()) return;
        if (m_kb_purpose == sl::smi::Kb_DiscordToken) {
            m_dc->svc.signInWithUserToken(m_kb_text);
        } else if (m_dc->chat) {
            m_dc->svc.send(m_dc->chat, m_kb_text);   // the draft came in as the keyboard's start
            m_dc->draft.clear();
            m_dc->chat_up = 0;
        }
    }

    // Downloads a picture the first time it is asked for, at w x h; alt is tried
    // when url fails (a still for an animation too big to fetch).
    SDL_Texture *Menu::DiscordPicture(const std::string &url, int w, int h, const std::string &alt) {
        if (url.empty()) return nullptr;
        DiscordUi &d = *m_dc;
        auto it = d.pictures.find(url);
        if (it != d.pictures.end()) return it->second.tex;
        // ponytail: drops all of them at the cap, they come back as they are drawn.
        if (d.pictures.size() >= kDcMaxPictures) {
            for (auto &kv : d.pictures) if (kv.second.tex) m_gfx->FreeImage(kv.second.tex);
            d.pictures.clear();
        }
        d.pictures[url] = Pic{};
        std::weak_ptr<bool> alive = d.alive;
        w = std::max(1, w);
        h = std::max(1, h);
        Async::Run(Async::Pool::Images, [this, url, alt, alive, w, h] {
            Decoded dec;
            // A file on the card (the picker's preview): its own shape, inside w x h.
            if (url.compare(0, 6, "sdmc:/") == 0) {
                const std::string body = ReadFile(url, (size_t)kDcMaxUpload);
                if (!body.empty()) dec = Decode(body, w, h, true);
            } else for (const std::string &get : { url, alt }) {
                if (get.empty()) continue;
                Http::Request rq;
                rq.url = get;
                rq.maxBytes = 8u << 20;
                Http::Response rs = Http::Perform(rq);
                // A cut-off body never reaches the decoders (see the libjpeg note in Gfx).
                if (!rs.Ok() || !rs.error.empty() || rs.body.empty()) continue;
                dec = Decode(rs.body, w, h);
                if (dec.frames > 1) LOG("picture: %d frames, %zu bytes, %.80s", dec.frames, rs.body.size(), get.c_str());
                if (dec.sheet) break;
                LOG("picture: no decode (%zu bytes) %.80s", rs.body.size(), get.c_str());
            }
            if (!dec.sheet) {   // say so, so text can stand in for it
                Async::Post([this, url, alive] {
                    auto a = alive.lock();
                    if (!a || !*a) return;
                    auto p = m_dc->pictures.find(url);
                    if (p != m_dc->pictures.end() && !p->second.tex) p->second.failed = true;
                });
                return;
            }
            Async::Post([this, url, alive, dec]() mutable {
                auto a = alive.lock();
                if (!a || !*a) { SDL_DestroySurface(dec.sheet); return; }
                auto p = m_dc->pictures.find(url);
                if (p == m_dc->pictures.end() || p->second.tex) { SDL_DestroySurface(dec.sheet); return; }
                p->second.tex = SDL_CreateTextureFromSurface(m_gfx->Renderer(), dec.sheet);
                SDL_DestroySurface(dec.sheet);
                if (p->second.tex) SDL_SetTextureScaleMode(p->second.tex, SDL_SCALEMODE_LINEAR);
                p->second.frames = dec.frames;
                p->second.cols = dec.cols;
                p->second.fw = dec.fw;
                p->second.fh = dec.fh;
                p->second.ends = std::move(dec.ends);
            });
        });
        return nullptr;
    }

    int Menu::DiscordText(FontSize fs, int x, int y, SDL_Color c, const std::string &raw, int maxw) {
        if (raw.empty()) return 0;
        DiscordUi &d = *m_dc;
        auto it = d.rich.find(raw);
        if (it == d.rich.end()) {
            if (d.rich.size() > 2000) d.rich.clear();   // ponytail: all at once, they come back as drawn
            DiscordUi::Rich r;
            r.text = DiscordService::emojiMarks(Plain(raw), r.urls);
            it = d.rich.emplace(raw, std::move(r)).first;
        }
        const DiscordUi::Rich &r = it->second;
        if (r.urls.empty()) {   // nothing to draw but text
            const std::string s = maxw > 0 ? Ellipsize(r.text, maxw, fs) : r.text;
            m_gfx->Text(fs, x, y, c, s.c_str());
            return m_gfx->TextWidth(fs, s.c_str());
        }
        // Text runs between emoji, measured, then drawn up to maxw.
        struct Seg { std::string text; int emoji, w; };
        std::vector<Seg> segs;
        const int lh = m_gfx->LineHeight(fs), es = std::max(12, lh - 6);
        std::string run;
        auto end_run = [&] {
            if (!run.empty()) segs.push_back({ run, -1, m_gfx->TextWidth(fs, run.c_str()) });
            run.clear();
        };
        for (size_t i = 0; i < r.text.size(); i++) {
            const int e = EmojiAt(r.text, i);
            if (e >= 0) {
                i += 3;
                if (e >= (int)r.urls.size()) continue;
                const auto p = d.pictures.find(r.urls[e]);
                if (p != d.pictures.end() && p->second.failed) { run += EmojiChars(r.urls[e]); continue; }
                end_run();
                segs.push_back({ "", e, es + 2 });
            }
            else run += r.text[i];
        }
        end_run();
        int total = 0;
        for (const Seg &g : segs) total += g.w;
        const bool cut = maxw > 0 && total > maxw;
        const int limit = cut ? maxw - m_gfx->TextWidth(fs, "...") : total;
        int cx = 0;
        for (const Seg &g : segs) {
            if (cx + g.w > limit) {
                if (g.emoji < 0) {   // the part of the run that fits
                    std::string part;
                    for (size_t i = 0; i < g.text.size();) {
                        size_t n = i + 1;
                        while (n < g.text.size() && ((unsigned char)g.text[n] & 0xC0) == 0x80) n++;
                        if (cx + m_gfx->TextWidth(fs, (part + g.text.substr(i, n - i)).c_str()) > limit) break;
                        part += g.text.substr(i, n - i);
                        i = n;
                    }
                    if (!part.empty()) { m_gfx->Text(fs, x + cx, y, c, part.c_str()); cx += m_gfx->TextWidth(fs, part.c_str()); }
                }
                break;
            }
            if (g.emoji < 0) m_gfx->Text(fs, x + cx, y, c, g.text.c_str());
            else {
                DiscordPicture(r.urls[g.emoji], 48, 48);   // one crisp copy for every size
                DiscordDraw(r.urls[g.emoji], x + cx + 1, y + (lh - es) / 2, es, es, c.a);
            }
            cx += g.w;
        }
        if (cut) { m_gfx->Text(fs, x + cx, y, c, "..."); cx += m_gfx->TextWidth(fs, "..."); }
        return cx;
    }

    bool Menu::DiscordDraw(const std::string &url, int x, int y, int w, int h, Uint8 a, const std::string &alt) {
        SDL_Texture *tex = DiscordPicture(url, w, h, alt);
        if (!tex) return false;
        const Pic &p = m_dc->pictures[url];
        if (p.frames <= 1 || p.ends.empty() || p.ends.back() <= 0) {
            m_gfx->DrawImage(tex, x, y, w, h, a);
            return true;
        }
        const u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ull;
        const int t = (int)(ms % (u64)p.ends.back());
        const int f = std::min((int)(std::upper_bound(p.ends.begin(), p.ends.end(), t) - p.ends.begin()), p.frames - 1);
        const SDL_FRect src{ (float)((f % p.cols) * p.fw), (float)((f / p.cols) * p.fh), (float)p.fw, (float)p.fh };
        const SDL_FRect dst{ (float)x, (float)y, (float)w, (float)h };
        m_gfx->FxClose();
        SDL_SetTextureAlphaMod(tex, a);
        SDL_RenderTexture(m_gfx->Renderer(), tex, &src, &dst);
        SDL_SetTextureAlphaMod(tex, 255);
        return true;
    }

    Menu::Action Menu::OnButtonDiscord(Btn b) {
        DiscordUi &d = *m_dc;
        DiscordService &s = d.svc;
        auto keyboard = [&](u32 purpose) {
            m_kb_purpose = purpose;
            m_kb_text.clear();
            m_kb_row = m_kb_col = 0;
            m_kb_upper = false;
            m_screen = Screen::Keyboard;
        };
        auto leave = [&] { m_screen = Screen::Main; };

        if (m_album_video_active) {                      // a chat's video, full screen
            if (b == Btn::B) StopAlbumVideo();
            if (b == Btn::Left)  m_album_video.Seek(m_album_video.Position() - 5.0);
            if (b == Btn::Right) m_album_video.Seek(m_album_video.Position() + 5.0);
            if (b == Btn::A) {
                if (m_album_video.Ended()) { m_album_video.Seek(0.0); m_album_video.SetPaused(false); }
                else m_album_video.SetPaused(!m_album_video.Paused());
            }
            return Action::None;
        }

        switch (s.state()) {
            case DiscordService::State::SignedOut:
                if (b == Btn::A) s.startQrLogin();
                if (b == Btn::Y) keyboard(sl::smi::Kb_DiscordToken);
                if (b == Btn::B) leave();
                return Action::None;
            case DiscordService::State::QrLogin:
                if (b == Btn::A && !s.error().empty()) s.startQrLogin();   // it failed: again
                if (b == Btn::Y && !s.error().empty()) keyboard(sl::smi::Kb_DiscordToken);
                if (b == Btn::B) s.cancelQrLogin();
                return Action::None;
            case DiscordService::State::Failed:
                if (b == Btn::A) s.retry();
                if (b == Btn::B) leave();
                return Action::None;
            case DiscordService::State::Ready:
                break;
            default:
                if (b == Btn::B) leave();
                return Action::None;
        }

        if (d.in_chat) {
            if (!d.attach.empty()) {                     // the preview: send it, or not
                if (b == Btn::A) { s.sendFile(d.chat, d.attach, d.attach_text); d.chat_up = 0; }
                if (b == Btn::Y) { keyboard(sl::smi::Kb_DiscordMessage); return Action::None; }
                if (b == Btn::A || b == Btn::B) {
                    DropPicture(d, m_gfx, d.attach);
                    d.attach.clear();
                    d.attach_text.clear();
                }
                return Action::None;
            }
            if (d.view) {                                // the picture viewer
                const bool zoomed = d.zoom > 1.01f;
                if (b == Btn::B) ViewClose(d, m_gfx);
                if (b == Btn::A) SetZoom(d, zoomed ? 1.0f : 2.5f);
                if (b == Btn::R) SetZoom(d, d.zoom * 1.5f);
                if (b == Btn::L) SetZoom(d, d.zoom / 1.5f);
                if (zoomed) {
                    if (b == Btn::Left)  d.pan_x += 120;
                    if (b == Btn::Right) d.pan_x -= 120;
                    if (b == Btn::Up)    d.pan_y += 120;
                    if (b == Btn::Down)  d.pan_y -= 120;
                } else {
                    if (b == Btn::Left)  ViewStep(d, m_gfx, -1);
                    if (b == Btn::Right) ViewStep(d, m_gfx, 1);
                }
                return Action::None;
            }
            const int rows = (int)d.chat_rows.size();
            const int step = b == Btn::L || b == Btn::R ? 12 : 3;
            if (b == Btn::Up || b == Btn::L)   d.chat_up = std::min(d.chat_up + step, std::max(0, rows - 1));
            if (b == Btn::Down || b == Btn::R) d.chat_up = std::max(d.chat_up - step, 0);
            if (d.picker) {                              // the emoji picker
                const int rows_n = (kEmoN + kEmoCols - 1) / kEmoCols;
                int r = d.pick_sel / kEmoCols, c = d.pick_sel % kEmoCols;
                if (b == Btn::Left)  c = (c + kEmoCols - 1) % kEmoCols;
                if (b == Btn::Right) c = (c + 1) % kEmoCols;
                if (b == Btn::Up)    r = (r + rows_n - 1) % rows_n;
                if (b == Btn::Down)  r = (r + 1) % rows_n;
                d.pick_sel = std::min(kEmoN - 1, r * kEmoCols + c);
                if (b == Btn::A && d.draft.size() < 1900) d.draft += kEmo[d.pick_sel].text;
                if (b == Btn::X && !d.draft.empty()) { s.send(d.chat, d.draft); d.draft.clear(); d.chat_up = 0; d.picker = false; }
                if (b == Btn::Y) { d.picker = false; keyboard(sl::smi::Kb_DiscordMessage); m_kb_text = d.draft; }
                if (b == Btn::B || b == Btn::Minus) d.picker = false;
                return Action::None;
            }
            if (b == Btn::A) { keyboard(sl::smi::Kb_DiscordMessage); m_kb_text = d.draft; }
            if (b == Btn::Minus) d.picker = true;
            if (b == Btn::Plus && !d.draft.empty()) { s.send(d.chat, d.draft); d.draft.clear(); d.chat_up = 0; }
            if (b == Btn::Y && d.vis_media >= 0) {
                if (d.chat_rows[d.vis_media].kind == DiscordUi::ChatRow::Video) DiscordPlay(d.vis_media);
                else ViewOpen(d, m_gfx, d.vis_media);
            }
            if (b == Btn::X) {
                m_fm_pick = true;
                m_fm_menu_open = false;
                FmGo(NewestAlbumDir(), std::string());
                m_screen = Screen::Files;
            }
            if (b == Btn::B) { d.in_chat = false; d.chat = 0; d.chat_rows.clear(); d.vis_media = -1; }
            return Action::None;
        }

        if (b == Btn::B) { leave(); return Action::None; }
        if (b == Btn::Left)  d.rail = true;
        if (b == Btn::Right) d.rail = false;
        if (b == Btn::Y) s.startQrLogin();      // another account
        if (b == Btn::X) { s.signOut(); d.built = ~0ull; return Action::None; }
        if (b == Btn::Minus && s.accounts().size() > 1) {
            s.switchAccount(s.accounts().back().id);   // the current one is listed first
            d.built = ~0ull;
            return Action::None;
        }
        if (d.rail) {
            const int n = (int)d.servers.size() + 1;
            if (b == Btn::Up)   d.rail_sel = (d.rail_sel + n - 1) % n;
            if (b == Btn::Down) d.rail_sel = (d.rail_sel + 1) % n;
            if (b == Btn::Up || b == Btn::Down) { d.list_sel = 0; d.list_pos = 0; d.next_build = 0; }
            if (b == Btn::A) d.rail = false;
            return Action::None;
        }
        // The list skips what cannot be opened: categories and voice channels.
        const int n = (int)d.rows.size();
        if (n == 0) return Action::None;
        const auto openable = [&](int i) { return !d.rows[i].category && !d.rows[i].voice; };
        if (b == Btn::Up || b == Btn::Down) {
            const int step = b == Btn::Up ? n - 1 : 1;
            int i = d.list_sel;
            for (int tries = 0; tries < n; tries++) {
                i = (i + step) % n;
                if (openable(i)) { d.list_sel = i; break; }
            }
        }
        if (b == Btn::A && d.list_sel < n && openable(d.list_sel)) {
            const DiscordUi::Row &r = d.rows[d.list_sel];
            d.chat = r.id;
            d.chat_name = r.text;
            d.chat_up = 0;
            d.chat_pos = 0;
            d.chat_built = ~0ull;
            d.in_chat = true;
            s.openChannel(r.id);
        }
        return Action::None;
    }

    void Menu::DiscordBuildLists() {
        DiscordUi &d = *m_dc;
        const u64 now = armGetSystemTick();
        const uint64_t v = d.svc.version();
        const Snowflake guild = d.Guild();
        if (v == d.built && guild == d.built_guild) return;
        if (guild == d.built_guild && now < d.next_build) return;   // a big server never stops changing
        d.next_build = now + armGetSystemTickFreq() / 2;
        d.built = v;
        d.built_guild = guild;
        d.servers = d.svc.servers();
        d.rail_sel = std::min(d.rail_sel, (int)d.servers.size());
        d.rows.clear();
        if (const Snowflake g = d.Guild()) {
            for (const auto &c : d.svc.channels(g)) {
                DiscordUi::Row r;
                r.id = c.id; r.text = Plain(c.name); r.category = c.category; r.voice = c.voice;
                r.unread = c.unread; r.mentions = c.mentions;
                d.rows.push_back(std::move(r));
            }
        } else {
            for (const auto &c : d.svc.dms(40)) {
                DiscordUi::Row r;
                r.id = c.channelId; r.text = Plain(c.name); r.avatar = c.avatarUrl;
                r.unread = c.unread; r.mentions = c.mentions;
                d.rows.push_back(std::move(r));
            }
        }
        d.list_sel = std::clamp(d.list_sel, 0, std::max(0, (int)d.rows.size() - 1));
        // Landing on a category would leave nothing selected that A can open.
        for (int i = d.list_sel; i < (int)d.rows.size(); i++)
            if (!d.rows[i].category && !d.rows[i].voice) { d.list_sel = i; break; }
    }

    void Menu::DiscordBuildChat() {
        DiscordUi &d = *m_dc;
        const uint64_t v = d.svc.version();
        const u64 now = armGetSystemTick();
        if (v == d.chat_built) return;
        // A busy server changes the store every few ms; lay out twice a second at most.
        if (!d.chat_rows.empty() && now < d.chat_next_build) return;
        d.chat_next_build = now + armGetSystemTickFreq() / 2;
        d.chat_built = v;
        d.chat_rows.clear();
        d.chat_emoji.clear();
        const Theme &t = m_theme.Current();
        const int width = gfx::Gfx::Width - kDcTextX - kDcChatX;
        const int space = std::max(4, m_gfx->TextWidth(FontSize::Small, "a a") - m_gfx->TextWidth(FontSize::Small, "aa"));
        Snowflake last_author = 0;
        int64_t last_ms = 0;
        for (const auto &l : d.svc.lines(d.chat, 600)) {
            // What it answers, over its author's name like Discord (a reply always starts a group).
            if (l.reply) {
                DiscordUi::ChatRow rp;
                rp.kind = DiscordUi::ChatRow::Reply;
                rp.h = 26;
                rp.text = "@" + l.replyAuthor;
                rp.time = l.replyText;
                d.chat_rows.push_back(std::move(rp));
            }
            // A new author, or the same one after a pause, starts a group.
            if (l.reply || l.authorId != last_author || l.ms - last_ms > 7 * 60 * 1000) {
                DiscordUi::ChatRow h;
                h.kind = DiscordUi::ChatRow::Head;
                h.h = 40;
                h.text = Plain(l.author);
                h.time = l.time;
                h.url = l.avatarUrl;
                h.color = l.color ? SDL_Color{ (Uint8)(l.color >> 16), (Uint8)(l.color >> 8), (Uint8)l.color, 255 } : t.fg;
                d.chat_rows.push_back(std::move(h));
                last_author = l.authorId;
            }
            last_ms = l.ms;
            const int ebase = (int)d.chat_emoji.size();
            d.chat_emoji.insert(d.chat_emoji.end(), l.emojis.begin(), l.emojis.end());
            // Only emoji (a few): Discord shows them big.
            bool only = !l.text.empty();
            int count = 0;
            for (size_t i = 0; i < l.text.size() && only; i++) {
                if (EmojiAt(l.text, i) >= 0) { count++; i += 3; }
                else if (l.text[i] != ' ') only = false;
            }
            const int es = only && count <= 27 ? kDcJumbo : kDcEmoji;
            const SDL_Color tone = l.failed ? SDL_Color{ 240, 90, 90, 255 } : l.pending ? t.dim : t.fg;

            DiscordUi::ChatRow cur;
            int cx = 0;
            bool gap = false;
            auto row = [&] {
                cur.kind = DiscordUi::ChatRow::Text;
                cur.color = tone;
                cur.esize = es;
                cur.h = std::max(kDcChatLineH, es + 4);
                d.chat_rows.push_back(std::move(cur));
                cur = DiscordUi::ChatRow{};
                cx = 0;
            };
            auto text = [&](const std::string &w, int ww) {
                if (cx > 0 && cx + ww > width) row();
                if (gap && cx > 0) {
                    if (!cur.pieces.empty() && cur.pieces.back().emoji < 0) cur.pieces.back().text += ' ';
                    else cur.pieces.push_back({ " ", -1, cx });
                    cx += space;
                }
                gap = false;
                if (!cur.pieces.empty() && cur.pieces.back().emoji < 0) cur.pieces.back().text += w;
                else cur.pieces.push_back({ w, -1, cx });
                cx += ww;
            };
            std::string word;
            auto flush = [&] {
                if (word.empty()) return;
                int ww = m_gfx->TextWidth(FontSize::Small, word.c_str());
                // A link or anything else longer than a line: cut it where it fills one.
                while (ww > width) {
                    size_t cut = 0, next = 0;
                    while (next < word.size()) {
                        size_t n2 = next + 1;
                        while (n2 < word.size() && ((unsigned char)word[n2] & 0xC0) == 0x80) n2++;
                        if (m_gfx->TextWidth(FontSize::Small, word.substr(0, n2).c_str()) > width - cx) break;
                        next = cut = n2;
                    }
                    if (cut == 0) { if (cx > 0) { row(); continue; } cut = 1; }
                    text(word.substr(0, cut), m_gfx->TextWidth(FontSize::Small, word.substr(0, cut).c_str()));
                    row();
                    word.erase(0, cut);
                    ww = m_gfx->TextWidth(FontSize::Small, word.c_str());
                }
                if (!word.empty()) text(word, ww);
                word.clear();
            };
            for (size_t i = 0; i < l.text.size(); i++) {
                const int e = EmojiAt(l.text, i);
                if (e >= 0) {
                    flush();
                    i += 3;
                    if (e >= (int)l.emojis.size()) continue;
                    if (cx > 0 && cx + es > width) row();
                    if (gap && cx > 0) cx += space;
                    gap = false;
                    cur.pieces.push_back({ "", ebase + e, cx });
                    cx += es + 2;
                } else if (l.text[i] == ' ') {
                    flush();
                    gap = true;
                } else {
                    word += l.text[i];
                }
            }
            flush();
            if (!cur.pieces.empty()) row();
            for (const auto &md : l.media) {
                DiscordUi::ChatRow m;
                m.kind = DiscordUi::ChatRow::Media;
                m.url = md.animated && !md.url.empty() ? md.url : md.fallback.empty() ? md.url : md.fallback;
                if (md.animated && m.url != md.fallback) m.alt = md.fallback;
                m.full = md.full;
                m.ow = md.w;
                m.oh = md.h;
                int w = md.w > 0 ? md.w : 240, h = md.h > 0 ? md.h : 160;
                const float sc = std::min(1.0f, std::min((float)kDcMediaW / w, (float)kDcMediaH / h));
                m.w = std::max(16, (int)(w * sc));
                m.ph = std::max(16, (int)(h * sc));
                m.h = m.ph + 10;
                if (!m.url.empty()) d.chat_rows.push_back(std::move(m));
            }
            for (const auto *list : { &l.audio, &l.video })
                for (const auto &au : *list) {
                    DiscordUi::ChatRow m;
                    m.kind = list == &l.video ? DiscordUi::ChatRow::Video : DiscordUi::ChatRow::Audio;
                    m.url = au.url;
                    m.text = au.name;
                    m.h = kDcChatLineH + 10;
                    d.chat_rows.push_back(std::move(m));
                }
        }
    }

    void Menu::DrawDiscordQr(const std::string &text, int x, int y, int size) {
        DiscordUi &d = *m_dc;
        if (text != d.qr_text) {
            d.qr_text = text;
            d.qr.clear();
            d.qr_size = 0;
            if (!text.empty()) {
                const auto code = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
                d.qr_size = code.getSize();
                d.qr.resize((size_t)d.qr_size * d.qr_size);
                for (int cy = 0; cy < d.qr_size; cy++)
                    for (int cx = 0; cx < d.qr_size; cx++)
                        d.qr[(size_t)cy * d.qr_size + cx] = code.getModule(cx, cy);
            }
        }
        m_gfx->FillRect(x, y, size, size, SDL_Color{ 255, 255, 255, 255 });
        if (d.qr_size <= 0) return;
        const int cell = size / (d.qr_size + 6);
        const int ox = x + (size - cell * d.qr_size) / 2, oy = y + (size - cell * d.qr_size) / 2;
        for (int cy = 0; cy < d.qr_size; cy++)
            for (int cx = 0; cx < d.qr_size; cx++)
                if (d.qr[(size_t)cy * d.qr_size + cx])
                    m_gfx->FillRect(ox + cx * cell, oy + cy * cell, cell, cell, SDL_Color{ 0, 0, 0, 255 });
    }

    void Menu::DrawDiscord() {
        DiscordUi &d = *m_dc;
        DiscordService &s = d.svc;
        const Theme &t = m_theme.Current();
        const int W = gfx::Gfx::Width;
        d.hits.clear();
        if (m_album_video_active) { DrawDiscordVideo(); return; }
        auto hit = [&](int x, int y, int w, int h, int kind, int idx = 0) { d.hits.push_back({ SDL_Rect{ x, y, w, h }, kind, idx }); };
        DrawTopBar("");   // the clock and battery; no title, the screen says what it is

        switch (s.state()) {
            case DiscordService::State::SignedOut: {
                m_gfx->TextCentered(FontSize::Normal, W / 2, 230, t.fg, T("Sign in to Discord"));
                // The risk is the reason this is worth saying before anyone signs in.
                int y = 290;
                for (const char *line : { T("sLaunch is an unofficial client. Discord may limit or close accounts"),
                                          T("that use third-party clients, so this is at your own risk."),
                                          T("Your sign-in is kept on the SD card: slaunch/config/discord.json") }) {
                    m_gfx->TextCentered(FontSize::Small, W / 2, y, t.dim, line);
                    y += 32;
                }
                // A refused token lands back here, and without this nothing says so.
                if (!s.error().empty())
                    m_gfx->TextCentered(FontSize::Small, W / 2, y + 20, SDL_Color{ 240, 90, 90, 255 },
                                        Ellipsize(s.error(), W - 120, FontSize::Small).c_str());
                DrawHint({ {{"a"}, "QR code"}, {{"y"}, "Token"}, {{"b"}, "Back"} });
                return;
            }
            case DiscordService::State::QrLogin: {
                DrawDiscordQr(s.qrUrl(), W / 2 - 170, 100, 340);
                const std::string status = s.qrStatus();
                int y = 460;
                for (const std::string &line : WrapText(FontSize::Small, status.empty() ? T("Scan this in the Discord app on your phone") : status.c_str(), W - 160)) {
                    m_gfx->TextCentered(FontSize::Small, W / 2, y, t.fg, line.c_str());
                    y += 30;
                }
                if (s.error().empty()) DrawHint({ {{"b"}, "Cancel"} });
                else                   DrawHint({ {{"a"}, "Retry"}, {{"y"}, "Token"}, {{"b"}, "Cancel"} });
                return;
            }
            case DiscordService::State::Failed:
                m_gfx->TextCentered(FontSize::Normal, W / 2, 300, t.fg, T("Could not connect to Discord"));
                m_gfx->TextCentered(FontSize::Small, W / 2, 350, t.dim,
                                    Ellipsize(s.error(), W - 120, FontSize::Small).c_str());
                DrawHint({ {{"a"}, "Retry"}, {{"b"}, "Back"} });
                return;
            case DiscordService::State::Ready:
                break;
            default:
                m_gfx->TextCentered(FontSize::Normal, W / 2, 330, t.fg, T("Connecting..."));
                DrawHint({ {{"b"}, "Back"} });
                return;
        }

        if (d.in_chat) {
            DiscordBuildChat();
            const int top = kDcTop + 34;
            DiscordText(FontSize::Normal, kDcChatX, kDcTop - 6, t.fg, (d.Guild() ? "# " : "") + d.chat_name, W - kDcChatX * 2);
            m_gfx->FillRect(kDcChatX, top - 6, W - kDcChatX * 2, 1, WithAlpha(t.fg, 40));
            if (d.svc.loading(d.chat) && d.chat_rows.empty())
                m_gfx->TextCentered(FontSize::Normal, W / 2, 330, t.dim, T("Loading..."));
            if (d.reopen) { d.reopen = false; s.openChannel(d.chat); }   // after a fresh session
            // Newest at the bottom, so it is drawn upward from above the input bar,
            // eased toward where the stick put it and cut at the chat's edges.
            const int n_rows = (int)d.chat_rows.size();
            d.chat_pos += ((float)d.chat_up - d.chat_pos) * 0.25f;
            if (std::fabs((float)d.chat_up - d.chat_pos) < 0.02f) d.chat_pos = (float)d.chat_up;
            const int k = std::max(0, (int)std::floor(d.chat_pos));
            int i = n_rows - 1 - k;
            int y = kDcInputY - 10;
            if (i >= 0 && i < n_rows) y += (int)((d.chat_pos - (float)k) * d.chat_rows[i].h);
            m_gfx->FxClose();
            const SDL_Rect clip{ 0, top, W, kDcInputY - 4 - top };
            SDL_SetRenderClipRect(m_gfx->Renderer(), &clip);
            d.vis_media = -1;
            for (; i >= 0; i--) {
                const DiscordUi::ChatRow &r = d.chat_rows[i];
                if (y <= top) break;
                y -= r.h;
                if (r.kind == DiscordUi::ChatRow::Reply) {
                    // A bent line from the avatar up to the quoted message.
                    m_gfx->FillRect(kDcChatX + 19, y + 14, 2, r.h - 14, WithAlpha(t.fg, 90));
                    m_gfx->FillRect(kDcChatX + 19, y + 14, kDcTextX - kDcChatX - 26, 2, WithAlpha(t.fg, 90));
                    const int nw = DiscordText(FontSize::Small, kDcTextX, y + 1, WithAlpha(t.fg, 200), r.text, 360);
                    DiscordText(FontSize::Small, kDcTextX + nw + 10, y + 1, t.dim, r.time, W - kDcTextX - nw - 10 - kDcChatX);
                } else if (r.kind == DiscordUi::ChatRow::Head) {
                    if (!DiscordDraw(r.url, kDcChatX, y + 10, 40, 40))
                        m_gfx->FillRect(kDcChatX, y + 10, 40, 40, WithAlpha(t.fg, 30));
                    const int aw = DiscordText(FontSize::Small, kDcTextX, y + 10, r.color, r.text, 520);
                    m_gfx->Text(FontSize::Small, kDcTextX + aw + 14,
                                y + 10, t.dim, r.time.c_str());
                } else if (r.kind == DiscordUi::ChatRow::Media) {
                    if (!DiscordDraw(r.url, kDcTextX, y + 4, r.w, r.ph, 255, r.alt))
                        m_gfx->FillRect(kDcTextX, y + 4, r.w, r.ph, WithAlpha(t.fg, 24));
                    hit(kDcTextX, std::max(y + 4, top), r.w, std::min(y + 4 + r.ph, kDcInputY - 4) - std::max(y + 4, top), HMedia, i);
                    if (d.vis_media < 0 && y + 4 >= top && y + 4 + r.ph <= kDcInputY - 4) d.vis_media = i;
                } else if (r.kind == DiscordUi::ChatRow::Audio || r.kind == DiscordUi::ChatRow::Video) {
                    // A pill with a play triangle and the file name; tap to play.
                    // Y plays the first video on screen, as it opens the first picture.
                    if (r.kind == DiscordUi::ChatRow::Video && d.vis_media < 0 && y + 6 >= top && y + r.h - 6 <= kDcInputY - 4)
                        d.vis_media = i;
                    const int pw = std::min(360, 70 + m_gfx->TextWidth(FontSize::Small, r.text.c_str()));
                    const int px = kDcTextX, py = y + 6, ph = r.h - 12;
                    gfx::Gfx::CardStyle cs; cs.radius = ph / 2.0f; cs.shadow = 0.0f;
                    cs.fill = WithAlpha(t.fg, 24);
                    if (!m_gfx->Card(nullptr, px, py, pw, ph, cs, 255))
                        m_gfx->FillRect(px, py, pw, ph, WithAlpha(t.fg, 24));
                    const int cxp = px + ph / 2, cyp = py + ph / 2, tr = ph / 5;
                    m_gfx->FillTriangle(cxp - tr / 2, cyp - tr, cxp - tr / 2, cyp + tr, cxp + tr, cyp, t.accent);
                    DiscordText(FontSize::Small, px + ph, py + (ph - m_gfx->LineHeight(FontSize::Small)) / 2,
                                t.fg, r.text, pw - ph - 12);
                    hit(px, std::max(py, top), pw, std::min(py + ph, kDcInputY - 4) - std::max(py, top), HAudio, i);
                } else {
                    const int ty = y + (r.h - m_gfx->LineHeight(FontSize::Small)) / 2;
                    for (const DiscordUi::Piece &p : r.pieces) {
                        if (p.emoji < 0) { m_gfx->Text(FontSize::Small, kDcTextX + p.x, ty, r.color, p.text.c_str()); continue; }
                        if (p.emoji >= (int)d.chat_emoji.size()) continue;
                        const std::string &u = d.chat_emoji[p.emoji];
                        const auto pic = d.pictures.find(u);
                        if (pic != d.pictures.end() && pic->second.failed)
                            m_gfx->Text(FontSize::Small, kDcTextX + p.x, ty, r.color, EmojiChars(u).c_str());
                        else
                            DiscordDraw(u, kDcTextX + p.x, y + (r.h - r.esize) / 2, r.esize, r.esize);
                    }
                }
            }
            m_gfx->FxClose();
            SDL_SetRenderClipRect(m_gfx->Renderer(), nullptr);
            // The oldest row is on screen: fetch the page before it.
            if (i < 0 && !d.chat_rows.empty() && !d.view) {
                if (d.svc.reachedStart(d.chat))
                    m_gfx->TextCentered(FontSize::Small, W / 2, top + 2, t.dim, T("This is the start of the conversation"));
                else {
                    d.svc.loadOlder(d.chat);
                    m_gfx->TextCentered(FontSize::Small, W / 2, top + 2, t.dim, T("Loading older messages..."));
                }
            }
            // The input bar: + for a file, the rest to write.
            m_gfx->FillRect(kDcChatX, kDcInputY, W - kDcChatX * 2, kDcInputH - 6, WithAlpha(t.fg, 26));
            {   // + drawn, not typed: a font's "+" sits wherever its baseline puts it
                const int pcx = kDcChatX + 20, pcy = kDcInputY + (kDcInputH - 6) / 2;
                m_gfx->FillRect(pcx - 14, pcy - 14, 28, 28, WithAlpha(t.fg, 60));
                m_gfx->FillRect(pcx - 8, pcy - 1, 16, 3, t.fg);
                m_gfx->FillRect(pcx - 1, pcy - 8, 3, 16, t.fg);
            }
            const int bar_r = W - kDcChatX;                  // the bar's right end
            const int ebx = bar_r - 40, eby = kDcInputY + (kDcInputH - 6 - 28) / 2;
            if (!DiscordDraw(EmoUrl(12), ebx, eby, 28, 28)) m_gfx->FillRect(ebx, eby, 28, 28, WithAlpha(t.fg, 60));
            int text_r = ebx - 10;
            if (!d.draft.empty()) {                          // a send button for the draft
                text_r -= 70;
                m_gfx->FillRect(text_r + 4, kDcInputY + 6, 62, kDcInputH - 18, t.accent);
                m_gfx->TextCentered(FontSize::Small, text_r + 35, kDcInputY + 9, t.bg_bottom, T("Send"));
                hit(text_r, kDcInputY, 70, kDcInputH - 6, HSendDraft);
            }
            hit(kDcChatX, kDcInputY, 44, kDcInputH - 6, HPlus);
            hit(ebx - 6, kDcInputY, 46, kDcInputH - 6, HEmojiBtn);
            hit(kDcChatX + 44, kDcInputY, text_r - kDcChatX - 44, kDcInputH - 6, HInput);
            const int iy = kDcInputY + (kDcInputH - 6 - m_gfx->LineHeight(FontSize::Small)) / 2;
            if (d.draft.empty()) {
                DiscordText(FontSize::Small, kDcChatX + 52, iy, t.dim,
                            std::string(T("Message")) + " " + (d.Guild() ? "#" : "") + d.chat_name, text_r - kDcChatX - 60);
            } else {
                // The draft, its emoji as pictures; the end shows when it runs long.
                int x = kDcChatX + 52;
                std::string run;
                auto flush_run = [&] {
                    if (run.empty()) return;
                    m_gfx->Text(FontSize::Small, x, iy, t.fg, run.c_str());
                    x += m_gfx->TextWidth(FontSize::Small, run.c_str());
                    run.clear();
                };
                size_t from = 0;
                if (d.draft.size() > 60) { from = d.draft.size() - 60; while (from < d.draft.size() && ((unsigned char)d.draft[from] & 0xC0) == 0x80) from++; }
                for (size_t i = from; i < d.draft.size() && x < text_r - 30;) {
                    const int k = EmoAt(d.draft, i);
                    if (k >= 0) {
                        flush_run();
                        DiscordDraw(EmoUrl(k), x, kDcInputY + (kDcInputH - 6 - 24) / 2, 24, 24);
                        x += 26;
                        i += strlen(kEmo[k].text);
                    } else {
                        run += d.draft[i++];
                    }
                }
                flush_run();
            }
            if (d.picker) {
                // The emoji picker, over the bottom of the chat.
                const int rows_n = (kEmoN + kEmoCols - 1) / kEmoCols;
                const int pw = kEmoCols * kEmoCell + 20, ph = rows_n * kEmoCell + 20;
                const int px = W - kDcChatX - pw, py = kDcInputY - 8 - ph;
                m_gfx->FillRect(px, py, pw, ph, WithAlpha(t.bg_bottom, 250));
                for (int k = 0; k < kEmoN; k++) {
                    const int cx = px + 10 + (k % kEmoCols) * kEmoCell, cy = py + 10 + (k / kEmoCols) * kEmoCell;
                    if (k == d.pick_sel) m_gfx->FillRect(cx, cy, kEmoCell, kEmoCell, WithAlpha(t.accent, 120));
                    DiscordDraw(EmoUrl(k), cx + 8, cy + 8, kEmoCell - 16, kEmoCell - 16);
                    hit(cx, cy, kEmoCell, kEmoCell, HEmoji, k);
                }
                DrawStatusHint({ {{"a"}, "Add"}, {{"x"}, "Send"}, {{"y"}, "Add text"}, {{"b"}, "Close"} });
                return;
            }

            if (!d.attach.empty()) {
                // The preview: what goes out, before it does.
                d.hits.clear();
                const int H = gfx::Gfx::Height;
                m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, 170 });
                const int cw = 680, ch = 500, cx = (W - cw) / 2, cy = 90;
                m_gfx->FillRect(cx, cy, cw, ch, WithAlpha(t.bg_bottom, 250));
                const int bw = cw - 40, bh = 300, bx = cx + 20, by = cy + 20;
                bool shown = false;
                if (IsPictureFile(d.attach) && DiscordPicture(d.attach, bw, bh)) {
                    const Pic &p = d.pictures[d.attach];
                    if (p.fw > 0 && p.fh > 0) {
                        DiscordDraw(d.attach, bx + (bw - p.fw) / 2, by + (bh - p.fh) / 2, p.fw, p.fh);
                        shown = true;
                    }
                }
                if (!shown) {
                    m_gfx->FillRect(bx, by, bw, bh, WithAlpha(t.fg, 18));
                    const std::string ext = d.attach.substr(d.attach.find_last_of('.') + 1);
                    m_gfx->TextCentered(FontSize::Normal, bx + bw / 2, by + bh / 2 - 16, t.dim,
                                        IsPictureFile(d.attach) ? T("Loading...") : ext.c_str());
                }
                struct stat st {};
                const long long size = stat(d.attach.c_str(), &st) == 0 ? (long long)st.st_size : 0;
                const std::string name = d.attach.substr(d.attach.find_last_of('/') + 1) + "   " + SizeText(size);
                m_gfx->Text(FontSize::Small, cx + 20, by + bh + 16, t.fg, Ellipsize(name, cw - 40, FontSize::Small).c_str());
                const std::string to = std::string(T("To")) + " " + (d.Guild() ? "#" : "") + d.chat_name;
                DiscordText(FontSize::Small, cx + 20, by + bh + 46, t.dim, to, cw - 40);
                m_gfx->Text(FontSize::Small, cx + 20, by + bh + 76, d.attach_text.empty() ? t.dim : t.fg,
                            Ellipsize(d.attach_text.empty() ? std::string(T("No message")) : d.attach_text,
                                      cw - 40, FontSize::Small).c_str());
                // Buttons, for a finger; A / Y / B do the same.
                const char *labels[3] = { T("Send"), T("Add text"), T("Cancel") };
                const int kinds[3] = { HSend, HText, HCancel };
                const int btw = 190, gap = 20, btx = cx + (cw - (btw * 3 + gap * 2)) / 2, bty = cy + ch - 64;
                for (int k = 0; k < 3; k++) {
                    const int x = btx + k * (btw + gap);
                    m_gfx->FillRect(x, bty, btw, 48, k == 0 ? t.accent : WithAlpha(t.fg, 40));
                    m_gfx->TextCentered(FontSize::Small, x + btw / 2, bty + 10, k == 0 ? t.bg_bottom : t.fg, labels[k]);
                    hit(x, bty, btw, 48, kinds[k]);
                }
                DrawStatusHint({ {{"a"}, "Send"}, {{"y"}, "Add text"}, {{"b"}, "Cancel"} });
                return;
            }

            if (d.view) {
                // The picture viewer: fitted to the screen, then zoomed and moved.
                d.hits.clear();
                const int H = gfx::Gfx::Height;
                m_gfx->FillRect(0, 0, W, H, SDL_Color{ 0, 0, 0, 240 });
                const float aw = (float)std::max(1, d.view_w), ah = (float)std::max(1, d.view_h);
                const float fit = std::min((float)W / aw, (float)(H - 60) / ah);
                const float dw = aw * fit * d.zoom, dh = ah * fit * d.zoom;
                const float mx = std::max(0.0f, (dw - W) / 2), my = std::max(0.0f, (dh - (H - 60)) / 2);
                d.pan_x = std::clamp(d.pan_x, -mx, mx);
                d.pan_y = std::clamp(d.pan_y, -my, my);
                const int x = (int)((W - dw) / 2 + d.pan_x), y = (int)((H - 60 - dh) / 2 + d.pan_y);
                // The big copy, sized once to the screen's limit; the chat's copy until it lands.
                if (d.view_url != d.view_still) {
                    int lw = 1920, lh = 1080;
                    FitBox((int)aw, (int)ah, lw, lh);
                    DiscordPicture(d.view_url, lw, lh);
                }
                if (!DiscordDraw(d.view_url, x, y, (int)dw, (int)dh) &&
                    !DiscordDraw(d.view_still, x, y, (int)dw, (int)dh))
                    m_gfx->TextCentered(FontSize::Normal, W / 2, H / 2 - 20, t.dim, T("Loading..."));
                if (d.zoom > 1.01f) {
                    char z[16];
                    snprintf(z, sizeof(z), "%.1fx", d.zoom);
                    m_gfx->Text(FontSize::Small, W - 90, 20, SDL_Color{ 255, 255, 255, 200 }, z);
                }
                DrawHint({ {{"l", "r"}, "Zoom"}, {{"dpad"}, d.zoom > 1.01f ? "Move" : "Next"}, {{"a"}, "Zoom"}, {{"b"}, "Close"} });
                return;
            }
            if (!d.draft.empty())
                DrawStatusHint({ {{"plus"}, "Send"}, {{"a"}, "Write"}, {{"minus"}, "Emoji"}, {{"x"}, "Send a file"}, {{"b"}, "Back"} });
            else
                DrawStatusHint({ {{"a"}, "Write"}, {{"minus"}, "Emoji"}, {{"x"}, "Send a file"}, {{"y"}, "View picture"}, {{"b"}, "Back"} });
            return;
        }

        DiscordBuildLists();
        // The server column: Direct Messages, then the servers.
        const int rail_n = (int)d.servers.size() + 1, rail_fit = (kDcBottom - kDcTop) / kDcRailRowH;
        const int rail_first = std::clamp(d.rail_sel - rail_fit / 2, 0, std::max(0, rail_n - rail_fit));
        d.rail_pos += ((float)rail_first - d.rail_pos) * 0.25f;
        if (std::fabs((float)rail_first - d.rail_pos) < 0.02f) d.rail_pos = (float)rail_first;
        m_gfx->FxClose();
        const SDL_Rect lclip{ 0, kDcTop - 6, W, kDcBottom - kDcTop + 6 };
        SDL_SetRenderClipRect(m_gfx->Renderer(), &lclip);
        for (int i = std::max(0, (int)d.rail_pos - 1); i < rail_n && i < (int)d.rail_pos + rail_fit + 1; i++) {
            const int y = kDcTop + (int)(((float)i - d.rail_pos) * kDcRailRowH);
            if (i == d.rail_sel)
                m_gfx->FillRect(kDcRailX - 4, y - 3, kDcRailW - 8, kDcRailRowH - 6,
                                WithAlpha(t.accent, d.rail ? 255 : 90));
            const DiscordService::Server *sv = i > 0 ? &d.servers[i - 1] : nullptr;
            hit(kDcRailX - 4, y - 3, kDcRailW - 8, kDcRailRowH - 6, HRail, i);
            if (!sv || !DiscordDraw(sv->iconUrl, kDcRailX + 8, y + 8, kDcAvatar, kDcAvatar))
                m_gfx->TextCentered(FontSize::Small, kDcRailX + 28, y + 16, t.fg,
                                        sv ? Plain(sv->initials).c_str() : "DM");
            if (sv && sv->unread) m_gfx->FillRect(kDcRailX - 10, y + 22, 4, 16, t.fg);
        }
        // The list, under who is signed in.
        DiscordText(FontSize::Small, kDcListX + 12, kDcTop - 4, t.dim, std::string(T("Signed in as")) + " " + s.myName(), W - kDcListX - 60);
        const int list_top = kDcTop + 30;
        const int n = (int)d.rows.size(), fit = (kDcBottom - list_top) / kDcListRowH;
        const int first = std::clamp(d.list_sel - fit / 2, 0, std::max(0, n - fit));
        d.list_pos += ((float)first - d.list_pos) * 0.25f;
        if (std::fabs((float)first - d.list_pos) < 0.02f) d.list_pos = (float)first;
        if (n == 0)
            m_gfx->TextCentered(FontSize::Normal, (kDcListX + W) / 2, 330, t.dim,
                                d.Guild() ? T("No channels") : T("No messages yet"));
        const SDL_Rect list_clip{ kDcListX - 4, list_top - 4, W - kDcListX + 4, kDcBottom - list_top + 4 };
        m_gfx->FxClose();
        SDL_SetRenderClipRect(m_gfx->Renderer(), &list_clip);
        for (int i = std::max(0, (int)d.list_pos - 1); i < n && i < (int)d.list_pos + fit + 1; i++) {
            const DiscordUi::Row &r = d.rows[i];
            const int y = list_top + (int)(((float)i - d.list_pos) * kDcListRowH);
            hit(kDcListX, std::max(y - 2, list_top - 2), W - kDcListX - 30, kDcListRowH - 4, HList, i);
            if (i == d.list_sel && !d.rail)
                m_gfx->FillRect(kDcListX, y - 2, W - kDcListX - 30, kDcListRowH - 4, WithAlpha(t.accent, 90));
            int x = kDcListX + 12;
            if (!r.avatar.empty()) {
                DiscordDraw(r.avatar, x, y + 2, kDcAvatar, kDcAvatar);
                x += kDcAvatar + 12;
            }
            const std::string name = r.category ? r.text : r.voice ? r.text + " (voice)" :
                                     d.Guild() ? "# " + r.text : r.text;
            const SDL_Color col = r.category || r.voice ? t.dim : r.unread ? t.fg : WithAlpha(t.fg, 190);
            DiscordText(r.category ? FontSize::Small : FontSize::Normal, x, y + 8, col, name, W - x - 110);
            if (r.mentions > 0) {
                char num[12];
                snprintf(num, sizeof(num), "%d", r.mentions);
                const int nw = m_gfx->TextWidth(FontSize::Small, num);
                m_gfx->FillRect(W - 70 - nw, y + 10, nw + 16, 28, SDL_Color{ 237, 66, 69, 255 });
                m_gfx->Text(FontSize::Small, W - 62 - nw, y + 12, SDL_Color{ 255, 255, 255, 255 }, num);
            }
        }
        m_gfx->FxClose();
        SDL_SetRenderClipRect(m_gfx->Renderer(), nullptr);
        DrawHint({ {{"a"}, "Open"}, {{"y"}, "Add account"}, {{"x"}, "Sign out"}, {{"minus"}, "Switch"}, {{"b"}, "Back"} });
    }

    // Touch: tap what is drawn (last frame's targets), drag to scroll, and in the
    // viewer drag to move, pinch or double-tap to zoom, swipe for the next picture.
    void Menu::OnTouchDiscord(int phase, int x, int y) {
        DiscordUi &d = *m_dc;
        DiscordService &s = d.svc;
        if (m_album_video_active) {   // the video: tap the bar to seek, anywhere else to pause
            if (phase != 0) return;
            const double dur = m_album_video.Duration();
            if (y > gfx::Gfx::Height - 120 && dur > 0.0)
                m_album_video.Seek(dur * std::clamp((x - 40) / (double)(gfx::Gfx::Width - 80), 0.0, 1.0));
            else
                OnButtonDiscord(Btn::A);
            return;
        }
        if (s.state() != DiscordService::State::Ready) {   // sign-in screens: a tap is A
            if (phase == 0) OnButtonDiscord(Btn::A);
            return;
        }
        const int dx = x - d.t_x0, dy = y - d.t_y0;
        if (phase == 0) {
            d.t_x0 = x; d.t_y0 = y;
            d.t_moved = d.t_pinch = false;
            d.t_pos0 = d.chat_pos;
            d.t_list0 = d.list_sel;
            d.t_rail0 = d.rail_sel;
            d.t_zoom0 = d.zoom;
            d.t_panx0 = d.pan_x;
            d.t_pany0 = d.pan_y;
            return;
        }
        if (phase == 1) {
            if (std::abs(dx) + std::abs(dy) > 16) d.t_moved = true;
            if (d.view) {
                HidTouchScreenState ts {};
                if (hidGetTouchScreenStates(&ts, 1) && ts.count >= 2) {
                    const float dist = std::max(1.0f, std::hypot((float)ts.touches[0].x - ts.touches[1].x,
                                                                 (float)ts.touches[0].y - ts.touches[1].y));
                    if (!d.t_pinch) { d.t_pinch = true; d.t_dist0 = dist; d.t_zoom0 = d.zoom; }
                    else SetZoom(d, d.t_zoom0 * dist / d.t_dist0);
                } else if (!d.t_pinch && d.zoom > 1.01f) {
                    d.pan_x = d.t_panx0 + dx;
                    d.pan_y = d.t_pany0 + dy;
                }
                return;
            }
            if (!d.t_moved || !d.attach.empty()) return;
            if (d.in_chat) {   // finger down shows older messages, like a phone
                const float rows = (float)std::max(0, (int)d.chat_rows.size() - 1);
                d.chat_pos = std::clamp(d.t_pos0 + dy / (float)kDcChatLineH, 0.0f, rows);
                d.chat_up = (int)std::lround(d.chat_pos);
            } else if (d.t_x0 < kDcListX) {
                const int n = (int)d.servers.size() + 1;
                d.rail = true;
                d.rail_sel = std::clamp(d.t_rail0 - dy / kDcRailRowH, 0, n - 1);
                if (d.rail_sel != d.t_rail0) { d.list_sel = 0; d.list_pos = 0; d.next_build = 0; }
            } else if (!d.rows.empty()) {
                d.rail = false;
                d.list_sel = std::clamp(d.t_list0 - dy / kDcListRowH, 0, (int)d.rows.size() - 1);
            }
            return;
        }
        // Lifted.
        if (d.t_pinch) return;
        if (d.view) {
            if (d.t_moved) {
                if (d.zoom <= 1.01f && std::abs(dx) > 120 && std::abs(dx) > std::abs(dy))
                    ViewStep(d, m_gfx, dx < 0 ? 1 : -1);
                return;
            }
            const u64 now = armGetSystemTick();
            if (d.t_last_tap && now - d.t_last_tap < armGetSystemTickFreq() * 35 / 100) {
                if (d.zoom > 1.01f) SetZoom(d, 1.0f);
                else {   // in on the spot that was tapped
                    SetZoom(d, 2.5f);
                    d.pan_x = (gfx::Gfx::Width / 2 - x) * 1.5f;
                    d.pan_y = ((gfx::Gfx::Height - 60) / 2 - y) * 1.5f;
                }
                d.t_last_tap = 0;
            } else {
                d.t_last_tap = now;
            }
            return;
        }
        if (d.t_moved) return;
        for (const DiscordUi::Hit &h : d.hits) {
            if (x < h.r.x || x >= h.r.x + h.r.w || y < h.r.y || y >= h.r.y + h.r.h) continue;
            switch (h.kind) {
                case HSend:   OnButtonDiscord(Btn::A); break;
                case HText:   OnButtonDiscord(Btn::Y); break;
                case HCancel: OnButtonDiscord(Btn::B); break;
                case HMedia:  ViewOpen(d, m_gfx, h.idx); break;
                case HAudio:  if (h.idx >= 0 && h.idx < (int)d.chat_rows.size()) DiscordPlay(h.idx);
                              break;
                case HInput:  OnButtonDiscord(Btn::A); break;
                case HPlus:   OnButtonDiscord(Btn::X); break;
                case HEmojiBtn: d.picker = !d.picker; break;
                case HSendDraft: OnButtonDiscord(Btn::Plus); break;
                case HEmoji:
                    d.pick_sel = h.idx;
                    if (d.draft.size() < 1900) d.draft += kEmo[h.idx].text;
                    break;
                case HRail:
                    d.rail = false;
                    if (h.idx != d.rail_sel) { d.rail_sel = h.idx; d.list_sel = 0; d.list_pos = 0; d.next_build = 0; }
                    break;
                case HList:
                    d.rail = false;
                    d.list_sel = h.idx;
                    OnButtonDiscord(Btn::A);   // opens it (categories and voice do nothing)
                    break;
            }
            return;
        }
    }

} // namespace sl::menu::ui
#endif
