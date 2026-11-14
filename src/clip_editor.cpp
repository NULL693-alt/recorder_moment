// ============================================================================
// clip_editor.cpp — recorte sin recodificar + reproductor de vista previa
// (ver clip_editor.h)
// ============================================================================
#include "clip_editor.h"
#include "game_detect.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string av_err(int e) {
    char buf[128];
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

static void set_err(std::string *err, const std::string &m) {
    if (err) *err = m;
}

static int64_t stream_start(const AVStream *st) {
    return st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
}

static bool open_input(const std::string &path, AVFormatContext **ic, std::string *err) {
    *ic = nullptr;
    int r = avformat_open_input(ic, path.c_str(), nullptr, nullptr);
    if (r < 0) {
        set_err(err, "No se pudo abrir el clip (" + av_err(r) + ")");
        return false;
    }
    r = avformat_find_stream_info(*ic, nullptr);
    if (r < 0) {
        avformat_close_input(ic);
        set_err(err, "El clip esta danado (" + av_err(r) + ")");
        return false;
    }
    return true;
}

static void fill_info(AVFormatContext *ic, int vidx, ClipInfo &info) {
    AVStream *vs = ic->streams[vidx];
    const double tb = av_q2d(vs->time_base);
    const int64_t s0 = stream_start(vs);

    info = ClipInfo();
    info.width = vs->codecpar->width;
    info.height = vs->codecpar->height;
    info.fps = vs->avg_frame_rate.num > 0 ? av_q2d(vs->avg_frame_rate) : 0.0;
    info.video_codec = avcodec_get_name(vs->codecpar->codec_id);
    if (vs->duration > 0)
        info.duration = vs->duration * tb;
    else if (ic->duration > 0)
        info.duration = ic->duration / (double)AV_TIME_BASE;

    // El demuxer de MP4 ya tiene el indice entero (todas las muestras con su
    // flag de keyframe), asi que no hace falta leer el archivo.
    int n = avformat_index_get_entries_count(vs);
    for (int i = 0; i < n; i++) {
        const AVIndexEntry *e = avformat_index_get_entry(vs, i);
        if (!e || !(e->flags & AVINDEX_KEYFRAME)) continue;
        info.keyframes.push_back(std::max(0.0, (e->timestamp - s0) * tb));
    }
    std::sort(info.keyframes.begin(), info.keyframes.end());
    info.keyframes.erase(std::unique(info.keyframes.begin(), info.keyframes.end(),
                                     [](double a, double b) { return std::fabs(a - b) < 1e-4; }),
                         info.keyframes.end());
    if (info.keyframes.empty() || info.keyframes[0] > 0.001)
        info.keyframes.insert(info.keyframes.begin(), 0.0);

    for (unsigned i = 0; i < ic->nb_streams; i++)
        if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) info.audio_tracks++;
}

bool probe_clip(const std::string &path, ClipInfo &info, std::string *err) {
    AVFormatContext *ic = nullptr;
    if (!open_input(path, &ic, err)) return false;
    int vidx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0) {
        avformat_close_input(&ic);
        set_err(err, "El archivo no tiene video");
        return false;
    }
    fill_info(ic, vidx, info);
    avformat_close_input(&ic);
    return true;
}

double keyframe_at_or_before(const ClipInfo &info, double t) {
    double best = 0.0;
    for (double k : info.keyframes) {
        if (k <= t + 1e-3)
            best = k;
        else
            break;
    }
    return best;
}

std::string make_trim_path(const std::string &in) {
    fs::path p = fs::u8path(in);
    fs::path dir = p.parent_path();
    std::string stem = p.stem().u8string();
    std::error_code ec;
    for (int i = 1; i < 1000; i++) {
        std::string name = stem + "_recorte" + (i > 1 ? "_" + std::to_string(i) : "") + ".mp4";
        fs::path c = dir / fs::u8path(name);
        if (!fs::exists(c, ec)) return c.u8string();
    }
    return (dir / fs::u8path(stem + "_recorte_nuevo.mp4")).u8string();
}

static std::wstring lower_w(std::wstring s) {
    for (wchar_t &c : s) c = (wchar_t)towlower(c);
    return s;
}

