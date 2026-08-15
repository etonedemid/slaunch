#include <sl/menu/audio/Sound.hpp>
#include <SDL2/SDL_mixer.h>

namespace sl::menu::audio {

    namespace {
        // Candidates per effect, tried in order and first hit wins.
        //
        // .wav is listed ahead of .mp3 because SDL_mixer decodes WAV as a chunk
        // unconditionally, while MP3-as-a-chunk depends on which decoders this
        // build of the library was compiled with - so the format that always
        // works gets first refusal, and the mp3 is there for anyone who only has
        // that.
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
        };
    }

    void Sound::Init(bool audio_ok) {
        if (!audio_ok) return;
        Mix_AllocateChannels(8);          // a few voices so nav clicks can overlap
        for (int i = 0; i < (int)Sfx::Count; i++) {
            for (int k = 0; k < 3 && kPaths[i][k]; k++) {
                if ((m_chunks[i] = Mix_LoadWAV(kPaths[i][k]))) break;
            }
            // Still null: that effect has no file and stays silent.
        }
        m_ok = true;
        SetVolume(m_volume);
    }

    void Sound::Exit() {
        for (auto &c : m_chunks) {
            if (c) Mix_FreeChunk((Mix_Chunk *)c);
            c = nullptr;
        }
        m_ok = false;
    }

    void Sound::Play(Sfx s) {
        if (!m_ok) return;
        const int i = (int)s;
        if (i < 0 || i >= (int)Sfx::Count || !m_chunks[i]) return;
        Mix_PlayChannel(-1, (Mix_Chunk *)m_chunks[i], 0);
    }

    void Sound::SetVolume(int vol) {
        m_volume = vol < 0 ? 0 : (vol > 100 ? 100 : vol);
        if (!m_ok) return;
        for (auto c : m_chunks)
            if (c) Mix_VolumeChunk((Mix_Chunk *)c, m_volume * MIX_MAX_VOLUME / 100);
    }

} // namespace sl::menu::audio
