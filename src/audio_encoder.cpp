#include "audio_encoder.h"

#include <cstdio>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

AudioEncoder::~AudioEncoder() { close(); }

bool AudioEncoder::open(int sample_rate, int channels, int bitrate_kbps) {
    close();
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_ = channels > 0 ? channels : 2;
    time_base_ = AVRational{1, sample_rate_};

    const AVCodec *codec = avcodec_find_encoder_by_name("aac");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        fprintf(stderr, "[audio] no encontre un codificador AAC\n");
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    codec_ctx_->sample_rate = sample_rate_;
    av_channel_layout_default(&codec_ctx_->ch_layout, channels_);
    // Averiguamos que formato de muestra quiere el codificador (AAC: FLTP).
    const void *fmts = nullptr;
    int num = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &fmts,
                                     &num) >= 0 &&
        fmts && num > 0)
        codec_ctx_->sample_fmt = ((const enum AVSampleFormat *)fmts)[0];
    else
        codec_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    codec_ctx_->bit_rate = (int64_t)bitrate_kbps * 1000;
    codec_ctx_->time_base = time_base_;
    // El mp4 necesita los parametros (SPS/PPS/ASC) en la cabecera del stream.
    codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        fprintf(stderr, "[audio] no pude abrir el codificador AAC\n");
        close();
        return false;
    }

    codecpar_ = avcodec_parameters_alloc();
    if (!codecpar_ || avcodec_parameters_from_context(codecpar_, codec_ctx_) < 0) {
        fprintf(stderr, "[audio] no pude copiar los parametros del codificador\n");
        close();
        return false;
    }

    // Conversor: PCM 16 bits intercalado -> formato que quiere el codificador.
    AVChannelLayout in_layout;
    av_channel_layout_default(&in_layout, channels_);
    int rc = swr_alloc_set_opts2(&swr_, &codec_ctx_->ch_layout, codec_ctx_->sample_fmt,
                                 sample_rate_, &in_layout, AV_SAMPLE_FMT_S16, sample_rate_, 0,
                                 nullptr);
    av_channel_layout_uninit(&in_layout);
    if (rc < 0 || !swr_ || swr_init(swr_) < 0) {
        fprintf(stderr, "[audio] no pude preparar el conversor de audio\n");
        close();
        return false;
    }

    frame_ = av_frame_alloc();
    frame_->format = codec_ctx_->sample_fmt;
    frame_->ch_layout = codec_ctx_->ch_layout;
    frame_->sample_rate = sample_rate_;
    frame_->nb_samples = codec_ctx_->frame_size > 0 ? codec_ctx_->frame_size : 1024;
    if (av_frame_get_buffer(frame_, 0) < 0) {
        fprintf(stderr, "[audio] no pude reservar el frame de audio\n");
        close();
        return false;
    }

    pkt_ = av_packet_alloc();

    fprintf(stderr, "[audio] codificador: %s @ %d Hz, %d kbps\n", codec->name, sample_rate_,
            bitrate_kbps);
    return true;
}

void AudioEncoder::close() {
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (swr_) swr_free(&swr_);
    if (codecpar_) avcodec_parameters_free(&codecpar_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    pending_.clear();
    next_pts_ = 0;
}

void AudioEncoder::drain(const PacketFn &on_packet) {
    while (avcodec_receive_packet(codec_ctx_, pkt_) == 0) {
        on_packet(pkt_);
        av_packet_unref(pkt_);
    }
}

// Junta muestras hasta completar frames del tamano que pide el codificador.
bool AudioEncoder::feed(const PacketFn &on_packet) {
    int frame_size = frame_->nb_samples;
    while ((int)(pending_.size() / channels_) >= frame_size) {
        if (av_frame_make_writable(frame_) < 0) return false;

        const uint8_t *in = reinterpret_cast<const uint8_t *>(pending_.data());
        int got = swr_convert(swr_, frame_->data, frame_size, &in, frame_size);
        if (got <= 0) return false;

        frame_->nb_samples = got;
        frame_->pts = next_pts_;
        next_pts_ += got;

        if (avcodec_send_frame(codec_ctx_, frame_) < 0) return false;
        drain(on_packet);

        pending_.erase(pending_.begin(), pending_.begin() + (size_t)frame_size * channels_);
    }
    return true;
}

bool AudioEncoder::encode(const int16_t *data, int frames, const PacketFn &on_packet) {
    if (!codec_ctx_) return false;
    if (frames <= 0) return true;
    pending_.insert(pending_.end(), data, data + (size_t)frames * channels_);
    return feed(on_packet);
}

void AudioEncoder::flush(const PacketFn &on_packet) {
    if (!codec_ctx_) return;

    // Completamos el ultimo frame parcial con silencio para no perder audio.
    int frame_size = frame_->nb_samples;
    int have = (int)(pending_.size() / channels_);
    if (have > 0 && have < frame_size) {
        size_t missing = (size_t)(frame_size - have) * channels_;
        pending_.insert(pending_.end(), missing, 0);
        feed(on_packet);
    }

    avcodec_send_frame(codec_ctx_, nullptr);
    drain(on_packet);
}