std::string edited_clip_path(const std::string &clip, const std::string &clips_dir) {
    std::error_code ec;
    fs::path c = fs::absolute(fs::u8path(clip), ec).lexically_normal();
    fs::path root = fs::absolute(fs::u8path(clips_dir.empty() ? "." : clips_dir), ec)
                        .lexically_normal();
    const fs::path edited = fs::u8path(kEditedFolder);
    fs::path dest = root / edited;

    // ¿El clip esta dentro de la carpeta de clips? (Windows no distingue
    // mayusculas, asi que comparamos en minusculas.)
    std::wstring cl = lower_w(c.parent_path().wstring()), rl = lower_w(root.wstring());
    while (!rl.empty() && (rl.back() == L'\\' || rl.back() == L'/')) rl.pop_back();
    if (cl.size() > rl.size() && cl.compare(0, rl.size(), rl) == 0 &&
        (cl[rl.size()] == L'\\' || cl[rl.size()] == L'/')) {
        fs::path rel = c.parent_path().lexically_relative(root);
        fs::path first = *rel.begin();
        if (lower_w(first.wstring()) == lower_w(edited.wstring()))
            dest = c.parent_path(); // ya era un recorte: queda donde estaba
        else
            dest = root / edited / first; // la carpeta del juego
    }

    std::string stem = c.stem().u8string();
    for (int i = 1; i < 1000; i++) {
        std::string name = stem + (i > 1 ? "_" + std::to_string(i) : "") + ".mp4";
        fs::path p = dest / fs::u8path(name);
        if (!fs::exists(p, ec)) return p.u8string();
    }
    return (dest / fs::u8path(stem + "_nuevo.mp4")).u8string();
}

