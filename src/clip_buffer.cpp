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
    video_.clear();
    for (std::deque<Entry> &q : audio_) {
        for (Entry &e : q) av_packet_free(&e.pkt);
        q.clear();
    }
    last_video_us_ = 0;
}

void ClipBuffer::set_audio_tracks(int n) {
    for (std::deque<Entry> &q : audio_)
        for (Entry &e : q) av_packet_free(&e.pkt);
    audio_.assign((size_t)(n > 0 ? n : 1), std::deque<Entry>());
}

void ClipBuffer::prune(std::deque<Entry> &q, int seconds, int64_t last_us) {
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
    copy->audio_.assign(audio_.size(), std::deque<Entry>());
    for (size_t t = 0; t < audio_.size(); t++) {
        for (const Entry &e : audio_[t]) {
            AVPacket *p = av_packet_clone(e.pkt);
            if (p) copy->audio_[t].push_back(Entry{p, e.pts_us});
        }
    }
    copy->last_video_us_ = last_video_us_;
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

void ClipBuffer::push_audio(int track, const AVPacket *pkt, int64_t pts_us) {
    if (track < 0 || track >= (int)audio_.size()) return;
    AVPacket *copy = av_packet_clone(pkt);
    if (!copy) return;
    audio_[(size_t)track].push_back(Entry{copy, pts_us});
    prune(audio_[(size_t)track], seconds_, pts_us);
}

double ClipBuffer::buffered_seconds() const {
    if (video_.empty()) return 0.0;
    return (double)(last_video_us_ - video_.front().pts_us) / 1e6;
}

bool ClipBuffer::save_to(const std::string &path, const AVCodecParameters *video_par,
                         AVRational video_tb, const std::vector<AudioTrack> &audio,
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

    // Pistas de audio que van al clip: las que tengan parametros y paquetes
    // desde el origen en adelante.
    struct OutAudio {
        const std::deque<Entry> *q;
        size_t next;
        AVStream *st;
        const AudioTrack *info;
    };
    std::vector<OutAudio> tracks;
    for (size_t t = 0; t < audio.size() && t < audio_.size(); t++) {
        if (!audio[t].par || audio_[t].empty()) continue;
        const std::deque<Entry> &q = audio_[t];
        size_t start = q.size();
        for (size_t i = 0; i < q.size(); i++) {
            if (q[i].pts_us >= origin_us) {
                start = i;
                break;
            }
        }
        if (start == q.size()) continue; // el audio es mas viejo que el video
        tracks.push_back(OutAudio{&q, start, nullptr, &audio[t]});
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

    for (size_t i = 0; i < tracks.size(); i++) {
        AVStream *ast = avformat_new_stream(out, nullptr);
        if (!ast || avcodec_parameters_copy(ast->codecpar, tracks[i].info->par) < 0) {
            fprintf(stderr, "[error] no pude preparar el stream de audio\n");
            avformat_free_context(out);
            return false;
        }
        ast->codecpar->codec_tag = 0;
        ast->time_base = audio_tb;
        // La primera pista es la que suena; las otras quedan para editar.
        ast->disposition = (i == 0) ? AV_DISPOSITION_DEFAULT : 0;
        if (!tracks[i].info->title.empty()) {
            av_dict_set(&ast->metadata, "title", tracks[i].info->title.c_str(), 0);
            av_dict_set(&ast->metadata, "handler_name", tracks[i].info->title.c_str(), 0);
        }
        tracks[i].st = ast;
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

    // Escribimos todos los streams entrelazados por tiempo (siempre el paquete
    // mas viejo que quede), rebasando los timestamps para que el clip arranque
    // en 0 y el audio quede alineado.
    const AVRational us_tb = {1, 1000000};
    const int64_t vbase = video_[vstart].pkt->dts != AV_NOPTS_VALUE ? video_[vstart].pkt->dts
                                                                     : video_[vstart].pkt->pts;
    size_t vi = vstart;
    int written = 0;
    bool failed = false;

    while (!failed) {
        // -1 = video; si no, indice de la pista de audio.
        int pick = -2;
        int64_t best = INT64_MAX;
        if (vi < video_.size()) {
            pick = -1;
            best = video_[vi].pts_us;
        }
        for (size_t t = 0; t < tracks.size(); t++) {
            const OutAudio &a = tracks[t];
            if (a.next < a.q->size() && (*a.q)[a.next].pts_us < best) {
                pick = (int)t;
                best = (*a.q)[a.next].pts_us;
            }
        }
        if (pick == -2) break; // no queda nada

        AVPacket *p = nullptr;
        if (pick == -1) {
            p = av_packet_clone(video_[vi++].pkt);
            if (!p) continue;
            if (p->pts != AV_NOPTS_VALUE) p->pts -= vbase;
            if (p->dts != AV_NOPTS_VALUE) p->dts -= vbase;
            p->stream_index = vst->index;
            written++;
        } else {
            OutAudio &a = tracks[(size_t)pick];
            const Entry &e = (*a.q)[a.next++];
            p = av_packet_clone(e.pkt);
            if (!p) continue;
            int64_t rel_us = e.pts_us - origin_us;
            if (rel_us < 0) rel_us = 0;
            p->pts = p->dts = av_rescale_q(rel_us, us_tb, audio_tb);
            p->stream_index = a.st->index;
        }
        if (av_interleaved_write_frame(out, p) < 0) failed = true;
        av_packet_free(&p);
    }

    av_write_trailer(out);
    avio_closep(&out->pb);
    avformat_free_context(out);

    if (written == 0) {
        fprintf(stderr, "[error] no se escribio ningun frame en el clip\n");
        return false;
    }
    return !failed;
}
