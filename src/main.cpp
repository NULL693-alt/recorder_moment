// ============================================================================
// FASE 3: codificacion por GPU (Intel Quick Sync via VAAPI) en vez de CPU
//
// Esto cambia UNA sola cosa respecto a la Fase 2: como se codifica cada
// frame. Todo lo demas (portal, PipeWire, el archivo .mp4, Ctrl+C para
// cortar) es identico.
//
// Con libx264 (Fase 2), la CPU hacia todo el trabajo de comprimir el video.
// Ahora se lo mandamos a tu GPU Intel, que tiene un bloque de hardware
// dedicado a esto (Quick Sync) — mucho mas rapido y con much simo menos uso
// de CPU, algo importante si vas a grabar mientras jugas.
//
// El cambio tecnico:
//   - Antes: frame en RAM (YUV420P) -> libx264 -> paquete comprimido.
//   - Ahora: frame en RAM (NV12) -> SUBIDO a la memoria de la GPU (una
//     "superficie VAAPI") -> el chip Quick Sync lo comprime -> paquete
//     comprimido.
//
// Esto agrega dos conceptos nuevos de FFmpeg:
//   - AVHWDeviceContext: representa "tu GPU" (el archivo /dev/dri/renderD128).
//   - AVHWFramesContext: un pool de superficies de video EN la GPU, del
//     mismo ancho/alto que tu pantalla, listas para que Quick Sync las lea.
//
// Nota importante: yo no tengo una GPU Intel en mi entorno de pruebas, asi
// que esta parte no la pude ejecutar de punta a punta como las anteriores
// (si la compile y verifique contra los headers reales de FFmpeg). Es el
// tramo con mas chance de necesitar un ajuste chico en tu maquina. Si algo
// falla, el mensaje de error nos va a decir bastante.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <chrono>
#include <csignal>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>

#include <glib.h>
#include <gio/gio.h>
#include <libportal/portal.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

static std::atomic<bool> g_stop_requested{false};
static void handle_stop_signal(int) { g_stop_requested.store(true); }

struct AppState {
    // --- lado del portal (GLib/D-Bus) ---
    GMainLoop *glib_loop = nullptr;
    XdpPortal *portal = nullptr;
    XdpSession *session = nullptr;
    guint32 node_id = 0;
    int pipewire_fd = -1;
    bool failed = false;

    // --- lado de PipeWire ---
    struct pw_main_loop *pw_loop = nullptr;
    struct pw_stream *pw_stream = nullptr;
    struct spa_hook stream_listener{};
    struct spa_video_info format{};
    bool have_format = false;

    // --- lado de FFmpeg / VAAPI (Fase 3) ---
    std::string output_path = "grabacion.mp4";
    std::string vaapi_device = "/dev/dri/renderD128";
    AVFormatContext *fmt_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    AVStream *out_stream = nullptr;
    SwsContext *sws_ctx = nullptr;
    AVBufferRef *hw_device_ctx = nullptr;
    AVBufferRef *hw_frames_ref = nullptr;
    AVFrame *sw_frame = nullptr; // frame normal en RAM, formato NV12
    AVFrame *hw_frame = nullptr; // frame "en la GPU", formato VAAPI
    bool encoder_ready = false;
    int64_t start_time_us = -1;
    int64_t frame_count = 0;
    int64_t last_pts_us = 0;
};

static AVPixelFormat spa_format_to_av_pix_fmt(enum spa_video_format f) {
    switch (f) {
        case SPA_VIDEO_FORMAT_BGRx: return AV_PIX_FMT_BGR0;
        case SPA_VIDEO_FORMAT_BGRA: return AV_PIX_FMT_BGRA;
        case SPA_VIDEO_FORMAT_RGBx: return AV_PIX_FMT_RGB0;
        case SPA_VIDEO_FORMAT_RGBA: return AV_PIX_FMT_RGBA;
        default: return AV_PIX_FMT_NONE;
    }
}

// ============================================================================
// LADO FFMPEG / VAAPI
// ============================================================================