// ---------------------------------------------------------------------------
// Recorte
// ---------------------------------------------------------------------------
bool trim_clip(const std::string &in, const std::string &out, double start, double end,
               std::string *err, double *out_duration) {
    std::error_code ec;
    if (fs::equivalent(fs::u8path(in), fs::u8path(out), ec)) {
        set_err(err, "El recorte no puede pisar el clip original");
        return false;
    }
    fs::path out_dir = fs::u8path(out).parent_path();
    if (!out_dir.empty()) fs::create_directories(out_dir, ec);

    AVFormatContext *ic = nullptr;
    if (!open_input(in, &ic, err)) return false;
    int vidx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0) {
        avformat_close_input(&ic);
        set_err(err, "El archivo no tiene video");
        return false;
    }
    AVStream *vs = ic->streams[vidx];
    ClipInfo info;
    fill_info(ic, vidx, info);

    if (start < 0.0) start = 0.0;
    if (end <= 0.0 || (info.duration > 0.0 && end > info.duration)) end = info.duration;
    if (end - start < 0.1) {
        avformat_close_input(&ic);
        set_err(err, "El recorte es demasiado corto");
        return false;
    }

    const double kstart = keyframe_at_or_before(info, start);
    const int64_t s0 = stream_start(vs);
    const int64_t seek_ts = s0 + (int64_t)std::llround((kstart + 0.001) / av_q2d(vs->time_base));
    const int64_t end_us =
        av_rescale_q(s0, vs->time_base, AV_TIME_BASE_Q) + (int64_t)std::llround(end * 1e6);

    // Salida: mismas pistas (video + todos los audios), mismos nombres.
    AVFormatContext *oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mp4", out.c_str()) < 0 || !oc) {
        avformat_close_input(&ic);
        set_err(err, "No se pudo crear el archivo de salida");
        return false;
    }
    std::vector<int> map(ic->nb_streams, -1);
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        bool video = (int)i == vidx;
        bool audio = st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
        if (!video && !audio) continue;
        AVStream *os = avformat_new_stream(oc, nullptr);
        if (!os || avcodec_parameters_copy(os->codecpar, st->codecpar) < 0) continue;
        os->codecpar->codec_tag = 0;
        os->time_base = st->time_base;
        av_dict_copy(&os->metadata, st->metadata, 0);
        os->disposition = st->disposition;
        map[i] = os->index;
    }

    bool ok = true;
    std::string why;
    bool header = false;
    int r = avio_open(&oc->pb, out.c_str(), AVIO_FLAG_WRITE);
    if (r < 0) {
        ok = false;
        why = "No se pudo escribir " + out + " (" + av_err(r) + ")";
    } else {
        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "movflags", "+faststart", 0);
        r = avformat_write_header(oc, &opts);
        av_dict_free(&opts);
        if (r < 0) {
            ok = false;
            why = "No se pudo escribir el encabezado (" + av_err(r) + ")";
        } else {
            header = true;
        }
    }

    int64_t off_us = AV_NOPTS_VALUE;   // tiempo del primer keyframe (pasa a ser el 0)
    int64_t last_video_end_us = 0;
    int video_written = 0;
    std::vector<AVPacket *> pending;   // audio leido antes del primer keyframe
    std::vector<char> done(ic->nb_streams, 0);

    auto write_pkt = [&](AVPacket *p) -> bool {
        AVStream *st = ic->streams[p->stream_index];
        AVStream *os = oc->streams[map[p->stream_index]];
        int64_t off = av_rescale_q(off_us, AV_TIME_BASE_Q, st->time_base);
        if (p->pts != AV_NOPTS_VALUE) p->pts -= off;
        if (p->dts != AV_NOPTS_VALUE) p->dts -= off;
        av_packet_rescale_ts(p, st->time_base, os->time_base);
        p->stream_index = os->index;
        p->pos = -1;
        return av_interleaved_write_frame(oc, p) >= 0;
    };
    auto all_done = [&]() {
        for (unsigned i = 0; i < ic->nb_streams; i++)
            if (map[i] >= 0 && !done[i]) return false;
        return true;
    };

    AVPacket *pkt = av_packet_alloc();
    if (ok) {
        av_seek_frame(ic, vidx, seek_ts, AVSEEK_FLAG_BACKWARD);
        while (ok && av_read_frame(ic, pkt) >= 0) {
            const int idx = pkt->stream_index;
            if (idx < 0 || idx >= (int)ic->nb_streams || map[idx] < 0 ||
                pkt->pts == AV_NOPTS_VALUE || done[idx]) {
                av_packet_unref(pkt);
                continue;
            }
            AVStream *st = ic->streams[idx];
            const bool video = idx == vidx;
            const int64_t pts_us = av_rescale_q(pkt->pts, st->time_base, AV_TIME_BASE_Q);

            if (off_us == AV_NOPTS_VALUE) {
                // Hasta encontrar el keyframe: el audio se guarda aparte (puede
                // venir antes en el archivo) y el video que no es key se tira.
                if (!video) {
                    pending.push_back(av_packet_clone(pkt));
                    av_packet_unref(pkt);
                    continue;
                }
                if (!(pkt->flags & AV_PKT_FLAG_KEY)) {
                    av_packet_unref(pkt);
                    continue;
                }
                off_us = pts_us;
                for (AVPacket *a : pending) {
                    int64_t a_us = av_rescale_q(a->pts, ic->streams[a->stream_index]->time_base,
                                                AV_TIME_BASE_Q);
                    if (ok && a_us >= off_us && a_us < end_us && !write_pkt(a)) ok = false;
                    av_packet_free(&a);
                }
                pending.clear();
            }

            if (video) {
                int64_t dts_us = pkt->dts != AV_NOPTS_VALUE
                                     ? av_rescale_q(pkt->dts, st->time_base, AV_TIME_BASE_Q)
                                     : pts_us;
                if (dts_us >= end_us) done[idx] = 1;
            } else {
                if (pts_us < off_us) {
                    av_packet_unref(pkt);
                    continue;
                }
                if (pts_us >= end_us) done[idx] = 1;
            }
            if (done[idx]) {
                av_packet_unref(pkt);
                if (all_done()) break;
                continue;
            }

            if (video) {
                int64_t dur_us =
                    pkt->duration > 0 ? av_rescale_q(pkt->duration, st->time_base, AV_TIME_BASE_Q) : 0;
                last_video_end_us = std::max(last_video_end_us, pts_us + dur_us);
                video_written++;
            }
            if (!write_pkt(pkt)) {
                ok = false;
                why = "Fallo al escribir el recorte (¿disco lleno?)";
            }
        }
    }
    for (AVPacket *a : pending) av_packet_free(&a);
    av_packet_free(&pkt);

    if (header && av_write_trailer(oc) < 0 && ok) {
        ok = false;
        why = "No se pudo terminar el archivo";
    }
    if (oc->pb) avio_closep(&oc->pb);
    avformat_free_context(oc);
    avformat_close_input(&ic);

    if (ok && video_written == 0) {
        ok = false;
        why = "No hay video en ese tramo";
    }
    if (!ok) {
        fs::remove(fs::u8path(out), ec);
        set_err(err, why);
        return false;
    }
    if (out_duration) *out_duration = (last_video_end_us - off_us) / 1e6;
    return true;
}

