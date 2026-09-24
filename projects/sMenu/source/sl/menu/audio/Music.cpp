#include <sl/menu/audio/Music.hpp>
#include <sl/menu/cfg/UserCfg.hpp>
#include <string>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace sl::menu::audio {

    namespace {
        constexpr const char *kDir   = "sdmc:/slaunch/music";
        // The tracks are shared; which one is playing, how loud, and whether
        // music is on at all is per account.
        inline std::string StatePath() { return cfg::Path("music.txt"); }

        bool HasAudioExt(const char *name) {
            size_t n = strlen(name);
            auto ends = [&](const char *e) {
                size_t k = strlen(e);
                return n > k && strcasecmp(name + n - k, e) == 0;
            };
            return ends(".mp3") || ends(".ogg") || ends(".flac") ||
                   ends(".opus") || ends(".mod") || ends(".wav");
        }

        std::string BaseName(const std::string &path) {
            size_t slash = path.find_last_of("/\\");
            std::string b = (slash == std::string::npos) ? path : path.substr(slash + 1);
            size_t dot = b.find_last_of('.');
            if (dot != std::string::npos) b = b.substr(0, dot);
            return b;
        }

        bool ReadFile(const std::string &path, std::vector<u8> &out) {
            FILE *fp = fopen(path.c_str(), "rb");
            if (!fp) return false;
            fseek(fp, 0, SEEK_END);
            const long n = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (n > 0 && n < (16 << 20)) {
                out.resize((size_t)n);
                if (fread(out.data(), 1, out.size(), fp) != out.size()) out.clear();
            }
            fclose(fp);
            return !out.empty();
        }

        u32 Be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
        u32 Syncsafe(const u8 *p) { return (u32)p[0] << 21 | (u32)p[1] << 14 | (u32)p[2] << 7 | p[3]; }

        // Picture out of an ID3v2.2/2.3/2.4 tag: the front cover (type 3) if
        // there is one, else the first picture.
        // ponytail: ignores tag-wide unsynchronisation; a tag written that way
        // just yields a broken image, which the decoder rejects.
        std::vector<u8> Id3Picture(FILE *fp) {
            u8 h[10];
            if (fread(h, 1, 10, fp) != 10 || memcmp(h, "ID3", 3) != 0) return {};
            const int ver = h[3];
            if (ver < 2 || ver > 4) return {};
            const u32 size = Syncsafe(h + 6);
            if (size == 0 || size > (16u << 20)) return {};
            std::vector<u8> tag(size);
            if (fread(tag.data(), 1, size, fp) != size) return {};

            size_t pos = 0;
            if ((h[5] & 0x40) && ver >= 3 && size >= 4)   // extended header
                pos = (ver == 4) ? Syncsafe(tag.data()) : Be32(tag.data()) + 4;

            const size_t hdr = (ver == 2) ? 6 : 10;
            std::vector<u8> first;
            while (pos + hdr <= size && tag[pos] != 0) {
                const u8 *f = tag.data() + pos;
                const u32 flen = (ver == 2) ? ((u32)f[3] << 16 | (u32)f[4] << 8 | f[5])
                               : (ver == 4) ? Syncsafe(f + 4) : Be32(f + 4);
                if (flen > size - pos - hdr) break;
                const bool pic = (ver == 2) ? memcmp(f, "PIC", 3) == 0
                                            : memcmp(f, "APIC", 4) == 0;
                if (pic && flen > 4) {
                    const u8 *d = f + hdr, *end = d + flen;
                    const u8 enc = *d++;
                    if (ver == 2) d += 3;                     // "JPG"/"PNG"
                    else { while (d < end && *d) d++; d++; }  // mime + NUL
                    const u8 type = (d < end) ? *d++ : 0;
                    // Description, terminated by a NUL of the text encoding's width.
                    if (enc == 1 || enc == 2) {
                        while (d + 1 < end && (d[0] || d[1])) d += 2;
                        d += 2;
                    } else {
                        while (d < end && *d) d++;
                        d += 1;
                    }
                    if (d < end) {
                        std::vector<u8> img(d, end);
                        if (type == 3) return img;
                        if (first.empty()) first = std::move(img);
                    }
                }
                pos += hdr + flen;
            }
            return first;
        }

        u32 Le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
        u64 Le64(const u8 *p) { return (u64)Le32(p) | (u64)Le32(p + 4) << 32; }

        // ---- track length, per container ------------------------------------
        // MP3: skip any ID3v2 tag, read the first frame header, and take the
        // frame count from a Xing/Info or VBRI header when there is one; a
        // plain CBR file has none, so its length is size over bitrate.
        double Mp3Seconds(FILE *fp, long size) {
            u8 h[10];
            long off = 0;
            if (fread(h, 1, 10, fp) == 10 && memcmp(h, "ID3", 3) == 0)
                off = 10 + (long)Syncsafe(h + 6) + ((h[5] & 0x10) ? 10 : 0);
            std::vector<u8> b(8192);
            fseek(fp, off, SEEK_SET);
            const size_t n = fread(b.data(), 1, b.size(), fp);
            for (size_t i = 0; i + 4 <= n; i++) {
                if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
                const int ver  = (b[i + 1] >> 3) & 3;      // 3 = MPEG1, 2 = MPEG2, 0 = 2.5
                const int lay  = (b[i + 1] >> 1) & 3;      // 1 = layer III
                const int bri  = b[i + 2] >> 4, sri = (b[i + 2] >> 2) & 3;
                if (ver == 1 || lay != 1 || bri == 0 || bri == 15 || sri == 3) continue;
                static const int kBr1[] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320 };
                static const int kBr2[] = { 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160 };
                static const int kSr[]  = { 44100, 48000, 32000 };
                const bool mpeg1 = (ver == 3);
                const int  rate  = kSr[sri] >> (mpeg1 ? 0 : (ver == 2 ? 1 : 2));
                const int  kbps  = mpeg1 ? kBr1[bri] : kBr2[bri];
                const int  spf   = mpeg1 ? 1152 : 576;
                const bool mono  = ((b[i + 3] >> 6) & 3) == 3;
                const size_t side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
                const size_t x = i + 4 + side;
                if (x + 12 <= n && (!memcmp(&b[x], "Xing", 4) || !memcmp(&b[x], "Info", 4)) &&
                    (b[x + 7] & 1))
                    return (double)Be32(&b[x + 8]) * spf / rate;
                if (i + 36 + 18 <= n && !memcmp(&b[i + 36], "VBRI", 4))
                    return (double)Be32(&b[i + 36 + 14]) * spf / rate;
                return (double)(size - off - (long)i) * 8.0 / (kbps * 1000.0);
            }
            return 0.0;
        }
        // FLAC: total samples over sample rate, from STREAMINFO.
        double FlacSeconds(FILE *fp) {
            u8 h[4 + 4 + 18];
            if (fread(h, 1, sizeof(h), fp) != sizeof(h) || memcmp(h, "fLaC", 4) != 0) return 0.0;
            const u8 *d = h + 8;
            const u32 rate  = (u32)d[10] << 12 | (u32)d[11] << 4 | d[12] >> 4;
            const u64 total = (u64)(d[13] & 0x0F) << 32 | Be32(d + 14);
            return rate ? (double)total / rate : 0.0;
        }
        // Ogg Vorbis / Opus: the last page's granule position is the sample
        // count; the first page says which codec, and so the rate.
        double OggSeconds(FILE *fp, long size) {
            u8 h[64];
            if (fread(h, 1, sizeof(h), fp) != sizeof(h) || memcmp(h, "OggS", 4) != 0) return 0.0;
            const u8 *pk = h + 27 + h[26];                 // first packet, past the segment table
            if (pk + 19 > h + sizeof(h)) return 0.0;
            double rate = 0.0; u64 preskip = 0;
            if (!memcmp(pk, "\x01vorbis", 7))  rate = Le32(pk + 12);
            else if (!memcmp(pk, "OpusHead", 8)) { rate = 48000.0; preskip = (u64)pk[10] | (u64)pk[11] << 8; }
            if (rate <= 0.0) return 0.0;
            const long tail = std::min(size, 65536L);
            std::vector<u8> b((size_t)tail);
            fseek(fp, size - tail, SEEK_SET);
            if (fread(b.data(), 1, b.size(), fp) != b.size()) return 0.0;
            for (long i = tail - 14; i >= 0; i--)
                if (!memcmp(&b[(size_t)i], "OggS", 4)) {
                    const u64 g = Le64(&b[(size_t)i + 6]);
                    return g > preskip ? (double)(g - preskip) / rate : 0.0;
                }
            return 0.0;
        }
        // WAV: the data chunk's size over the format's byte rate.
        double WavSeconds(FILE *fp) {
            u8 h[12];
            if (fread(h, 1, 12, fp) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0.0;
            u32 byte_rate = 0;
            u8 c[8];
            while (fread(c, 1, 8, fp) == 8) {
                const u32 len = Le32(c + 4);
                if (!memcmp(c, "fmt ", 4)) {
                    u8 f[16];
                    if (len < 16 || fread(f, 1, 16, fp) != 16) return 0.0;
                    byte_rate = Le32(f + 8);
                    fseek(fp, (long)(len - 16 + (len & 1)), SEEK_CUR);
                } else if (!memcmp(c, "data", 4)) {
                    return byte_rate ? (double)len / byte_rate : 0.0;
                } else {
                    fseek(fp, (long)(len + (len & 1)), SEEK_CUR);
                }
            }
            return 0.0;
        }

        // PICTURE metadata block out of a native FLAC file, same preference.
        std::vector<u8> FlacPicture(FILE *fp) {
            u8 h[4];
            if (fread(h, 1, 4, fp) != 4 || memcmp(h, "fLaC", 4) != 0) return {};
            std::vector<u8> first;
            for (bool last = false; !last;) {
                if (fread(h, 1, 4, fp) != 4) break;
                last = (h[0] & 0x80) != 0;
                const u32 len = (u32)h[1] << 16 | (u32)h[2] << 8 | h[3];
                if ((h[0] & 0x7F) != 6) { fseek(fp, len, SEEK_CUR); continue; }
                std::vector<u8> b(len);
                if (fread(b.data(), 1, len, fp) != len) break;
                // type, mime, description, 4x u32 geometry, then length + data.
                size_t p = 0;
                auto u32at = [&](u32 &v) { if (p + 4 > len) return false; v = Be32(&b[p]); p += 4; return true; };
                u32 type, n, skip, dl;
                if (!u32at(type) || !u32at(n) || (p += n) > len || !u32at(n) ||
                    (p += n) > len || !u32at(skip) || !u32at(skip) || !u32at(skip) ||
                    !u32at(skip) || !u32at(dl) || dl > len - p) continue;
                std::vector<u8> img(b.begin() + p, b.begin() + p + dl);
                if (type == 3) return img;
                if (first.empty()) first = std::move(img);
            }
            return first;
        }
    }

    bool Music::Init() {
        ScanTracks();
        LoadState();

        // Mix_OpenAudio needs the SDL audio subsystem; gfx.Init only brought up
        // VIDEO|JOYSTICK. Bring up AUDIO here, tolerating failure - music is
        // optional and must never take the menu down.
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { m_ok = false; return false; }
        if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096) != 0) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            m_ok = false;
            return false;
        }
        int want = MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_FLAC;
