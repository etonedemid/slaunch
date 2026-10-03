#pragma once
#include <switch.h>

// Short UI sound effects (nav clicks, welcome chime), on tracks of their own
// on the mixer Music::Init opens - so SFX layer over the background music.
// Init is a no-op if audio never came up.
//
// Assets live under sdmc:/slaunch/sounds. Each effect is looked up under a few
// candidate names in turn, so either an .mp3 or a .wav works and you can drop in
// whichever you have. A missing file is not an error: that effect simply stays
// silent, which is how a build ships before its audio does.

struct MIX_Audio;
struct MIX_Track;

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
        void Init(bool audio_ok);   // load effects; pass Music::Init()'s result
        void Exit();                // free them (before the mixer is closed)
        void Play(Sfx s);
        void SetVolume(int vol);    // 0..100

    private:
        bool  m_ok = false;
        int   m_volume = 70;
        MIX_Audio *m_sfx[(int)Sfx::Count] = {};
        // A few voices so nav clicks can overlap; the oldest is cut short
        // when all are busy.
        static constexpr int kVoices = 6;
        MIX_Track *m_voice[kVoices] = {};
        int        m_next = 0;
    };

} // namespace sl::menu::audio
