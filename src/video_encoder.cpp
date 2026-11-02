#include "video_encoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

// ---------------------------------------------------------------------------
// Codificadores que probamos, en orden. El primero que abra es el que se usa.
// h264_qsv es el que corresponde a las GPU Intel (Intel Arc incluida).
// ---------------------------------------------------------------------------
static const struct {
    const char *name;
    bool is_qsv;
    const char *preset;
} kCandidates[] = {
    {"h264_qsv", true, "veryfast"},
    {"h264_nvenc", false, nullptr},
    {"h264_amf", false, nullptr},
    {"libx264", false, "veryfast"},
};

// ---------------------------------------------------------------------------
// HDR: conversion de half-float (IEEE 754 media precision) a float normal.
// ---------------------------------------------------------------------------
static inline float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign;
        } else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3FF;
            f = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7F800000u | (mant << 13);
    } else {
        f = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    memcpy(&out, &f, 4);
    return out;
}

// Conversion de un valor lineal [0..1] a 8 bits con la curva sRGB.
static inline uint8_t linear_to_srgb8(float v) {
    if (!(v > 0.0f)) return 0; // tambien descarta NaN
    if (v >= 1.0f) return 255;
    float s = (v <= 0.0031308f) ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    int i = (int)(s * 255.0f + 0.5f);
    return (uint8_t)(i < 0 ? 0 : (i > 255 ? 255 : i));
}

// Cada cuanto se fuerza un keyframe, en tiempo (no en frames): con la pantalla
// casi quieta llegan pocos frames y un GOP contado en frames podria dejar los
// keyframes a decenas de segundos.
static const int64_t kKeyframeIntervalUs = 2000000;

// HDR -> SDR de un canal scRGB lineal. Primero se divide por el blanco SDR, asi
// lo que en el monitor se ve como contenido SDR queda igual que en SDR. Lo que
// pasa de un 75% (brillos HDR) se comprime de a poco hacia 1.0 en vez de
// recortarse de golpe.
static inline float tonemap_channel(float v, float white) {
    float x = v / white;
    if (!(x > 0.0f)) return 0.0f;
    const float knee = 0.75f;
    if (x <= knee) return x;
    return knee + (1.0f - knee) * (1.0f - expf(-(x - knee) / (1.0f - knee)));
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------
bool VideoEncoder::open(int width, int height, int fps, int bitrate_kbps,
                        AVPixelFormat src_fmt, int target_height, float sdr_white_scale) {
    close();
    last_key_us_ = -1;

    width_ = width;
    height_ = height;
    src_fmt_ = src_fmt;
    hdr_tonemap_ = (src_fmt == AV_PIX_FMT_RGBAF16);
    time_base_ = AVRational{1, 1000000};

    // Resolucion de salida: si pidieron una altura menor que la capturada,
    // bajamos manteniendo la relacion de aspecto (ancho y alto pares, que es lo
    // que necesitan los codificadores H.264).
    out_width_ = width_;
    out_height_ = height_;
    if (target_height > 0 && target_height < height_) {
        int h = target_height & ~1;
        int w = (int)((int64_t)width_ * h / height_) & ~1;
        if (w >= 2 && h >= 2) {
            out_width_ = w;
            out_height_ = h;
        }
    }

    bool opened = false;
    for (const auto &c : kCandidates) {
        Candidate cand{c.name, c.is_qsv, c.preset};
        if (try_open(cand, fps, bitrate_kbps)) {
            opened = true;
            break;
        }
    }
    if (!opened) {
        fprintf(stderr, "[error] no pude abrir ningun codificador h264 "
                        "(qsv/nvenc/amf/libx264)\n");
        return false;
    }

    // Parametros del stream, que despues se copian al escribir cada clip.
    codecpar_ = avcodec_parameters_alloc();
    if (!codecpar_ || avcodec_parameters_from_context(codecpar_, codec_ctx_) < 0) {
        fprintf(stderr, "[error] no pude copiar los parametros del codificador\n");
        return false;
    }

    // Entrada del conversor de color. Si es HDR primero pasamos por tonemap a
    // BGRA8, asi que el conversor recibe BGRA; si no, recibe el formato tal cual.
    // La salida es NV12 a la resolucion pedida (escala si hace falta).
    AVPixelFormat sws_src = hdr_tonemap_ ? AV_PIX_FMT_BGRA : src_fmt_;
    sws_ = sws_getContext(width_, height_, sws_src, out_width_, out_height_, AV_PIX_FMT_NV12,
                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) {
        fprintf(stderr, "[error] no pude crear el conversor de color (sws)\n");
        return false;
    }

    nv12_ = av_frame_alloc();
    nv12_->format = AV_PIX_FMT_NV12;
    nv12_->width = out_width_;
    nv12_->height = out_height_;
    if (av_frame_get_buffer(nv12_, 32) < 0) {
        fprintf(stderr, "[error] no pude reservar el frame NV12\n");
        return false;
    }

    pkt_ = av_packet_alloc();

    if (hdr_tonemap_) {
        tonemap_buf_.resize((size_t)width_ * height_ * 4);
        // Tabla con el resultado para cada uno de los 65536 half-float posibles:
        // por pixel queda una sola lectura de tabla (sin conversiones ni powf).
        float white = sdr_white_scale >= 1.0f ? sdr_white_scale : 1.0f;
        tonemap_lut_.resize(65536);
        for (int i = 0; i < 65536; i++)
            tonemap_lut_[i] = linear_to_srgb8(tonemap_channel(half_to_float((uint16_t)i), white));
        fprintf(stderr, "[ffmpeg] HDR: blanco SDR = %.0f nits\n", white * 80.0f);
    }

    if (out_width_ != width_ || out_height_ != height_)
        fprintf(stderr, "[ffmpeg] resolucion de salida: %dx%d (de %dx%d)\n",
                out_width_, out_height_, width_, height_);
    fprintf(stderr, "[ffmpeg] listo: %dx%d @ %d fps, %d kbps (codificador: %s%s)\n",
            out_width_, out_height_, fps, bitrate_kbps, encoder_name_,
            hdr_tonemap_ ? ", HDR->SDR" : "");
    return true;
}

bool VideoEncoder::try_open(const Candidate &cand, int fps, int bitrate_kbps) {
    const AVCodec *codec = avcodec_find_encoder_by_name(cand.name);
    if (!codec) return false;

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;

    ctx->width = out_width_;
    ctx->height = out_height_;
    ctx->time_base = time_base_;
    ctx->framerate = AVRational{fps, 1};
    ctx->pix_fmt = AV_PIX_FMT_NV12;
    // Los keyframes los forzamos cada kKeyframeIntervalUs; el GOP queda largo
    // solo como respaldo (si no, salen keyframes dobles pegados).
    ctx->gop_size = fps * 10;
    ctx->max_b_frames = 0;
    ctx->bit_rate = (int64_t)bitrate_kbps * 1000;
    if (strcmp(cand.name, "libx264") == 0) ctx->rc_buffer_size = bitrate_kbps * 2000;

    if (cand.preset) av_opt_set(ctx->priv_data, "preset", cand.preset, 0);

    // Los keyframes que forzamos por tiempo tienen que ser IDR (si no, el
    // paquete no sale marcado como keyframe y el clip no puede empezar ahi).
    // Cada codificador lo llama distinto; el que no lo tenga lo ignora.
    av_opt_set_int(ctx->priv_data, "forced_idr", 1, 0); // qsv, amf
    av_opt_set_int(ctx->priv_data, "forced-idr", 1, 0); // nvenc, libx264

    // Quick Sync necesita un "dispositivo" que representa al chip de la GPU.
    if (cand.is_qsv) {
        if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_QSV, nullptr,
                                   nullptr, 0) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }
    }

    // El mp4 necesita los parametros (SPS/PPS) en la cabecera del stream.
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return false;
    }

    codec_ctx_ = ctx;
    encoder_name_ = cand.name;
    return true;
}