static bool setup_encoder(AppState *state) {
    int width = state->format.info.raw.size.width;
    int height = state->format.info.raw.size.height;
    AVPixelFormat src_fmt = spa_format_to_av_pix_fmt(state->format.info.raw.format);

    if (src_fmt == AV_PIX_FMT_NONE) {
        fprintf(stderr, "[error] formato de color no soportado todavia (id=%d)\n",
                (int)state->format.info.raw.format);
        return false;
    }

    // 1) Abrir la GPU. Si esto falla (ej: /dev/dri/renderD128 no existe, o
    // no tenes permiso de lectura sobre el), va a fallar aca con un mensaje
    // claro. Si te pasa, avisame el mensaje exacto.
    int err = av_hwdevice_ctx_create(&state->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                      state->vaapi_device.c_str(), nullptr, 0);
    if (err < 0) {
        char errbuf[256];
        av_strerror(err, errbuf, sizeof(errbuf));
        fprintf(stderr, "[error] no pude abrir el dispositivo VAAPI '%s': %s\n"
                        "Prueba: 'ls /dev/dri/' para ver que nodos existen, o corre "
                        "'vainfo --display drm --device %s' para diagnosticar el driver.\n",
                state->vaapi_device.c_str(), errbuf, state->vaapi_device.c_str());
        return false;
    }

    // 2) Crear el pool de superficies de video EN la GPU.
    state->hw_frames_ref = av_hwframe_ctx_alloc(state->hw_device_ctx);
    if (!state->hw_frames_ref) {
        fprintf(stderr, "[error] av_hwframe_ctx_alloc fallo\n");
        return false;
    }
    auto *frames_ctx = reinterpret_cast<AVHWFramesContext *>(state->hw_frames_ref->data);
    frames_ctx->format = AV_PIX_FMT_VAAPI;   // el tipo de superficie (GPU)
    frames_ctx->sw_format = AV_PIX_FMT_NV12; // como esta organizado el pixel adentro
    frames_ctx->width = width;
    frames_ctx->height = height;
    frames_ctx->initial_pool_size = 20;

    if ((err = av_hwframe_ctx_init(state->hw_frames_ref)) < 0) {
        char errbuf[256];
        av_strerror(err, errbuf, sizeof(errbuf));
        fprintf(stderr, "[error] av_hwframe_ctx_init fallo: %s\n", errbuf);
        return false;
    }

    // 3) Configurar el codificador para que use esa GPU/pool.
    avformat_alloc_output_context2(&state->fmt_ctx, nullptr, nullptr,
                                    state->output_path.c_str());
    if (!state->fmt_ctx) {
        fprintf(stderr, "[error] no pude crear el contexto de salida\n");
        return false;
    }

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec) {
        fprintf(stderr, "[error] tu FFmpeg no tiene el codificador h264_vaapi\n");
        return false;
    }

    state->out_stream = avformat_new_stream(state->fmt_ctx, nullptr);
    state->codec_ctx = avcodec_alloc_context3(codec);

    state->codec_ctx->width = width;
    state->codec_ctx->height = height;
    state->codec_ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    state->codec_ctx->time_base = AVRational{1, 1000000}; // microsegundos, igual que Fase 2
    state->codec_ctx->framerate = AVRational{30, 1};
    state->codec_ctx->gop_size = 60;
    state->codec_ctx->max_b_frames = 0;
    state->codec_ctx->hw_frames_ctx = av_buffer_ref(state->hw_frames_ref);

    if (state->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        state->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(state->codec_ctx, codec, nullptr) < 0) {
        fprintf(stderr, "[error] no pude abrir el codificador h264_vaapi\n");
        return false;
    }

    avcodec_parameters_from_context(state->out_stream->codecpar, state->codec_ctx);
    state->out_stream->time_base = state->codec_ctx->time_base;

    if (!(state->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&state->fmt_ctx->pb, state->output_path.c_str(), AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "[error] no pude abrir el archivo de salida %s\n",
                    state->output_path.c_str());
            return false;
        }
    }

    if (avformat_write_header(state->fmt_ctx, nullptr) < 0) {
        fprintf(stderr, "[error] no pude escribir la cabecera del mp4\n");
        return false;
    }

    // Frame "normal" en RAM, en NV12 (lo que la GPU espera recibir).
    state->sw_frame = av_frame_alloc();
    state->sw_frame->format = AV_PIX_FMT_NV12;
    state->sw_frame->width = width;
    state->sw_frame->height = height;
    if (av_frame_get_buffer(state->sw_frame, 32) < 0) {
        fprintf(stderr, "[error] no pude reservar el frame NV12\n");
        return false;
    }

    // Frame "en la GPU": se lo pedimos al pool cada vez (ver on_process).
    state->hw_frame = av_frame_alloc();

    state->sws_ctx = sws_getContext(width, height, src_fmt,
                                     width, height, AV_PIX_FMT_NV12,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!state->sws_ctx) {
        fprintf(stderr, "[error] no pude crear el conversor de color (sws)\n");
        return false;
    }

    fprintf(stderr, "[ffmpeg] listo: %dx%d -> %s (h264_vaapi, GPU Intel)\n",
            width, height, state->output_path.c_str());
    state->encoder_ready = true;
    return true;
}

