#include "clip_buffer.h"

#include <cstdio>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

// Margen extra que conservamos por encima de la ventana pedida: sirve para
// tener algun keyframe de sobra antes del corte y para no podar de mas.
static const int kExtraMarginSeconds = 15;

ClipBuffer::ClipBuffer(int seconds) : seconds_(seconds > 0 ? seconds : 120) {}

ClipBuffer::~ClipBuffer() { clear(); }

void ClipBuffer::clear() {
    for (Entry &e : video_) av_packet_free(&e.pkt);
    for (Entry &e : audio_) av_packet_free(&e.pkt);
    video_.clear();
    audio_.clear();
    last_video_us_ = 0;
    last_audio_us_ = 0;
}

void ClipBuffer::prune(std::deque<Entry> &q, int seconds, int64_t &last_us) {
    int64_t keep_from = last_us - (int64_t)(seconds + kExtraMarginSeconds) * 1000000;
    while (q.size() > 1 && q.front().pts_us < keep_from) {
        av_packet_free(&q.front().pkt);
        q.pop_front();
    }
}

void ClipBuffer::prune_video(std::deque<Entry> &q, int seconds, int64_t last_us) {
    int64_t keep_from = last_us - (int64_t)(seconds + kExtraMarginSeconds) * 1000000;
    // Ultimo keyframe que ya quedo antes del corte: de ahi para atras sobra.
    size_t last_key = 0;
    bool found = false;
    for (size_t i = 0; i < q.size() && q[i].pts_us < keep_from; i++) {
        if (q[i].pkt->flags & AV_PKT_FLAG_KEY) {
            last_key = i;
            found = true;
        }
    }
    if (!found) return;
    for (size_t i = 0; i < last_key; i++) {
        av_packet_free(&q.front().pkt);
        q.pop_front();
    }
}

std::unique_ptr<ClipBuffer> ClipBuffer::snapshot() const {
    std::unique_ptr<ClipBuffer> copy(new ClipBuffer(seconds_));
    for (const Entry &e : video_) {
        AVPacket *p = av_packet_clone(e.pkt);
        if (p) copy->video_.push_back(Entry{p, e.pts_us});
    }
    for (const Entry &e : audio_) {
        AVPacket *p = av_packet_clone(e.pkt);
        if (p) copy->audio_.push_back(Entry{p, e.pts_us});
    }
    copy->last_video_us_ = last_video_us_;
    copy->last_audio_us_ = last_audio_us_;
    return copy;
}

void ClipBuffer::push_video(const AVPacket *pkt, int64_t pts_us, bool keyframe) {
    (void)keyframe; // el flag ya viene dentro del paquete
    AVPacket *copy = av_packet_clone(pkt);
    if (!copy) return;
    video_.push_back(Entry{copy, pts_us});
    last_video_us_ = pts_us;
    prune_video(video_, seconds_, last_video_us_);
}

void ClipBuffer::push_audio(const AVPacket *pkt, int64_t pts_us) {
    AVPacket *copy = av_packet_clone(pkt);
    if (!copy) return;
    audio_.push_back(Entry{copy, pts_us});
    last_audio_us_ = pts_us;
    prune(audio_, seconds_, last_audio_us_);
}

double ClipBuffer::buffered_seconds() const {
    if (video_.empty()) return 0.0;
    return (double)(last_video_us_ - video_.front().pts_us) / 1e6;
}

