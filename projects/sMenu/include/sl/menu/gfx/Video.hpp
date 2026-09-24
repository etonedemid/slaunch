#pragma once
#include <SDL2/SDL.h>
#include <switch.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Video playback: looped and silent for theme wallpapers, or with sound,
// pause and seeking for the album's clip player.
//
// Decodes on a worker thread (libnx Thread, same pattern as
// Menu::ShotDecodeTrampoline in Menu_Flow.cpp) into a pair of plain NV12
// pixel buffers; the main thread's Tick() uploads whichever one is ready via
// SDL_UpdateNVTexture. All SDL_Renderer calls stay on the main thread, same
// rule the cover-art decode already follows.
//
// On the real Switch build this decodes on the NVDEC hardware block via the
// nvtegra hwaccel ported from averne/FFmpeg (see THIRDPARTY.md); on other
// builds (desktop simulator) the same code runs FFmpeg's software H.264
// decoder instead - same source, FFmpeg just does not offer the hwaccel
// there. See scripts/build-ffmpeg.sh.
//
// Built only when SL_HAS_VIDEO is defined (real Switch build, and the native
// desktop simulator, both of which link FFmpeg); everything is a harmless
// no-op stub otherwise (e.g. the Windows simulator build, which does not
// stage FFmpeg for MinGW).

struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;
struct AVPacket;
struct AVFrame;

namespace sl::menu::gfx {

    class Gfx;

    class VideoPlayer {
    public:
        ~VideoPlayer() { Close(); }

        // path: an .mp4 file on the SD card. Kicks off the worker thread and
        // returns immediately - the video is not decodable yet on return.
        // loop: start over at the end (wallpapers, previews) instead of
        // holding the last frame. audio: also play the file's sound (AAC)
        // through the menu's mixer, as a post-mix so music is untouched.
        // false when the file cannot be opened; later decode errors just
        // leave the picture where it was.
        bool Open(Gfx *gfx, const char *path, bool loop = true, bool audio = false);
        // Call once per menu frame. Uploads a newly-decoded frame to the GPU
        // texture if the worker has produced one since the last Tick().
        void Tick();
        // nullptr until Open has succeeded.
        SDL_Texture *GetTexture() const { return m_tex; }
        void Close();

        // Transport. Positions are seconds from the start of the file.
        void   SetPaused(bool paused);
        bool   Paused() const { return m_paused.load(); }
        void   Seek(double seconds);          // clamped to the file
        double Position() const;
        double Duration() const { return m_duration; }
        bool   Ended() const { return m_ended.load(); }   // non-looping only

    private:
        std::atomic<bool> m_paused{false};
        std::atomic<bool> m_ended{false};
        double m_duration = 0.0;
#ifdef SL_HAS_VIDEO
        static void DecodeTrampoline(void *self);
        void DecodeLoop();

        // Playback clock: wall time since m_clock_tick, plus m_clock_base,
        // frozen while paused. Audio is not the master - it plays as fast as
        // the mixer takes it, and an underrun is paid back by dropping the
        // same amount of audio later (m_audio_debt), so the two stay together.
        double ClockNow() const;
        void   ClockSet(double seconds);
        mutable std::mutex m_clock_mx;
        double m_clock_base = 0.0;
        u64    m_clock_tick = 0;
        double m_pause_at   = 0.0;

        void DoSeek(double seconds);
        void DecodeAudio(AVPacket *pkt, AVFrame *frame);
        static void MixTrampoline(void *self, Uint8 *stream, int len);
        void Mix(Uint8 *stream, int len);

        Gfx *m_gfx = nullptr;
        std::string m_path;
        bool m_loop = true;

        AVFormatContext *m_fmt = nullptr;
        AVCodecContext  *m_dec = nullptr;
        AVCodecContext  *m_adec = nullptr;    // audio, when asked for and present
        AVBufferRef     *m_hw_device_ctx = nullptr;
        int m_stream_idx = -1, m_astream_idx = -1;
        int m_width = 0, m_height = 0;

        Thread m_thread{};
        bool m_thread_running = false;
        std::atomic<bool> m_stop{false};
        std::atomic<bool> m_seek_req{false};
        std::atomic<double> m_seek_to{0.0};

        // Audio ring: interleaved stereo s16 at the mixer's rate, filled by
        // the worker and drained by the mixer's post-mix callback.
        std::mutex m_ring_mx;
        std::vector<int16_t> m_ring;
        size_t m_ring_r = 0, m_ring_n = 0;    // read index, frames held
        size_t m_audio_debt = 0;              // frames the mixer went without
        int    m_mix_rate = 0;
        bool   m_mix_hooked = false;
        double m_rs_pos = 0.0;                // resampler position
        float  m_rs_prev[2] = { 0, 0 };       // last input frame
        double m_skip_until = 0.0;            // drop frames before this (seek)

        // Double-buffered NV12 pixel storage (Y plane + interleaved UV plane),
        // sized to m_width/m_height. The worker fills whichever buffer is not
        // the one currently on the GPU, then flips m_ready_buf; Tick() reads
        // it and uploads if it names a buffer newer than what is on-screen.
        struct Buf { std::vector<uint8_t> y, uv; int y_pitch = 0, uv_pitch = 0; };
        Buf m_buf[2];
        std::atomic<int> m_ready_buf{-1};
        int m_shown_buf = -1;
#endif

        SDL_Texture *m_tex = nullptr;
    };

} // namespace sl::menu::gfx