// ---------------------------------------------------------------------------
// Reproductor
// ---------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

static constexpr int kAudioRate = 48000;

struct ClipPlayer::Impl {
    // --- solo el hilo de decodificacion (y open/close) ---
    AVFormatContext *ic = nullptr;
    int vidx = -1, aidx = -1;
    AVCodecContext *vdec = nullptr, *adec = nullptr;
    SwsContext *sws = nullptr;
    int sws_fmt = -1, sws_w = 0, sws_h = 0, sws_ow = 0, sws_oh = 0;
    SwrContext *swr = nullptr;
    AVFrame *vframe = nullptr, *prev = nullptr, *aframe = nullptr;
    AVPacket *pkt = nullptr;
    int max_w = 1280, max_h = 720;
    double vtb = 0, atb = 0, origin = 0; // origin = inicio del video, en segundos
    bool demux_eof = false, vdec_flushed = false, at_end = false;
    bool clock_running = false;
    Clock::time_point clock_origin;
    double pos_origin = 0;
    std::vector<uint8_t> scratch;
    std::vector<float> abuf;
    ClipInfo info;

    // --- compartido con la ventana ---
    std::mutex m;
    std::condition_variable cv;
    bool quit = false, want_play = false, seek_pending = false;
    double seek_to = 0, stop_at = -1;
    std::atomic<double> pos{0.0};
    std::thread th;

    std::mutex fm; // ultimo cuadro
    std::vector<uint8_t> frame;
    int fw = 0, fh = 0;
    uint64_t fver = 0, ftaken = 0;

    // --- audio (salida por WASAPI) ---
    std::mutex am;
    std::deque<float> ring;    // estereo intercalado, 48 kHz
    double ring_end_t = -1.0;  // tiempo de la muestra que sigue al final del ring
    uint64_t agen = 0;         // cambia en cada salto: hay que vaciar la placa
    bool audio_run = false;
    std::atomic<bool> audio_ok{false}, aquit{false};
    std::thread ath;

    ~Impl() {
        if (vdec) avcodec_free_context(&vdec);
        if (adec) avcodec_free_context(&adec);
        if (sws) sws_freeContext(sws);
        if (swr) swr_free(&swr);
        if (vframe) av_frame_free(&vframe);
        if (prev) av_frame_free(&prev);
        if (aframe) av_frame_free(&aframe);
        if (pkt) av_packet_free(&pkt);
        if (ic) avformat_close_input(&ic);
    }

