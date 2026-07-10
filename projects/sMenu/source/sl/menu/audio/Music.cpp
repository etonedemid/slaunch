#include <sl/menu/audio/Music.hpp>
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
        constexpr const char *kState = "sdmc:/slaunch/config/music.txt";

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
            PlayCurrent(m_pos);   // resume where the last session left off
        return true;
    }

    void Music::Exit() {
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
        const double dt = (double)(now - m_last_tick) / (double)armGetSystemTickFreq();
        m_last_tick = now;

        if (!m_ok || !m_enabled || m_tracks.empty()) return;

        if (Mix_PlayingMusic() && !Mix_PausedMusic()) {
            m_pos += dt;
        } else if (m_music && !Mix_PausedMusic()) {
            Next();   // track finished -> advance
        }
    }

    void Music::SetEnabled(bool on) {
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
        if (m_tracks.empty()) return;
        m_index = (m_index + (int)m_tracks.size() - 1) % (int)m_tracks.size();
        if (m_enabled) PlayCurrent(0.0);
        SaveState();
    }

    void Music::SelectTrack(int i) {
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

    void Music::LoadState() {
        FILE *fp = fopen(kState, "r");
        if (!fp) return;
        char line[64];
        std::string want;
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            int v = 0; double d = 0; char buf[48];
            if (sscanf(line, "enabled=%d", &v) == 1)      m_enabled = (v != 0);
            else if (sscanf(line, "volume=%d", &v) == 1)  m_volume  = std::min(100, std::max(0, v));
            else if (sscanf(line, "shuffle=%d", &v) == 1) m_shuffle = (v != 0);
            else if (sscanf(line, "pos=%lf", &d) == 1)    m_pos     = (d > 0 ? d : 0);
            else if (sscanf(line, "track=%47[^\n]", buf) == 1) want = buf;
        }
        fclose(fp);
        if (!want.empty())
            for (int i = 0; i < (int)m_tracks.size(); i++)
                if (BaseName(m_tracks[i]) == want) { m_index = i; break; }
    }

    void Music::SaveState() {
        mkdir("sdmc:/slaunch", 0777);
        mkdir("sdmc:/slaunch/config", 0777);
        FILE *fp = fopen(kState, "w");
        if (!fp) return;
        fprintf(fp, "enabled=%d\n", m_enabled ? 1 : 0);
        fprintf(fp, "volume=%d\n",  m_volume);
        fprintf(fp, "shuffle=%d\n", m_shuffle ? 1 : 0);
        fprintf(fp, "pos=%.2f\n",   m_pos);
        if (!m_tracks.empty() && m_index >= 0 && m_index < (int)m_tracks.size())
            fprintf(fp, "track=%s\n", BaseName(m_tracks[m_index]).c_str());
        fclose(fp);
    }

} // namespace sl::menu::audio
