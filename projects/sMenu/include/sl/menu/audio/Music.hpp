#pragma once
#include <switch.h>
#include <string>
#include <vector>
#include <atomic>

// Background music for the main menu. Tracks are loaded from sdmc:/slaunch/music
// (mp3/ogg/flac/opus/mod). The current track + play position are persisted so
// playback resumes where it left off after launching a game and returning - the
// menu applet is torn down on launch, so we save on Exit and restore on Init.
//
// SDL_mixer 2.0.4 has Mix_SetMusicPosition (seek) but no Mix_GetMusicPosition, so
// the elapsed position is tracked here by accumulating frame time while playing.

namespace sl::menu::audio {

    class Music {
    public:
        bool Init();     // open the mixer, scan tracks, load saved state, resume
        void Exit();     // save state (incl. position), stop, close the mixer
        void Update();   // advance position + auto-advance to the next track at end

        void SetEnabled(bool on);
        bool Enabled() const { return m_enabled; }
        void SetVolume(int vol);         // 0..100
        int  Volume() const { return m_volume; }
        void ToggleShuffle();
        bool Shuffle() const { return m_shuffle; }
        // What happens when a track ends: go on to the next (All), play it
        // again (One), or go on and stop after the last one (Off).
        enum Repeat { RepeatAll = 0, RepeatOne, RepeatOff, RepeatCount };
        void   CycleRepeat();
        Repeat RepeatMode() const { return m_repeat; }

        double Position() const { return m_pos; }   // seconds into the current track
        // Length of track i in seconds, read from the file's own headers
        // (SDL_mixer 2.0.4 cannot tell us) and cached. 0 when unknown.
        double Duration(int i);
        void   Seek(double seconds);                // within the current track

        void Next();
        void Prev();
        void SelectTrack(int i);         // pick by playlist index and play it

        int  TrackCount() const { return (int)m_tracks.size(); }
        int  TrackIndex() const { return m_index; }
        std::string TrackName(int i) const;   // display name (file base, no ext)
        std::string CurrentName() const;
        // Encoded cover image for track i (jpg/png bytes), empty if it has none:
        // a sidecar <track>.jpg/.png beside it, else a picture embedded in the
        // file's ID3v2 tag (mp3) or FLAC metadata.
        std::vector<u8> CoverArt(int i) const;

        void SaveState();

    private:
        void ScanTracks();
        void LoadState();
        void PlayCurrent(double start_seconds = 0.0);
        void ApplyVolume();

        // Loading + seeking an MP3 to resume position can take ~2s, so the initial
        // resume runs on a worker thread instead of blocking the menu-start path.
        // While it runs, m_loading is set and the main thread leaves the mixer alone.
        static void ResumeTrampoline(void *self);
        void StartResumeLoad(double start_seconds);

        Thread m_load_thread{};
        bool   m_load_running = false;
        std::atomic<bool> m_loading{false};
        double m_load_pos = 0.0;

        bool   m_ok       = false;   // Mix_OpenAudio succeeded
        bool   m_enabled  = true;
        int    m_volume   = 55;      // 0..100
        bool   m_shuffle  = false;
        Repeat m_repeat   = RepeatAll;
        std::vector<std::string> m_tracks;   // full paths
        std::vector<double>      m_dur;      // Duration cache, -1 = not read yet
        int    m_index    = 0;
        double m_pos      = 0.0;     // seconds into the current track
        u64    m_last_tick = 0;      // for the Update() dt
        void  *m_music    = nullptr; // Mix_Music*
    };

} // namespace sl::menu::audio