static void encode_and_write(AppState *state, AVFrame *frame) {
    if (avcodec_send_frame(state->codec_ctx, frame) < 0) {
        fprintf(stderr, "[error] avcodec_send_frame fallo\n");
        return;
    }
    AVPacket *pkt = av_packet_alloc();
    while (avcodec_receive_packet(state->codec_ctx, pkt) == 0) {
        av_packet_rescale_ts(pkt, state->codec_ctx->time_base, state->out_stream->time_base);
        pkt->stream_index = state->out_stream->index;
        av_interleaved_write_frame(state->fmt_ctx, pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

static void finish_encoding(AppState *state) {
    if (!state->encoder_ready) return;

    encode_and_write(state, nullptr); // flush
    av_write_trailer(state->fmt_ctx);

    if (!(state->fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&state->fmt_ctx->pb);

    avcodec_free_context(&state->codec_ctx);
    av_frame_free(&state->sw_frame);
    av_frame_free(&state->hw_frame);
    sws_freeContext(state->sws_ctx);
    av_buffer_unref(&state->hw_frames_ref);
    av_buffer_unref(&state->hw_device_ctx);
    avformat_free_context(state->fmt_ctx);

    double seconds = (double)state->last_pts_us / 1e6;
    fprintf(stderr, "\n[ok] Video guardado: %s (%ld frames, %.1fs)\n",
            state->output_path.c_str(), (long)state->frame_count, seconds);
}

// ============================================================================
// LADO PIPEWIRE
// ============================================================================

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param) {
    auto *state = static_cast<AppState *>(userdata);
    if (param == nullptr || id != SPA_PARAM_Format)
        return;
    if (spa_format_parse(param, &state->format.media_type, &state->format.media_subtype) < 0)
        return;
    if (state->format.media_type != SPA_MEDIA_TYPE_video ||
        state->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &state->format.info.raw) < 0)
        return;

    state->have_format = true;
    fprintf(stderr, "[pipewire] formato negociado: %s, %dx%d @ %d/%d fps\n",
            spa_debug_type_find_name(spa_type_video_format, state->format.info.raw.format),
            state->format.info.raw.size.width,
            state->format.info.raw.size.height,
            state->format.info.raw.framerate.num,
            state->format.info.raw.framerate.denom);
}

static void on_process(void *userdata) {
    auto *state = static_cast<AppState *>(userdata);

    struct pw_buffer *b = pw_stream_dequeue_buffer(state->pw_stream);
    if (!b) {
        fprintf(stderr, "[pipewire] out of buffers\n");
        return;
    }

    struct spa_buffer *buf = b->buffer;
    uint8_t *data = static_cast<uint8_t *>(buf->datas[0].data);

    if (data && state->have_format) {
        if (!state->encoder_ready) {
            if (!setup_encoder(state)) {
                state->failed = true;
                pw_stream_queue_buffer(state->pw_stream, b);
                pw_main_loop_quit(state->pw_loop);
                return;
            }
        }

        int32_t stride = buf->datas[0].chunk->stride;
        if (stride <= 0)
            stride = state->format.info.raw.size.width * 4;

        const uint8_t *src_slices[1] = { data };
        int src_stride[1] = { stride };

        // Paso 1: convertir de BGRx (o lo que haya negociado tu compositor)
        // a NV12, todavia en RAM normal.
        av_frame_make_writable(state->sw_frame);
        sws_scale(state->sws_ctx, src_slices, src_stride, 0,
                  state->format.info.raw.size.height,
                  state->sw_frame->data, state->sw_frame->linesize);

        // Paso 2: pedirle al pool una superficie libre en la GPU, y subir
        // ahi los datos NV12 que acabamos de generar.
        av_frame_unref(state->hw_frame);
        if (av_hwframe_get_buffer(state->hw_frames_ref, state->hw_frame, 0) < 0) {
            fprintf(stderr, "[error] av_hwframe_get_buffer fallo (se quedo sin superficies libres?)\n");
            pw_stream_queue_buffer(state->pw_stream, b);
            return;
        }
        if (av_hwframe_transfer_data(state->hw_frame, state->sw_frame, 0) < 0) {
            fprintf(stderr, "[error] av_hwframe_transfer_data fallo (no pude subir el frame a la GPU)\n");
            pw_stream_queue_buffer(state->pw_stream, b);
            return;
        }

        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (state->start_time_us < 0)
            state->start_time_us = now_us;

        state->hw_frame->pts = now_us - state->start_time_us;
        state->last_pts_us = state->hw_frame->pts;

        // Paso 3: mandarselo al chip Quick Sync.
        encode_and_write(state, state->hw_frame);
        state->frame_count++;

        if (state->frame_count % 30 == 0) {
            fprintf(stderr, "\r[grabando] %ld frames, %.1fs (Ctrl+C para detener)  ",
                    (long)state->frame_count, state->hw_frame->pts / 1e6);
            fflush(stderr);
        }
    }

    pw_stream_queue_buffer(state->pw_stream, b);

    if (g_stop_requested.load()) {
        pw_main_loop_quit(state->pw_loop);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process = on_process,
};

static void run_pipewire_capture(AppState *state) {
    pw_init(nullptr, nullptr);

    state->pw_loop = pw_main_loop_new(nullptr);
    struct pw_context *context =
        pw_context_new(pw_main_loop_get_loop(state->pw_loop), nullptr, 0);

    struct pw_core *core =
        pw_context_connect_fd(context, fcntl(state->pipewire_fd, F_DUPFD_CLOEXEC, 5),
                               nullptr, 0);
    if (!core) {
        fprintf(stderr, "[error] no pude conectar con PipeWire (pw_context_connect_fd)\n");
        return;
    }

    state->pw_stream = pw_stream_new(core, "medal-clone-capture",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            nullptr));

    pw_stream_add_listener(state->pw_stream, &state->stream_listener,
                            &stream_events, state);

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    struct spa_rectangle rect_default = SPA_RECTANGLE(1920, 1080);
    struct spa_rectangle rect_min     = SPA_RECTANGLE(1, 1);
    struct spa_rectangle rect_max     = SPA_RECTANGLE(8192, 4320);
    struct spa_fraction  fps_default  = SPA_FRACTION(60, 1);
    struct spa_fraction  fps_min      = SPA_FRACTION(0, 1);
    struct spa_fraction  fps_max      = SPA_FRACTION(1000, 1);

    const struct spa_pod *params[1];
    params[0] = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
                                    SPA_VIDEO_FORMAT_BGRx,
                                    SPA_VIDEO_FORMAT_BGRx,
                                    SPA_VIDEO_FORMAT_RGBx,
                                    SPA_VIDEO_FORMAT_BGRA,
                                    SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size,   SPA_POD_CHOICE_RANGE_Rectangle(
                                    &rect_default, &rect_min, &rect_max),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                                    &fps_default, &fps_min, &fps_max)));

    pw_stream_connect(state->pw_stream,
                       PW_DIRECTION_INPUT,
                       state->node_id,
                       static_cast<enum pw_stream_flags>(
                           PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                       params, 1);

    fprintf(stderr, "[pipewire] conectando al nodo %u...\n", state->node_id);
    fprintf(stderr, "Grabando a %s. Apreta Ctrl+C para detener.\n", state->output_path.c_str());

    signal(SIGINT, handle_stop_signal);
    signal(SIGTERM, handle_stop_signal);

    pw_main_loop_run(state->pw_loop);

    finish_encoding(state);

    pw_stream_destroy(state->pw_stream);
    pw_context_destroy(context);
    pw_main_loop_destroy(state->pw_loop);
}