bool ClipBuffer::save_to(const std::string &path, const AVCodecParameters *video_par,
                         AVRational video_tb, const AVCodecParameters *audio_par,
                         AVRational audio_tb) const {
    if (video_.empty()) {
        fprintf(stderr, "[clip] todavia no hay video en el buffer\n");
        return false;
    }

    // Buscamos el keyframe mas reciente que este antes del corte para que el
    // clip empiece en un frame clave (si no, el reproductor muestra basura).
    int64_t cutoff = last_video_us_ - (int64_t)seconds_ * 1000000;
    size_t vstart = video_.size();
    for (size_t i = 0; i < video_.size(); i++) {
        if ((video_[i].pkt->flags & AV_PKT_FLAG_KEY) && video_[i].pts_us <= cutoff) vstart = i;
    }
    if (vstart == video_.size()) {
        // No hay keyframe antes del corte (el buffer todavia no se lleno):
        // arrancamos en el primero que haya.
        for (size_t i = 0; i < video_.size(); i++) {
            if (video_[i].pkt->flags & AV_PKT_FLAG_KEY) {
                vstart = i;
                break;
            }
        }
    }
    if (vstart == video_.size()) {
        fprintf(stderr, "[clip] todavia no hay ningun keyframe en el buffer\n");
        return false;
    }

    // Origen del clip: el instante del primer frame de video que se va a
    // escribir. El audio se alinea contra ese mismo instante.
    int64_t origin_us = video_[vstart].pts_us;

    bool with_audio = audio_par != nullptr && !audio_.empty();
    size_t astart = 0;
    if (with_audio) {
        astart = audio_.size();
        for (size_t i = 0; i < audio_.size(); i++) {
            if (audio_[i].pts_us >= origin_us) {
                astart = i;
                break;
            }
        }
        if (astart == audio_.size()) with_audio = false; // el audio es mas viejo que el video
    }

    AVFormatContext *out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, "mp4", path.c_str()) < 0 || !out) {
        fprintf(stderr, "[error] no pude crear el archivo '%s'\n", path.c_str());
        return false;
    }

    AVStream *vst = avformat_new_stream(out, nullptr);
    if (!vst || avcodec_parameters_copy(vst->codecpar, video_par) < 0) {
        fprintf(stderr, "[error] no pude preparar el stream de video\n");
        avformat_free_context(out);
        return false;
    }
    vst->codecpar->codec_tag = 0;
    vst->time_base = video_tb;
    // FPS del clip para los reproductores (si no, lo adivinan de los tiempos).
    if (video_par->framerate.num > 0 && video_par->framerate.den > 0)
        vst->avg_frame_rate = video_par->framerate;

    AVStream *ast = nullptr;
    if (with_audio) {
        ast = avformat_new_stream(out, nullptr);
        if (!ast || avcodec_parameters_copy(ast->codecpar, audio_par) < 0) {
            fprintf(stderr, "[error] no pude preparar el stream de audio\n");
            avformat_free_context(out);
            return false;
        }
        ast->codecpar->codec_tag = 0;
        ast->time_base = audio_tb;
    }

    if (avio_open(&out->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
        fprintf(stderr, "[error] no pude abrir '%s' para escribir\n", path.c_str());
        avformat_free_context(out);
        return false;
    }

    if (avformat_write_header(out, nullptr) < 0) {
        fprintf(stderr, "[error] no pude escribir la cabecera del clip\n");
        avio_closep(&out->pb);
        avformat_free_context(out);
        return false;
    }

    // Escribimos los dos streams entrelazados por tiempo, rebasando los
    // timestamps para que el clip arranque en 0 (y el audio quede alineado).
    const AVRational us_tb = {1, 1000000};
    size_t vi = vstart;
    size_t ai = astart;
    int written = 0;

    while (vi < video_.size() || (with_audio && ai < audio_.size())) {
        bool take_video;
        if (vi >= video_.size()) {
            take_video = false;
        } else if (!with_audio || ai >= audio_.size()) {
            take_video = true;
        } else {
            take_video = video_[vi].pts_us <= audio_[ai].pts_us;
        }

        if (take_video) {
            AVPacket *p = av_packet_clone(video_[vi].pkt);
            vi++;
            if (!p) continue;
            int64_t base = video_[vstart].pkt->dts != AV_NOPTS_VALUE
                               ? video_[vstart].pkt->dts
                               : video_[vstart].pkt->pts;
            if (p->pts != AV_NOPTS_VALUE) p->pts -= base;
            if (p->dts != AV_NOPTS_VALUE) p->dts -= base;
            p->stream_index = vst->index;
            if (av_interleaved_write_frame(out, p) < 0) {
                av_packet_free(&p);
                break;
            }
            av_packet_free(&p);
            written++;
        } else {
            AVPacket *p = av_packet_clone(audio_[ai].pkt);
            ai++;
            if (!p) continue;
            int64_t rel_us = audio_[ai - 1].pts_us - origin_us;
            if (rel_us < 0) rel_us = 0;
            int64_t new_pts = av_rescale_q(rel_us, us_tb, audio_tb);
            p->pts = new_pts;
            p->dts = new_pts;
            p->stream_index = ast->index;
            if (av_interleaved_write_frame(out, p) < 0) {
                av_packet_free(&p);
                break;
            }
            av_packet_free(&p);
        }
    }

    av_write_trailer(out);
    avio_closep(&out->pb);
    avformat_free_context(out);

    if (written == 0) {
        fprintf(stderr, "[error] no se escribio ningun frame en el clip\n");
        return false;
    }
    return true;
}
