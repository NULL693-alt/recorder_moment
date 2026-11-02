#include "video_encoder.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

// ---------------------------------------------------------------------------
// Codificadores que probamos, en orden. El primero que abra es el que se usa.
// Los *_qsv son los de las GPU Intel (Intel Arc incluida).
// ---------------------------------------------------------------------------
struct CandidateDef {
    const char *name;
    bool is_qsv;
    const char *preset;
};

static const CandidateDef kH264[] = {
    {"h264_qsv", true, "veryfast"},
    {"h264_nvenc", false, nullptr},
    {"h264_amf", false, nullptr},
    {"libx264", false, "veryfast"},
};

// AV1 solo por hardware (Intel Arc, NVIDIA RTX 40+, AMD RX 7000+): por CPU es
// demasiado lento para grabar en vivo. Si no hay ninguno, se usa H.264.
static const CandidateDef kAV1[] = {
    {"av1_qsv", true, "veryfast"},
    {"av1_nvenc", false, nullptr},
    {"av1_amf", false, nullptr},
};

const char *video_codec_name(VideoCodec c) { return c == VideoCodec::AV1 ? "AV1" : "H.264"; }

// Cada cuanto se fuerza un keyframe, en tiempo (no en frames): con la pantalla
// casi quieta llegan pocos frames y un GOP contado en frames podria dejar los
// keyframes a decenas de segundos.
static const int64_t kKeyframeIntervalUs = 2000000;

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------
bool VideoEncoder::open(int width, int height, int fps, int bitrate_kbps, int target_height,
                        VideoCodec codec) {
    close();
    last_key_us_ = -1;
    time_base_ = AVRational{1, 1000000};

    // Resolucion de salida: si pidieron una altura menor que la capturada,
    // bajamos manteniendo la relacion de aspecto (ancho y alto pares, que es lo
    // que necesitan los codificadores H.264).
    out_width_ = width & ~1;
    out_height_ = height & ~1;
    if (target_height > 0 && target_height < height) {
        int h = target_height & ~1;
        int w = (int)((int64_t)width * h / height) & ~1;
        if (w >= 2 && h >= 2) {
            out_width_ = w;
            out_height_ = h;
        }
    }

    auto try_list = [&](const CandidateDef *list, size_t n) {
        for (size_t i = 0; i < n; i++) {
            Candidate cand{list[i].name, list[i].is_qsv, list[i].preset};
            if (try_open(cand, fps, bitrate_kbps)) return true;
        }
        return false;
    };

    bool opened = false;
    codec_ = VideoCodec::H264;
    if (codec == VideoCodec::AV1) {
        opened = try_list(kAV1, sizeof(kAV1) / sizeof(kAV1[0]));
        if (opened)
            codec_ = VideoCodec::AV1;
        else
            fprintf(stderr, "[ffmpeg] esta GPU no codifica AV1; se usa H.264\n");
    }
    if (!opened) opened = try_list(kH264, sizeof(kH264) / sizeof(kH264[0]));
    if (!opened) {
        fprintf(stderr, "[error] no pude abrir ningun codificador de video "
                        "(qsv/nvenc/amf/libx264)\n");
        return false;
    }

    // Parametros del stream, que despues se copian al escribir cada clip.
    codecpar_ = avcodec_parameters_alloc();
    if (!codecpar_ || avcodec_parameters_from_context(codecpar_, codec_ctx_) < 0) {
        fprintf(stderr, "[error] no pude copiar los parametros del codificador\n");
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

    if (out_width_ != width || out_height_ != height)
        fprintf(stderr, "[ffmpeg] resolucion de salida: %dx%d (de %dx%d)\n", out_width_,
                out_height_, width, height);
    fprintf(stderr, "[ffmpeg] listo: %s %dx%d @ %d fps, %d kbps (codificador: %s)\n",
            video_codec_name(codec_), out_width_, out_height_, fps, bitrate_kbps, encoder_name_);
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

    // El NV12 que arma gpu_convert es BT.709 rango limitado: lo anotamos en el
    // stream para que los reproductores muestren los colores bien.
    ctx->color_range = AVCOL_RANGE_MPEG;
    ctx->colorspace = AVCOL_SPC_BT709;
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;

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
    if (codecpar_) avcodec_parameters_free(&codecpar_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    encoder_name_ = nullptr;
}

VideoEncoder::~VideoEncoder() { close(); }

// ---------------------------------------------------------------------------
// encode
// ---------------------------------------------------------------------------
void VideoEncoder::drain(const PacketFn &on_packet) {
    while (avcodec_receive_packet(codec_ctx_, pkt_) == 0) {
        on_packet(pkt_);
        av_packet_unref(pkt_);
    }
}

bool VideoEncoder::encode_nv12(const uint8_t *y, int y_pitch, const uint8_t *uv, int uv_pitch,
                               int64_t pts_us, const PacketFn &on_packet) {
    if (!codec_ctx_) return false;

    // El codificador puede seguir usando el frame anterior: si es asi, esto
    // nos da un buffer nuevo en vez de pisarle los datos.
    if (av_frame_make_writable(nv12_) < 0) {
        fprintf(stderr, "[error] no pude preparar el frame NV12\n");
        return false;
    }
    av_image_copy_plane(nv12_->data[0], nv12_->linesize[0], y, y_pitch, out_width_,
                        out_height_);
    av_image_copy_plane(nv12_->data[1], nv12_->linesize[1], uv, uv_pitch, out_width_,
                        out_height_ / 2);
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
