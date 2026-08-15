#pragma once
#include <switch.h>

// Short UI sound effects (nav clicks, welcome chime). Played on SDL_mixer's
// channel mixer, which is separate from Mix_Music - so SFX layer over the
// background music. Requires the mixer to already be open (Music::Init does
// Mix_OpenAudio); Init is a no-op if audio never came up.
//
// Assets live under sdmc:/slaunch/sounds. Each effect is looked up under a few
// candidate names in turn, so either an .mp3 or a .wav works and you can drop in
// whichever you have. A missing file is not an error: that effect simply stays
// silent, which is how a build ships before its audio does.

namespace sl::menu::audio {

    enum class Sfx {
        Welcome,
        PageLeft,
        PageRight,
        Startup,    // boot / post-setup chime (was "Opening")
        Confirm,    // weighty yes: launching something, saving a theme
        Click,      // ordinary yes: opening a submenu, toggling a setting
        Back,       // leaving a screen
        Count
    };

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