void VideoEncoder::close() {
    if (pkt_) av_packet_free(&pkt_);
    if (nv12_) av_frame_free(&nv12_);
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (codecpar_) avcodec_parameters_free(&codecpar_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    encoder_name_ = nullptr;
    tonemap_buf_.clear();
    tonemap_lut_.clear();
}

VideoEncoder::~VideoEncoder() { close(); }

// ---------------------------------------------------------------------------
// Tonemap HDR (scRGB float lineal) -> BGRA8 (sRGB)
// ---------------------------------------------------------------------------
void VideoEncoder::tonemap_to_bgra(const uint8_t *data, int pitch) {
    for (int y = 0; y < height_; y++) {
        const uint16_t *src = reinterpret_cast<const uint16_t *>(data + (size_t)y * pitch);
        uint8_t *dst = tonemap_buf_.data() + (size_t)y * width_ * 4;
        const uint8_t *lut = tonemap_lut_.data();
        for (int x = 0; x < width_; x++) {
            dst[0] = lut[src[2]]; // BGRA
            dst[1] = lut[src[1]];
            dst[2] = lut[src[0]];
            dst[3] = 255;
            src += 4;
            dst += 4;
        }
    }
}

// ---------------------------------------------------------------------------
// encode
// ---------------------------------------------------------------------------
void VideoEncoder::drain(const PacketFn &on_packet) {
    while (avcodec_receive_packet(codec_ctx_, pkt_) == 0) {
        on_packet(pkt_);
        av_packet_unref(pkt_);
    }
}

bool VideoEncoder::encode(const uint8_t *data, int pitch, int64_t pts_us,
                          const PacketFn &on_packet) {
    if (!codec_ctx_) return false;

    const uint8_t *src[1];
    int stride[1];
    if (hdr_tonemap_) {
        tonemap_to_bgra(data, pitch);
        src[0] = tonemap_buf_.data();
        stride[0] = width_ * 4;
    } else {
        src[0] = data;
        stride[0] = pitch;
    }

    sws_scale(sws_, src, stride, 0, height_, nv12_->data, nv12_->linesize);
    nv12_->pts = pts_us;

    // Keyframe por tiempo (el codificador igual pone los suyos por GOP).
    bool force_key = last_key_us_ < 0 || pts_us - last_key_us_ >= kKeyframeIntervalUs;
    nv12_->pict_type = force_key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (force_key) last_key_us_ = pts_us;

    int r = avcodec_send_frame(codec_ctx_, nv12_);
    if (r < 0) {
        fprintf(stderr, "[error] avcodec_send_frame fallo\n");
        return false;
    }
    // Tambien contamos los keyframes que pone el codificador por su GOP, para
    // no meter keyframes de mas.
    drain([this, &on_packet](const AVPacket *pkt) {
        if ((pkt->flags & AV_PKT_FLAG_KEY) && pkt->pts > last_key_us_) last_key_us_ = pkt->pts;
        on_packet(pkt);
    });
    return true;
}

void VideoEncoder::flush(const PacketFn &on_packet) {
    if (!codec_ctx_) return;
    avcodec_send_frame(codec_ctx_, nullptr);
    drain(on_packet);
}