    // Convierte el cuadro a BGRA chico y lo deja para la ventana.
    void publish(AVFrame *f) {
        if (!sws || f->format != sws_fmt || f->width != sws_w || f->height != sws_h) {
            if (sws) sws_freeContext(sws);
            double s = std::min(1.0, std::min(max_w / (double)f->width, max_h / (double)f->height));
            sws_ow = std::max(2, (int)(f->width * s) & ~1);
            sws_oh = std::max(2, (int)(f->height * s) & ~1);
            sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, sws_ow, sws_oh,
                                 AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            sws_fmt = f->format;
            sws_w = f->width;
            sws_h = f->height;
            if (!sws) return;
            int cs = SWS_CS_ITU709;
            if (f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M ||
                (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height < 720))
                cs = SWS_CS_ITU601;
            int src_full = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
            sws_setColorspaceDetails(sws, sws_getCoefficients(cs), src_full,
                                     sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        }
        if (!sws) return;
        scratch.resize((size_t)sws_ow * sws_oh * 4);
        uint8_t *dst[4] = {scratch.data(), nullptr, nullptr, nullptr};
        int dls[4] = {sws_ow * 4, 0, 0, 0};
        sws_scale(sws, f->data, f->linesize, 0, f->height, dst, dls);
        std::lock_guard<std::mutex> lk(fm);
        frame.swap(scratch);
        fw = sws_ow;
        fh = sws_oh;
        fver++;
    }

    double frame_time(AVFrame *f, double tb) const {
        int64_t ts = f->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = f->pts;
        if (ts == AV_NOPTS_VALUE) return -1.0;
        return ts * tb - origin;
    }

    void decode_audio(AVPacket *p) {
        if (avcodec_send_packet(adec, p) < 0) return;
        while (avcodec_receive_frame(adec, aframe) == 0) {
            double t = frame_time(aframe, atb);
            if (!swr) {
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, kAudioRate,
                                        &aframe->ch_layout, (AVSampleFormat)aframe->format,
                                        aframe->sample_rate, 0, nullptr) < 0 ||
                    swr_init(swr) < 0) {
                    if (swr) swr_free(&swr);
                    av_frame_unref(aframe);
                    return;
                }
            }
            int max_out = swr_get_out_samples(swr, aframe->nb_samples);
            if (max_out <= 0) {
                av_frame_unref(aframe);
                continue;
            }
            abuf.resize((size_t)max_out * 2);
            uint8_t *outp = (uint8_t *)abuf.data();
            int n = swr_convert(swr, &outp, max_out, (const uint8_t **)aframe->extended_data,
                                aframe->nb_samples);
            av_frame_unref(aframe);
            if (n <= 0) continue;
            std::lock_guard<std::mutex> lk(am);
            if (ring_end_t < 0.0) ring_end_t = t >= 0.0 ? t : 0.0;
            ring.insert(ring.end(), abuf.begin(), abuf.begin() + (size_t)n * 2);
            ring_end_t += n / (double)kAudioRate;
        }
    }

    // Siguiente cuadro de video (en vframe). Lee paquetes y, de paso, decodifica
    // el audio que va apareciendo. false = se termino el clip.
    bool decode_video(double &ft) {
        for (;;) {
            int r = avcodec_receive_frame(vdec, vframe);
            if (r == 0) {
                ft = frame_time(vframe, vtb);
                return true;
            }
            if (r != AVERROR(EAGAIN)) return false;
            if (demux_eof) {
                if (vdec_flushed) return false;
                avcodec_send_packet(vdec, nullptr);
                vdec_flushed = true;
                continue;
            }
            if (av_read_frame(ic, pkt) < 0) {
                demux_eof = true;
                continue;
            }
            if (pkt->stream_index == vidx)
                avcodec_send_packet(vdec, pkt);
            else if (pkt->stream_index == aidx && adec)
                decode_audio(pkt);
            av_packet_unref(pkt);
        }
    }

    // Salta a t: busca el keyframe anterior y decodifica hasta t, asi se ve el
    // cuadro exacto.
    void do_seek(double t) {
        t = std::max(0.0, std::min(t, info.duration));
        int64_t ts = (int64_t)std::llround((origin + t) / vtb);
        av_seek_frame(ic, vidx, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(vdec);
        if (adec) avcodec_flush_buffers(adec);
        if (swr) swr_free(&swr);
        demux_eof = vdec_flushed = at_end = false;
        clock_running = false;
        {
            std::lock_guard<std::mutex> lk(am);
            ring.clear();
            ring_end_t = -1.0;
            agen++;
        }

        const double half = info.fps > 0 ? 0.5 / info.fps : 0.008;
        av_frame_unref(prev);
        double prev_t = -1.0, shown = t;
        for (;;) {
            double f = 0;
            if (!decode_video(f)) {
                at_end = true;
                if (prev->data[0]) {
                    publish(prev);
                    shown = prev_t;
                }
                break;
            }
            if (f >= t - half) {
                publish(vframe);
                shown = f;
                break;
            }
            av_frame_unref(prev);
            av_frame_move_ref(prev, vframe);
            prev_t = f;
        }
        if (shown < 0) shown = t;
        pos.store(shown);

        // El audio decodificado de antes del cuadro que se ve, afuera.
        std::lock_guard<std::mutex> lk(am);
        if (ring_end_t >= 0.0) {
            double ring_t0 = ring_end_t - ring.size() / 2 / (double)kAudioRate;
            long drop = (long)std::llround((shown - ring_t0) * kAudioRate) * 2;
            if (drop > 0) {
                if ((size_t)drop >= ring.size())
                    ring.clear();
                else
                    ring.erase(ring.begin(), ring.begin() + drop);
            }
        }
    }

    void set_audio_run(bool on) {
        std::lock_guard<std::mutex> lk(am);
        audio_run = on;
    }

    void run() {
        do_seek(0.0);
        for (;;) {
            std::unique_lock<std::mutex> lk(m);
            if (quit) break;
            if (seek_pending) {
                double t = seek_to;
                seek_pending = false;
                lk.unlock();
                do_seek(t);
                continue;
            }
            if (!want_play) {
                if (clock_running) {
                    clock_running = false;
                    set_audio_run(false);
                }
                cv.wait(lk);
                continue;
            }
            lk.unlock();

            if (!clock_running) {
                if (at_end) do_seek(0.0);
                clock_origin = Clock::now();
                pos_origin = pos.load();
                clock_running = true;
                set_audio_run(true);
            }

            double ft = 0;
            if (!decode_video(ft)) { // fin del clip
                at_end = true;
                lk.lock();
                want_play = false;
                stop_at = -1;
                continue;
            }
            Clock::time_point due =
                clock_origin + std::chrono::microseconds((int64_t)((ft - pos_origin) * 1e6));
            if (Clock::now() > due + std::chrono::milliseconds(300)) { // muy atrasados
                clock_origin = Clock::now();
                pos_origin = ft;
                due = clock_origin;
            }

            lk.lock();
            cv.wait_until(lk, due, [&] { return quit || seek_pending || !want_play; });
            if (quit) break;
            if (seek_pending) continue;
            double stop = stop_at;
            lk.unlock();

            publish(vframe);
            pos.store(ft);
            if (stop >= 0.0 && ft >= stop) {
                lk.lock();
                want_play = false;
                stop_at = -1;
            }
        }
        set_audio_run(false);
    }

    // Salida de audio: WASAPI compartido, 48 kHz estereo float (Windows
    // convierte al formato de la placa).
    void audio_main() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool com = SUCCEEDED(hr);
        IMMDeviceEnumerator *en = nullptr;
        IMMDevice *dev = nullptr;
        IAudioClient *ac = nullptr;
        IAudioRenderClient *rc = nullptr;
        UINT32 buf_frames = 0;

        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kAudioRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kAudioRate * 8;

        bool ready =
            SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), (void **)&en)) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
            SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&ac)) &&
            SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                         AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                     2000000, 0, &wf, nullptr)) &&
            SUCCEEDED(ac->GetService(__uuidof(IAudioRenderClient), (void **)&rc)) &&
            SUCCEEDED(ac->GetBufferSize(&buf_frames));
        audio_ok.store(ready);

        bool started = false;
        uint64_t seen_gen = 0;
        while (ready && !aquit.load()) {
            {
                std::lock_guard<std::mutex> lk(am);
                if (agen != seen_gen) { // salto: lo que quedo en la placa ya no va
                    if (started) ac->Stop();
                    started = false;
                    ac->Reset();
                    seen_gen = agen;
                }
                if (audio_run) {
                    UINT32 pad = 0;
                    if (SUCCEEDED(ac->GetCurrentPadding(&pad)) && pad < buf_frames) {
                        UINT32 n = std::min<UINT32>(buf_frames - pad, (UINT32)(ring.size() / 2));
                        BYTE *data = nullptr;
                        if (n > 0 && SUCCEEDED(rc->GetBuffer(n, &data))) {
                            float *out = (float *)data;
                            std::copy(ring.begin(), ring.begin() + (size_t)n * 2, out);
                            ring.erase(ring.begin(), ring.begin() + (size_t)n * 2);
                            rc->ReleaseBuffer(n, 0);
                        }
                    }
                    if (!started && SUCCEEDED(ac->Start())) started = true;
                } else if (started) {
                    ac->Stop(); // pausa: lo que esta en la placa queda para seguir
                    started = false;
                }
            }
            Sleep(5);
        }
        if (ac && started) ac->Stop();
        if (rc) rc->Release();
        if (ac) ac->Release();
        if (dev) dev->Release();
        if (en) en->Release();
        if (com) CoUninitialize();
    }
};