#ifdef MIX_INIT_OPUS
        want |= MIX_INIT_OPUS;
#endif
        Mix_Init(want);
        m_ok = true;
        ApplyVolume();

        m_last_tick = armGetSystemTick();
        if (m_enabled && !m_tracks.empty())
            StartResumeLoad(m_pos);   // resume where we left off, off the main thread
        return true;
    }

    // Load + seek the current track on a worker so a ~2s MP3 seek never freezes the
    // menu on start. The main thread leaves the mixer alone while m_loading is set.
    void Music::ResumeTrampoline(void *self) {
        Music *m = static_cast<Music *>(self);
        m->PlayCurrent(m->m_load_pos);
        m->m_loading.store(false, std::memory_order_release);
    }

    void Music::StartResumeLoad(double start_seconds) {
        m_load_pos = start_seconds;
        m_loading.store(true, std::memory_order_release);
        if (R_SUCCEEDED(threadCreate(&m_load_thread, &Music::ResumeTrampoline, this,
                                     nullptr, 0x20000, 0x3B, -2))) {
            threadStart(&m_load_thread);
            m_load_running = true;
        } else {
            PlayCurrent(start_seconds);   // fallback: synchronous
            m_loading.store(false, std::memory_order_release);
        }
    }

    void Music::Exit() {
        if (m_load_running) {   // let the resume worker finish before we free anything
            threadWaitForExit(&m_load_thread);
            threadClose(&m_load_thread);
            m_load_running = false;
        }
        SaveState();
        if (m_music) { Mix_FreeMusic((Mix_Music *)m_music); m_music = nullptr; }
        if (m_ok) {
            Mix_HaltMusic();
            Mix_CloseAudio();
            Mix_Quit();
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            m_ok = false;
        }
    }

    void Music::ScanTracks() {
        m_tracks.clear();
        DIR *d = opendir(kDir);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            if (HasAudioExt(e->d_name))
                m_tracks.push_back(std::string(kDir) + "/" + e->d_name);
        }
        closedir(d);
        std::sort(m_tracks.begin(), m_tracks.end());
    }

    void Music::PlayCurrent(double start_seconds) {
        if (!m_ok || m_tracks.empty()) return;
        if (m_index < 0 || m_index >= (int)m_tracks.size()) m_index = 0;

        if (m_music) { Mix_FreeMusic((Mix_Music *)m_music); m_music = nullptr; }
        Mix_Music *mus = Mix_LoadMUS(m_tracks[m_index].c_str());
        if (!mus) { m_pos = 0.0; return; }
        m_music = mus;

        Mix_PlayMusic(mus, 1);         // play once; Update() advances at the end
        if (start_seconds > 0.5) {
            Mix_RewindMusic();
            if (Mix_SetMusicPosition(start_seconds) == 0) m_pos = start_seconds;
            else                                          m_pos = 0.0;
        } else {
            m_pos = 0.0;
        }
    }

    void Music::Update() {
        const u64 now = armGetSystemTick();
        // While the resume worker loads/seeks, don't touch the mixer and keep the
        // clock fresh so m_pos doesn't jump by the whole load time afterwards.
        if (m_loading.load(std::memory_order_acquire)) { m_last_tick = now; return; }
        const double dt = (double)(now - m_last_tick) / (double)armGetSystemTickFreq();
        m_last_tick = now;

        if (!m_ok || !m_enabled || m_tracks.empty()) return;

        if (Mix_PlayingMusic() && !Mix_PausedMusic()) {
            m_pos += dt;
        } else if (m_music && !Mix_PausedMusic()) {
            // Track finished.
            if (m_repeat == RepeatOne) {
                PlayCurrent(0.0);
            } else if (m_repeat == RepeatOff && !m_shuffle &&
                       m_index + 1 >= (int)m_tracks.size()) {
                m_index = 0;                    // end of the list: stop, ready
                m_pos = 0.0;                    // to start again from the top
                m_enabled = false;
                Mix_FreeMusic((Mix_Music *)m_music);
                m_music = nullptr;
                SaveState();
            } else {
                Next();
            }
        }
    }

    void Music::SetEnabled(bool on) {
        if (m_loading.load(std::memory_order_acquire)) return;   // don't race the resume worker
        if (on == m_enabled) return;
        m_enabled = on;
        if (!m_ok) { SaveState(); return; }
        if (on) {
            if (m_music && Mix_PausedMusic()) Mix_ResumeMusic();
            else PlayCurrent(m_pos);
        } else {
            if (Mix_PlayingMusic()) Mix_PauseMusic();
        }
        SaveState();
    }

    void Music::ApplyVolume() {
        if (m_ok) Mix_VolumeMusic(m_volume * MIX_MAX_VOLUME / 100);
    }

    void Music::SetVolume(int vol) {
        m_volume = std::min(100, std::max(0, vol));
        ApplyVolume();
        SaveState();
    }

    void Music::ToggleShuffle() { m_shuffle = !m_shuffle; SaveState(); }

    void Music::Next() {
        if (m_loading.load(std::memory_order_acquire)) return;
        if (m_tracks.empty()) return;
        if (m_shuffle && m_tracks.size() > 1) {
            int n = m_index;
            while (n == m_index) n = (int)(randomGet64() % m_tracks.size());
            m_index = n;
        } else {
            m_index = (m_index + 1) % (int)m_tracks.size();
        }
        if (m_enabled) PlayCurrent(0.0);
        SaveState();
    }

    void Music::Prev() {
        if (m_loading.load(std::memory_order_acquire)) return;
        if (m_tracks.empty()) return;
        // A few seconds in, "previous" means the start of this track - the
        // way every player's back button works.
        if (m_pos > 3.0 && m_music) { Seek(0.0); return; }
        m_index = (m_index + (int)m_tracks.size() - 1) % (int)m_tracks.size();
        if (m_enabled) PlayCurrent(0.0);
        SaveState();
    }

    void Music::SelectTrack(int i) {
        if (m_loading.load(std::memory_order_acquire)) return;
        if (i < 0 || i >= (int)m_tracks.size()) return;
        m_index = i;
        if (m_enabled) PlayCurrent(0.0);
        SaveState();
    }

    std::string Music::TrackName(int i) const {
        if (i < 0 || i >= (int)m_tracks.size()) return "";
        return BaseName(m_tracks[i]);
    }

    std::string Music::CurrentName() const { return TrackName(m_index); }

    double Music::Duration(int i) {
        if (i < 0 || i >= (int)m_tracks.size()) return 0.0;
        if (m_dur.size() != m_tracks.size()) m_dur.assign(m_tracks.size(), -1.0);
        if (m_dur[i] >= 0.0) return m_dur[i];
        double d = 0.0;
        if (FILE *fp = fopen(m_tracks[i].c_str(), "rb")) {
            fseek(fp, 0, SEEK_END);
            const long size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            const char *ext = strrchr(m_tracks[i].c_str(), '.');
            if (ext && !strcasecmp(ext, ".mp3"))       d = Mp3Seconds(fp, size);
            else if (ext && !strcasecmp(ext, ".flac")) d = FlacSeconds(fp);
            else if (ext && (!strcasecmp(ext, ".ogg") || !strcasecmp(ext, ".opus"))) d = OggSeconds(fp, size);
            else if (ext && !strcasecmp(ext, ".wav"))  d = WavSeconds(fp);
            fclose(fp);
        }
        m_dur[i] = (d > 0.0 && d < 24 * 3600.0) ? d : 0.0;
        return m_dur[i];
    }

    void Music::Seek(double seconds) {
        if (m_loading.load(std::memory_order_acquire) || !m_ok || !m_music) return;
        const double dur = Duration(m_index);
        if (seconds < 0.0) seconds = 0.0;
        if (dur > 0.0 && seconds > dur - 1.0) seconds = std::max(0.0, dur - 1.0);
        Mix_RewindMusic();
        if (seconds < 0.5 || Mix_SetMusicPosition(seconds) == 0) m_pos = seconds < 0.5 ? 0.0 : seconds;
    }

    void Music::CycleRepeat() {
        m_repeat = (Repeat)((m_repeat + 1) % RepeatCount);
        SaveState();
    }

    std::vector<u8> Music::CoverArt(int i) const {
        std::vector<u8> out;
        if (i < 0 || i >= (int)m_tracks.size()) return out;
        const std::string &path = m_tracks[i];
        const std::string stem  = path.substr(0, path.find_last_of('.'));
        for (const char *ext : { ".jpg", ".png", ".jpeg" })
            if (ReadFile(stem + ext, out)) return out;

        FILE *fp = fopen(path.c_str(), "rb");
        if (!fp) return out;
        out = Id3Picture(fp);
        if (out.empty()) { fseek(fp, 0, SEEK_SET); out = FlacPicture(fp); }
        fclose(fp);
        return out;
    }

    void Music::LoadState() {
        FILE *fp = fopen(StatePath().c_str(), "r");
        if (!fp) return;
        char line[64];
        std::string want;
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            int v = 0; double d = 0; char buf[48];
            if (sscanf(line, "enabled=%d", &v) == 1)      m_enabled = (v != 0);
            else if (sscanf(line, "volume=%d", &v) == 1)  m_volume  = std::min(100, std::max(0, v));
            else if (sscanf(line, "shuffle=%d", &v) == 1) m_shuffle = (v != 0);
            else if (sscanf(line, "repeat=%d", &v) == 1)  m_repeat  = (Repeat)((unsigned)v % RepeatCount);
            else if (sscanf(line, "pos=%lf", &d) == 1)    m_pos     = (d > 0 ? d : 0);
            else if (sscanf(line, "track=%47[^\n]", buf) == 1) want = buf;
        }
        fclose(fp);
        if (!want.empty())
            for (int i = 0; i < (int)m_tracks.size(); i++)
                if (BaseName(m_tracks[i]) == want) { m_index = i; break; }
    }

    void Music::SaveState() {
        cfg::EnsureDir();
        FILE *fp = fopen(StatePath().c_str(), "w");
        if (!fp) return;
        fprintf(fp, "enabled=%d\n", m_enabled ? 1 : 0);
        fprintf(fp, "volume=%d\n",  m_volume);
        fprintf(fp, "shuffle=%d\n", m_shuffle ? 1 : 0);
        fprintf(fp, "repeat=%d\n",  (int)m_repeat);
        fprintf(fp, "pos=%.2f\n",   m_pos);
        if (!m_tracks.empty() && m_index >= 0 && m_index < (int)m_tracks.size())
            fprintf(fp, "track=%s\n", BaseName(m_tracks[m_index]).c_str());
        fclose(fp);
    }

} // namespace sl::menu::audio
