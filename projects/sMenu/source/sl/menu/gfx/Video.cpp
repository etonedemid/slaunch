#include <sl/menu/gfx/Video.hpp>
#include <sl/menu/gfx/Gfx.hpp>

#ifdef SL_HAS_VIDEO
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#include <SDL2/SDL_mixer.h>
#include <cstring>
#include <cmath>
#include <deque>
#endif

namespace sl::menu::gfx {

#ifdef SL_HAS_VIDEO

    namespace {
        // NV12 byte sizes for a given coded size - Y is one byte per pixel,
        // UV is one interleaved U,V byte pair per 2x2 block (half width, half
        // height, two bytes per sample).
        size_t YSize(int w, int h)  { return (size_t)w * (size_t)h; }
        size_t UVSize(int w, int h) { return (size_t)(w / 2) * (size_t)(h / 2) * 2; }

        void CopyPlane(uint8_t *dst, int dst_stride,
                       const uint8_t *src, int src_stride,
                       int width_bytes, int height) {
            for (int y = 0; y < height; y++)
                memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, width_bytes);
        }

        // Interleaves two half-resolution planar chroma planes (YUV420P's U
        // and V) into one NV12-style UV plane. There is no swscale in this
        // trimmed FFmpeg build (video wallpapers do not need general-purpose
        // colorspace conversion) - this is the one specific reshuffle needed
        // to feed a software-decoded (YUV420P) frame to the same NV12
        // SDL_Texture the hardware path uses, written by hand instead.
        void InterleaveUV(uint8_t *dst, int dst_stride,
                          const uint8_t *u, int u_stride,
                          const uint8_t *v, int v_stride,
                          int cw, int ch) {
            for (int y = 0; y < ch; y++) {
                uint8_t *row = dst + (size_t)y * dst_stride;
                const uint8_t *ur = u + (size_t)y * u_stride;
                const uint8_t *vr = v + (size_t)y * v_stride;
                for (int x = 0; x < cw; x++) {
                    row[x * 2]     = ur[x];
                    row[x * 2 + 1] = vr[x];
                }
            }
        }
    } // namespace

    bool VideoPlayer::Open(Gfx *gfx, const char *path, bool loop, bool audio) {
        Close();
        if (!gfx || !path || !path[0]) return false;
        m_gfx  = gfx;
        m_path = path;
        m_loop = loop;

        // FFmpeg's avformat layer parses its input string as a URL and takes
        // everything before the first ':' as a protocol scheme - so a plain
        // "sdmc:/..." path (every path in this codebase) reads as protocol
        // "sdmc", which is not registered, and open fails with "Protocol not
        // found". The "file:" prefix forces the "file" protocol, which takes
        // the rest of the string as a literal path - colon included - the
        // same way fopen()/IMG_LoadTexture() already treat it everywhere
        // else in this codebase.
        const std::string url = "file:" + m_path;

        if (avformat_open_input(&m_fmt, url.c_str(), nullptr, nullptr) < 0) { Close(); return false; }
        if (avformat_find_stream_info(m_fmt, nullptr) < 0) { Close(); return false; }

        m_stream_idx = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (m_stream_idx < 0) { Close(); return false; }

        AVStream *stream = m_fmt->streams[m_stream_idx];
        const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) { Close(); return false; }

        m_dec = avcodec_alloc_context3(codec);
        if (!m_dec) { Close(); return false; }
        if (avcodec_parameters_to_context(m_dec, stream->codecpar) < 0) { Close(); return false; }

#ifdef SL_HAS_NVTEGRA
        // Only present when built against averne/FFmpeg's nvtegra branch
        // (the real Switch build - see scripts/build-ffmpeg.sh). The native
        // desktop simulator links the host's stock FFmpeg, which has no such
        // device type, so this block does not exist there at all - not a
        // runtime fallback, a compile-time one. A failure here (e.g. no
        // Tegra hardware, which is only reachable if this somehow ran on
        // non-Switch nvtegra-enabled FFmpeg) is not fatal: m_hw_device_ctx
        // stays null and the decoder falls back to its normal software path.
        av_hwdevice_ctx_create(&m_hw_device_ctx, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0);
        if (m_hw_device_ctx) m_dec->hw_device_ctx = av_buffer_ref(m_hw_device_ctx);
