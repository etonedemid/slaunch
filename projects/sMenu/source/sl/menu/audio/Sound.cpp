#include <sl/menu/audio/Sound.hpp>
#include <SDL2/SDL_mixer.h>

namespace sl::menu::audio {

    namespace {
        const char *kPaths[(int)Sfx::Count] = {
            "sdmc:/slaunch/sounds/welcome.wav",
            "sdmc:/slaunch/sounds/page_left.wav",
            "sdmc:/slaunch/sounds/page_right.wav",
        };
    }

    void Sound::Init(bool audio_ok) {
        if (!audio_ok) return;
        Mix_AllocateChannels(8);          // a few voices so nav clicks can overlap
        for (int i = 0; i < (int)Sfx::Count; i++)
            m_chunks[i] = Mix_LoadWAV(kPaths[i]);   // nullptr if the file is missing
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
