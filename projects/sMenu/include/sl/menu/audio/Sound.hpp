#pragma once
#include <switch.h>

// Short UI sound effects (nav clicks, welcome chime). Played on SDL_mixer's
// channel mixer, which is separate from Mix_Music - so SFX layer over the
// background music. Requires the mixer to already be open (Music::Init does
// Mix_OpenAudio); Init is a no-op if audio never came up.
//
// Assets: WAV files under sdmc:/slaunch/sounds (welcome/page_left/page_right).

namespace sl::menu::audio {

    enum class Sfx { Welcome, PageLeft, PageRight, Count };

    class Sound {
    public:
        void Init(bool audio_ok);   // load chunks; pass Music::Init()'s result
        void Exit();                // free chunks (before the mixer is closed)
        void Play(Sfx s);
        void SetVolume(int vol);    // 0..100

    private:
        bool  m_ok = false;
        int   m_volume = 70;
        void *m_chunks[(int)Sfx::Count] = {};  // Mix_Chunk*
    };

} // namespace sl::menu::audio
