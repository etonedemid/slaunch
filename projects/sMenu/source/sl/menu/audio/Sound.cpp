#include <sl/menu/audio/Sound.hpp>
#include <sl/menu/audio/Music.hpp>
#include <SDL3_mixer/SDL_mixer.h>

namespace sl::menu::audio {

    namespace {
        // Candidates per effect, tried in order and first hit wins.
        //
        // .wav is listed ahead of .mp3 because SDL_mixer always decodes WAV,
        // while MP3 depends on which decoders this build of the library was
        // compiled with - so the format that always works gets first refusal,
        // and the mp3 is there for anyone who only has that.
        //
        // Startup keeps opening.wav as a last resort: it is what shipped under
        // the old name, and an install that has not been given a startup sound
        // yet should keep the one it already had rather than falling silent.
        const char *kPaths[(int)Sfx::Count][3] = {
            { "sdmc:/slaunch/sounds/welcome.wav",    "sdmc:/slaunch/sounds/welcome.mp3",    nullptr },
            { "sdmc:/slaunch/sounds/page_left.wav",  "sdmc:/slaunch/sounds/page_left.mp3",  nullptr },
            { "sdmc:/slaunch/sounds/page_right.wav", "sdmc:/slaunch/sounds/page_right.mp3", nullptr },
            { "sdmc:/slaunch/sounds/startup.wav",    "sdmc:/slaunch/sounds/startup.mp3",
              "sdmc:/slaunch/sounds/opening.wav" },
            { "sdmc:/slaunch/sounds/confirm.wav",    "sdmc:/slaunch/sounds/confirm.mp3",    nullptr },
            { "sdmc:/slaunch/sounds/click.wav",      "sdmc:/slaunch/sounds/click.mp3",      nullptr },
            { "sdmc:/slaunch/sounds/back.wav",       "sdmc:/slaunch/sounds/back.mp3",       nullptr },
            { "sdmc:/slaunch/sounds/move.wav",       "sdmc:/slaunch/sounds/move.mp3",       nullptr },
        };
    }

    void Sound::Init(bool audio_ok) {
        MIX_Mixer *mixer = Mixer();
        if (!audio_ok || !mixer) return;
        for (int i = 0; i < (int)Sfx::Count; i++) {
            for (int k = 0; k < 3 && kPaths[i][k]; k++) {
                // Decoded once, up front: an effect must start on the frame
                // it is asked for.
                if ((m_sfx[i] = MIX_LoadAudio(mixer, kPaths[i][k], true))) break;
            }
            // Still null: that effect has no file and stays silent.
        }
        for (auto &v : m_voice) v = MIX_CreateTrack(mixer);
        m_move_voice = MIX_CreateTrack(mixer);
        m_ok = true;
        SetVolume(m_volume);
    }

    void Sound::Exit() {
        for (auto &v : m_voice) { if (v) MIX_DestroyTrack(v); v = nullptr; }
        if (m_move_voice) { MIX_DestroyTrack(m_move_voice); m_move_voice = nullptr; }
        for (auto &a : m_sfx) { if (a) MIX_DestroyAudio(a); a = nullptr; }
        m_ok = false;
    }

    void Sound::Play(Sfx s) {
        if (!m_ok) return;
        const int i = (int)s;
        if (i < 0 || i >= (int)Sfx::Count || !m_sfx[i]) return;
        MIX_Track *t = nullptr;
        if (s == Sfx::Move) t = m_move_voice;
        else for (MIX_Track *v : m_voice)
            if (v && !MIX_TrackPlaying(v)) { t = v; break; }
        if (!t) { t = m_voice[m_next]; m_next = (m_next + 1) % kVoices; }
        if (!t) return;
        MIX_SetTrackAudio(t, m_sfx[i]);
        MIX_PlayTrack(t, 0);
    }

    void Sound::SetVolume(int vol) {
        m_volume = vol < 0 ? 0 : (vol > 100 ? 100 : vol);
        if (!m_ok) return;
        for (MIX_Track *v : m_voice)
            if (v) MIX_SetTrackGain(v, m_volume / 100.0f);
        if (m_move_voice) MIX_SetTrackGain(m_move_voice, m_volume / 100.0f);
    }

} // namespace sl::menu::audio