#endif

        if (avcodec_open2(m_dec, codec, nullptr) < 0) { Close(); return false; }

        m_width  = m_dec->width;
        m_height = m_dec->height;
        if (m_width <= 0 || m_height <= 0) { Close(); return false; }

        m_duration = (m_fmt->duration > 0) ? (double)m_fmt->duration / AV_TIME_BASE : 0.0;

        // Sound, when asked for: only if the mixer is open as stereo s16 (what
        // Music::Init opens) and the file has a stream FFmpeg can decode. A
        // build without the AAC decoder just plays the picture.
        int mix_rate = 0, mix_ch = 0; Uint16 mix_fmt = 0;
        if (audio && Mix_QuerySpec(&mix_rate, &mix_fmt, &mix_ch) && mix_ch == 2 &&
            mix_fmt == AUDIO_S16SYS) {
            m_astream_idx = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            const AVCodec *ac = (m_astream_idx >= 0)
                ? avcodec_find_decoder(m_fmt->streams[m_astream_idx]->codecpar->codec_id) : nullptr;
            if (ac && (m_adec = avcodec_alloc_context3(ac)) &&
                avcodec_parameters_to_context(m_adec, m_fmt->streams[m_astream_idx]->codecpar) >= 0 &&
                avcodec_open2(m_adec, ac, nullptr) >= 0) {
                m_mix_rate = mix_rate;
                m_ring.assign((size_t)mix_rate * 2 * 2, 0);   // two seconds, stereo
                m_ring_r = m_ring_n = 0;
                Mix_SetPostMix(&VideoPlayer::MixTrampoline, this);
                m_mix_hooked = true;
            } else {
                if (m_adec) avcodec_free_context(&m_adec);
                m_astream_idx = -1;
            }
        }

        for (auto &b : m_buf) {
            b.y.resize(YSize(m_width, m_height));
            b.uv.resize(UVSize(m_width, m_height));
            b.y_pitch  = m_width;
            b.uv_pitch = m_width; // NV12: chroma row is (w/2) U,V pairs = w bytes
        }

        m_tex = SDL_CreateTexture(m_gfx->Renderer(), SDL_PIXELFORMAT_NV12,
                                  SDL_TEXTUREACCESS_STREAMING, m_width, m_height);
        if (!m_tex) { Close(); return false; }

        m_stop.store(false, std::memory_order_release);
        m_paused.store(false);
        m_ended.store(false);
        m_seek_req.store(false);
        m_skip_until = 0.0;
        ClockSet(0.0);
        if (R_FAILED(threadCreate(&m_thread, &VideoPlayer::DecodeTrampoline, this,
                                  nullptr, 0x40000, 0x3B, -2))) {
            Close();
            return false;
        }
        threadStart(&m_thread);
        m_thread_running = true;
        return true;
    }

    void VideoPlayer::Tick() {
        int ready = m_ready_buf.load(std::memory_order_acquire);
        if (ready < 0 || ready == m_shown_buf || !m_tex) return;
        const Buf &b = m_buf[ready];
        SDL_UpdateNVTexture(m_tex, nullptr, b.y.data(), b.y_pitch, b.uv.data(), b.uv_pitch);
        m_shown_buf = ready;
    }

    void VideoPlayer::Close() {
        m_stop.store(true, std::memory_order_release);
        if (m_thread_running) {
            threadWaitForExit(&m_thread);
            threadClose(&m_thread);
            m_thread_running = false;
        }
        if (m_mix_hooked) {
            // Mix_SetPostMix takes the audio lock, so once it returns the
            // callback is not running and will not run again.
            Mix_SetPostMix(nullptr, nullptr);
            m_mix_hooked = false;
        }
        if (m_adec) { avcodec_free_context(&m_adec); m_adec = nullptr; }
        m_astream_idx = -1;
        m_ring.clear(); m_ring_r = m_ring_n = 0; m_audio_debt = 0;
        m_rs_pos = 0.0; m_rs_prev[0] = m_rs_prev[1] = 0.0f;
        m_duration = 0.0;
        if (m_dec) { avcodec_free_context(&m_dec); m_dec = nullptr; }
        if (m_fmt) { avformat_close_input(&m_fmt); m_fmt = nullptr; }
        if (m_hw_device_ctx) { av_buffer_unref(&m_hw_device_ctx); m_hw_device_ctx = nullptr; }
        if (m_tex) { SDL_DestroyTexture(m_tex); m_tex = nullptr; }
        m_stream_idx = -1;
        m_width = m_height = 0;
        m_ready_buf.store(-1, std::memory_order_release);
        m_shown_buf = -1;
        m_path.clear();
    }

    void VideoPlayer::DecodeTrampoline(void *self) { static_cast<VideoPlayer *>(self)->DecodeLoop(); }

    // ---- clock ----------------------------------------------------------------
    double VideoPlayer::ClockNow() const {
        std::lock_guard<std::mutex> lk(m_clock_mx);
        if (m_paused.load()) return m_pause_at;
        return m_clock_base + (double)(armGetSystemTick() - m_clock_tick)
                            / (double)armGetSystemTickFreq();
    }
    void VideoPlayer::ClockSet(double seconds) {
        std::lock_guard<std::mutex> lk(m_clock_mx);
        m_clock_base = m_pause_at = seconds;
        m_clock_tick = armGetSystemTick();
    }
    void VideoPlayer::SetPaused(bool paused) {
        std::lock_guard<std::mutex> lk(m_clock_mx);
        if (paused == m_paused.load()) return;
        const u64 now = armGetSystemTick();
        if (paused) {
            m_pause_at = m_clock_base + (double)(now - m_clock_tick) / (double)armGetSystemTickFreq();
        } else {
            m_clock_base = m_pause_at;
            m_clock_tick = now;
        }
        m_paused.store(paused);
    }
    double VideoPlayer::Position() const {
        if (!m_fmt) return 0.0;
        const double p = ClockNow();
        return m_duration > 0.0 ? std::min(std::max(p, 0.0), m_duration) : std::max(p, 0.0);
    }
    void VideoPlayer::Seek(double seconds) {
        if (!m_fmt) return;
        if (m_duration > 0.0) seconds = std::min(seconds, m_duration - 0.1);
        m_seek_to.store(std::max(0.0, seconds));
        m_seek_req.store(true);
    }

    // Worker side of a seek: land on the keyframe at or before the target,
    // then decode forward silently (m_skip_until) so the first picture and
    // sound are exactly at it.
    void VideoPlayer::DoSeek(double seconds) {
        const AVStream *vs = m_fmt->streams[m_stream_idx];
        const int64_t start = (vs->start_time != AV_NOPTS_VALUE) ? vs->start_time : 0;
        const int64_t ts = start + (int64_t)(seconds / av_q2d(vs->time_base));
        av_seek_frame(m_fmt, m_stream_idx, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(m_dec);
        if (m_adec) avcodec_flush_buffers(m_adec);
        {
            std::lock_guard<std::mutex> lk(m_ring_mx);
            m_ring_r = m_ring_n = 0;
            m_audio_debt = 0;
        }
        m_rs_pos = 0.0;
        m_skip_until = seconds;
        m_ended.store(false);
        ClockSet(seconds);
    }

    // ---- audio ------------------------------------------------------------------
    // Decoded audio in, stereo s16 at the mixer's rate into the ring. There is
    // no swresample in this FFmpeg build, so the two conversions it would do
    // are done here: planar/packed float or s16 to stereo float, then a
    // linear resample (Switch clips are 48 kHz, the mixer runs at 44.1).
    void VideoPlayer::DecodeAudio(AVPacket *pkt, AVFrame *frame) {
        if (avcodec_send_packet(m_adec, pkt) < 0) return;
        const AVStream *as = m_fmt->streams[m_astream_idx];
        while (!m_stop.load() && !m_seek_req.load() &&
               avcodec_receive_frame(m_adec, frame) == 0) {
            const int n  = frame->nb_samples;
            const int ch = frame->ch_layout.nb_channels;
            const int rate = frame->sample_rate > 0 ? frame->sample_rate : m_mix_rate;
            const double pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? (frame->best_effort_timestamp - (as->start_time != AV_NOPTS_VALUE ? as->start_time : 0))
                  * av_q2d(as->time_base) : m_skip_until;
            if (n <= 0 || ch <= 0 || pts + (double)n / rate < m_skip_until) { av_frame_unref(frame); continue; }

            auto sample = [&](int c, int i) -> float {
                c = std::min(c, ch - 1);
                switch (frame->format) {
                    case AV_SAMPLE_FMT_FLTP: return ((const float *)frame->data[c])[i];
                    case AV_SAMPLE_FMT_FLT:  return ((const float *)frame->data[0])[i * ch + c];
                    case AV_SAMPLE_FMT_S16P: return ((const int16_t *)frame->data[c])[i] / 32768.0f;
                    case AV_SAMPLE_FMT_S16:  return ((const int16_t *)frame->data[0])[i * ch + c] / 32768.0f;
                    default:                 return 0.0f;
                }
            };

            const double step = (double)rate / (double)m_mix_rate;
            std::vector<int16_t> out;
            out.reserve((size_t)(n / step + 2) * 2);
            // Position runs over [prev, in0 .. in(n-1)]: index 0 is the last
            // frame of the previous packet, so the resample is seamless.
            for (; m_rs_pos + 1.0 < (double)n + 1.0; m_rs_pos += step) {
                const int   i = (int)m_rs_pos;
                const float f = (float)(m_rs_pos - i);
                for (int c = 0; c < 2; c++) {
                    const float a = (i == 0) ? m_rs_prev[c] : sample(c, i - 1);
                    const float b = sample(c, i);
                    const float v = std::min(1.0f, std::max(-1.0f, a + (b - a) * f));
                    out.push_back((int16_t)lrintf(v * 32767.0f));
                }
            }
            m_rs_pos -= (double)n;
            m_rs_prev[0] = sample(0, n - 1);
            m_rs_prev[1] = sample(1, n - 1);
            av_frame_unref(frame);

            size_t frames = out.size() / 2, off = 0;
            const size_t cap = m_ring.size() / 2;
            while (frames > 0 && !m_stop.load() && !m_seek_req.load()) {
                std::unique_lock<std::mutex> lk(m_ring_mx);
                // Pay back an underrun by skipping what the mixer missed.
                const size_t skip = std::min(frames, m_audio_debt);
                m_audio_debt -= skip; off += skip; frames -= skip;
                const size_t room = std::min(frames, cap - m_ring_n);
                for (size_t k = 0; k < room; k++) {
                    const size_t w = ((m_ring_r + m_ring_n + k) % cap) * 2;
                    m_ring[w]     = out[(off + k) * 2];
                    m_ring[w + 1] = out[(off + k) * 2 + 1];
                }
                m_ring_n += room; off += room; frames -= room;
                lk.unlock();
                if (frames > 0) svcSleepThread(5'000'000);   // ring full: let it drain
            }
        }
    }

    void VideoPlayer::MixTrampoline(void *self, Uint8 *stream, int len) {
        static_cast<VideoPlayer *>(self)->Mix(stream, len);
    }
    // Mixer thread: add the clip's sound on top of whatever else is playing.
    void VideoPlayer::Mix(Uint8 *stream, int len) {
        if (m_paused.load() || m_ring.empty()) return;
        int16_t *dst = (int16_t *)stream;
        const size_t want = (size_t)len / 4, cap = m_ring.size() / 2;
        std::lock_guard<std::mutex> lk(m_ring_mx);
        const size_t got = std::min(want, m_ring_n);
        for (size_t k = 0; k < got; k++) {
            const size_t r = ((m_ring_r + k) % cap) * 2;
            for (int c = 0; c < 2; c++) {
                const int v = dst[k * 2 + c] + m_ring[r + c];
                dst[k * 2 + c] = (int16_t)std::min(32767, std::max(-32768, v));
            }
        }
        m_ring_r = (m_ring_r + got) % cap;
        m_ring_n -= got;
        // Only a real underrun counts as debt - running dry after the last
        // packet is simply the end of the clip.
        if (got < want && !m_ended.load()) m_audio_debt += want - got;
    }

    // ---- worker -----------------------------------------------------------------
    // Demuxing runs ahead of presentation. Packets are read until about
    // 0.6 s of sound is buffered (or, with no sound, a few video packets are
    // queued): sound is decoded as soon as it is read, video packets wait in
    // a queue and are decoded only when the picture before them has been
    // shown. Reading one packet per shown frame, as this used to, kept the
    // sound only a frame or two ahead of the mixer - which takes it about a
    // tenth of a second at a time - so it kept running dry: choppy audio.
    void VideoPlayer::DecodeLoop() {
        AVStream *stream = m_fmt->streams[m_stream_idx];
        const int64_t vstart = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;
        const double  vtb    = av_q2d(stream->time_base);

        AVPacket *pkt       = av_packet_alloc();
        AVFrame  *dec_frame = av_frame_alloc();
        AVFrame  *sw_frame  = av_frame_alloc();
        AVFrame  *a_frame   = av_frame_alloc();
        std::deque<AVPacket *> vq;          // video packets read but not decoded
        bool   read_eof  = false;           // demuxer is done
        bool   dec_eof   = false;           // decoder has handed back its last frame
        bool   drain_sent = false;          // null packet sent to flush the decoder
        bool   have_frame = false;          // dec_frame holds the next picture
        double frame_pts  = 0.0;
        int    write_buf  = 0;

        auto clear_queue = [&]() {
            for (AVPacket *q : vq) av_packet_free(&q);
            vq.clear();
            if (have_frame) { av_frame_unref(dec_frame); have_frame = false; }
            read_eof = dec_eof = drain_sent = false;
        };
        auto audio_buffered = [&]() -> double {
            if (!m_adec || m_mix_rate <= 0) return 1e9;
            std::lock_guard<std::mutex> lk(m_ring_mx);
            return (double)m_ring_n / m_mix_rate;
        };

        while (pkt && dec_frame && sw_frame && a_frame && !m_stop.load(std::memory_order_acquire)) {
            if (m_seek_req.exchange(false)) { clear_queue(); DoSeek(m_seek_to.load()); }

            // ---- read ahead -------------------------------------------------
            while (!read_eof && !m_stop.load() && !m_seek_req.load() &&
                   vq.size() < 240 &&
                   (audio_buffered() < 0.6 || (vq.size() < 6 && !have_frame))) {
                if (av_read_frame(m_fmt, pkt) < 0) { read_eof = true; break; }
                if (pkt->stream_index == m_astream_idx && m_adec) {
                    DecodeAudio(pkt, a_frame);
                    av_packet_unref(pkt);
                } else if (pkt->stream_index == m_stream_idx) {
                    AVPacket *q = av_packet_alloc();
                    if (q) { av_packet_move_ref(q, pkt); vq.push_back(q); }
                    else   av_packet_unref(pkt);
                } else {
                    av_packet_unref(pkt);
                }
            }

            // ---- next picture -----------------------------------------------
            while (!have_frame && !dec_eof && !m_stop.load() && !m_seek_req.load()) {
                const int r = avcodec_receive_frame(m_dec, dec_frame);
                if (r == 0) {
                    frame_pts = (dec_frame->best_effort_timestamp != AV_NOPTS_VALUE)
                        ? (dec_frame->best_effort_timestamp - vstart) * vtb : ClockNow();
                    if (frame_pts < m_skip_until - 0.001) { av_frame_unref(dec_frame); continue; }
                    have_frame = true;
                } else if (r == AVERROR_EOF) {
                    dec_eof = true;
                } else if (!vq.empty()) {                 // EAGAIN: feed it
                    AVPacket *q = vq.front(); vq.pop_front();
                    avcodec_send_packet(m_dec, q);
                    av_packet_free(&q);
                } else if (read_eof && !drain_sent) {     // flush out the last frames
                    avcodec_send_packet(m_dec, nullptr);
                    drain_sent = true;
                } else {
                    break;                                 // wait for more packets
                }
            }

            if (have_frame) {
                if (ClockNow() < frame_pts) { svcSleepThread(2'000'000); continue; }

                AVFrame *src = dec_frame;
#ifdef SL_HAS_NVTEGRA
                if (dec_frame->format == AV_PIX_FMT_NVTEGRA) {
                    sw_frame->format = AV_PIX_FMT_NV12;
                    src = (av_hwframe_transfer_data(sw_frame, dec_frame, 0) == 0) ? sw_frame : nullptr;
                }
#endif
                if (src) {
                    Buf &b = m_buf[write_buf];
                    if (src->format == AV_PIX_FMT_NV12) {
                        CopyPlane(b.y.data(), b.y_pitch, src->data[0], src->linesize[0], m_width, m_height);
                        CopyPlane(b.uv.data(), b.uv_pitch, src->data[1], src->linesize[1], m_width, m_height / 2);
                        m_ready_buf.store(write_buf, std::memory_order_release);
                        write_buf ^= 1;
                    } else if (src->format == AV_PIX_FMT_YUV420P) {
                        CopyPlane(b.y.data(), b.y_pitch, src->data[0], src->linesize[0], m_width, m_height);
                        InterleaveUV(b.uv.data(), b.uv_pitch, src->data[1], src->linesize[1],
                                     src->data[2], src->linesize[2], m_width / 2, m_height / 2);
                        m_ready_buf.store(write_buf, std::memory_order_release);
                        write_buf ^= 1;
                    }
                    // Any other pixel format (e.g. 10-bit) is dropped - out of
                    // scope (8-bit H.264 only).
                }
                av_frame_unref(dec_frame);
                av_frame_unref(sw_frame);
                have_frame = false;
                continue;
            }

            // Nothing left to show: loop, or hold the last frame until the
            // clock reaches the end.
            if (read_eof && vq.empty() && (dec_eof || drain_sent)) {
                if (dec_eof && m_loop) { clear_queue(); DoSeek(0.0); continue; }
                if (ClockNow() >= m_duration - 0.05) m_ended.store(true);
            }
            svcSleepThread(5'000'000);
        }

        for (AVPacket *q : vq) av_packet_free(&q);
        av_frame_free(&a_frame);
        av_frame_free(&sw_frame);
        av_frame_free(&dec_frame);
        av_packet_free(&pkt);
    }

#else // !SL_HAS_VIDEO - stub for builds with no FFmpeg (e.g. the Windows
      // simulator, which does not stage FFmpeg for MinGW; see
      // projects/sMenu/sim/Makefile).

    bool VideoPlayer::Open(Gfx *, const char *, bool, bool) { return false; }
    void VideoPlayer::Tick() {}
    void VideoPlayer::Close() {}
    void VideoPlayer::SetPaused(bool p) { m_paused.store(p); }
    void VideoPlayer::Seek(double) {}
    double VideoPlayer::Position() const { return 0.0; }

#endif

} // namespace sl::menu::gfx