ClipPlayer::ClipPlayer() = default;

ClipPlayer::~ClipPlayer() { close(); }

static const AVCodec *find_decoder(AVCodecID id) {
    // Para AV1 preferimos dav1d (rapido y por software, anda en cualquier PC).
    if (id == AV_CODEC_ID_AV1) {
        if (const AVCodec *c = avcodec_find_decoder_by_name("libdav1d")) return c;
    }
    return avcodec_find_decoder(id);
}

bool ClipPlayer::open(const std::string &path, int max_w, int max_h, std::string *err) {
    close();
    std::unique_ptr<Impl> d(new Impl());
    d->max_w = std::max(16, max_w);
    d->max_h = std::max(16, max_h);
    if (!open_input(path, &d->ic, err)) return false;

    d->vidx = av_find_best_stream(d->ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d->vidx < 0) {
        set_err(err, "El archivo no tiene video");
        return false;
    }
    AVStream *vs = d->ic->streams[d->vidx];
    const AVCodec *vc = find_decoder(vs->codecpar->codec_id);
    if (!vc) {
        set_err(err, std::string("No hay decodificador para ") +
                         avcodec_get_name(vs->codecpar->codec_id));
        return false;
    }
    d->vdec = avcodec_alloc_context3(vc);
    avcodec_parameters_to_context(d->vdec, vs->codecpar);
    d->vdec->thread_count = 0;
    d->vdec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    int r = avcodec_open2(d->vdec, vc, nullptr);
    if (r < 0) {
        set_err(err, "No se pudo abrir el decodificador de video (" + av_err(r) + ")");
        return false;
    }

    // Audio: la primera pista (con pistas separadas es la "Mezcla").
    for (unsigned i = 0; i < d->ic->nb_streams; i++) {
        AVStream *st = d->ic->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        const AVCodec *ac = avcodec_find_decoder(st->codecpar->codec_id);
        if (!ac) break;
        d->adec = avcodec_alloc_context3(ac);
        avcodec_parameters_to_context(d->adec, st->codecpar);
        if (avcodec_open2(d->adec, ac, nullptr) < 0) {
            avcodec_free_context(&d->adec);
            break;
        }
        d->aidx = (int)i;
        d->atb = av_q2d(st->time_base);
        break;
    }

    fill_info(d->ic, d->vidx, d->info);
    d->vtb = av_q2d(vs->time_base);
    d->origin = stream_start(vs) * d->vtb;
    d->vframe = av_frame_alloc();
    d->prev = av_frame_alloc();
    d->aframe = av_frame_alloc();
    d->pkt = av_packet_alloc();

    Impl *p = d.get();
    d->th = std::thread([p] { p->run(); });
    if (d->aidx >= 0) d->ath = std::thread([p] { p->audio_main(); });
    d_ = std::move(d);
    return true;
}