// ============================================================================
// LADO PORTAL (igual que en las fases anteriores)
// ============================================================================

static void quit_glib_loop(AppState *state) {
    if (state->glib_loop) g_main_loop_quit(state->glib_loop);
}

static void on_session_started(GObject *source, GAsyncResult *result, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    GError *error = nullptr;

    gboolean ok = xdp_session_start_finish(state->session, result, &error);
    if (!ok) {
        fprintf(stderr, "[error] no se pudo iniciar la sesion: %s\n",
                error ? error->message : "desconocido");
        if (error) g_error_free(error);
        state->failed = true;
        quit_glib_loop(state);
        return;
    }

    GVariant *streams = xdp_session_get_streams(state->session);
    if (!streams) {
        fprintf(stderr, "[error] la sesion no devolvio ningun stream\n");
        state->failed = true;
        quit_glib_loop(state);
        return;
    }

    GVariantIter iter;
    g_variant_iter_init(&iter, streams);
    guint32 node_id = 0;
    GVariant *props = nullptr;
    if (!g_variant_iter_next(&iter, "(u@a{sv})", &node_id, &props)) {
        fprintf(stderr, "[error] no pude leer el node_id del stream\n");
        state->failed = true;
        quit_glib_loop(state);
        return;
    }
    if (props) g_variant_unref(props);

    state->node_id = node_id;
    state->pipewire_fd = xdp_session_open_pipewire_remote(state->session);

    fprintf(stderr, "[portal] sesion activa. node_id=%u, fd=%d\n",
            state->node_id, state->pipewire_fd);

    quit_glib_loop(state);
}