void ClipPlayer::close() {
    if (!d_) return;
    {
        std::lock_guard<std::mutex> lk(d_->m);
        d_->quit = true;
    }
    d_->cv.notify_all();
    if (d_->th.joinable()) d_->th.join();
    d_->aquit.store(true);
    if (d_->ath.joinable()) d_->ath.join();
    d_.reset();
}

bool ClipPlayer::is_open() const { return d_ != nullptr; }

const ClipInfo &ClipPlayer::info() const {
    static const ClipInfo empty;
    return d_ ? d_->info : empty;
}

void ClipPlayer::play() {
    if (!d_) return;
    {
        std::lock_guard<std::mutex> lk(d_->m);
        d_->want_play = true;
        d_->stop_at = -1;
    }
    d_->cv.notify_all();
}

void ClipPlayer::pause() {
    if (!d_) return;
    {
        std::lock_guard<std::mutex> lk(d_->m);
        if (!d_->want_play) return;
        d_->want_play = false;
        d_->stop_at = -1;
    }
    d_->cv.notify_all();
}

bool ClipPlayer::playing() const {
    if (!d_) return false;
    std::lock_guard<std::mutex> lk(d_->m);
    return d_->want_play;
}

void ClipPlayer::play_range(double start, double end) {
    if (!d_) return;
    {
        std::lock_guard<std::mutex> lk(d_->m);
        d_->seek_pending = true;
        d_->seek_to = start;
        d_->want_play = true;
        d_->stop_at = end;
    }
    d_->cv.notify_all();
}

void ClipPlayer::seek(double t) {
    if (!d_) return;
    {
        std::lock_guard<std::mutex> lk(d_->m);
        d_->seek_pending = true;
        d_->seek_to = t;
    }
    d_->pos.store(std::max(0.0, std::min(t, d_->info.duration)));
    d_->cv.notify_all();
}

double ClipPlayer::position() const { return d_ ? d_->pos.load() : 0.0; }

bool ClipPlayer::has_audio() const { return d_ && d_->aidx >= 0 && d_->audio_ok.load(); }

bool ClipPlayer::take_frame(std::vector<uint8_t> &bgra, int &w, int &h) {
    if (!d_) return false;
    std::lock_guard<std::mutex> lk(d_->fm);
    if (d_->fver == d_->ftaken || d_->frame.empty()) return false;
    d_->ftaken = d_->fver;
    bgra = d_->frame;
    w = d_->fw;
    h = d_->fh;
    return true;
}