static void on_session_created(GObject *source, GAsyncResult *result, gpointer user_data) {
    auto *state = static_cast<AppState *>(user_data);
    GError *error = nullptr;

    state->session = xdp_portal_create_screencast_session_finish(state->portal, result, &error);
    if (!state->session) {
        fprintf(stderr, "[error] no se pudo crear la sesion (¿cancelaste el dialogo?): %s\n",
                error ? error->message : "desconocido");
        if (error) g_error_free(error);
        state->failed = true;
        quit_glib_loop(state);
        return;
    }

    xdp_session_start(state->session, nullptr, nullptr, on_session_started, state);
}

int main(int argc, char **argv) {
    AppState state;
    if (argc > 1) state.output_path = argv[1];
    if (argc > 2) state.vaapi_device = argv[2];

    state.glib_loop = g_main_loop_new(nullptr, FALSE);
    state.portal = xdp_portal_new();

    fprintf(stderr, "Pidiendo permiso para capturar pantalla (deberia aparecer un dialogo "
                    "del sistema)...\n");

    xdp_portal_create_screencast_session(
        state.portal,
        XDP_OUTPUT_MONITOR,
        XDP_SCREENCAST_FLAG_NONE,
        XDP_CURSOR_MODE_EMBEDDED,
        XDP_PERSIST_MODE_NONE,
        nullptr,
        nullptr,
        on_session_created,
        &state);

    g_main_loop_run(state.glib_loop);

    if (state.failed || state.pipewire_fd < 0) {
        fprintf(stderr, "\nAlgo fallo antes de llegar a PipeWire. Revisa el mensaje de "
                        "error de arriba.\n");
        return 1;
    }

    run_pipewire_capture(&state);

    if (state.session) xdp_session_close(state.session);
    if (state.session) g_object_unref(state.session);
    if (state.portal) g_object_unref(state.portal);
    g_main_loop_unref(state.glib_loop);

    if (state.failed || state.frame_count == 0) {
        fprintf(stderr, "\nNo se llego a grabar nada.\n");
        return 1;
    }

    return 0;
}
