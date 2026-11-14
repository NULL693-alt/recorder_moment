// ============================================================================
// recorder_tests â€” pruebas automaticas de las partes que no necesitan grabar
// la pantalla de verdad:
//
//   * ClipBuffer: poda sin perder el keyframe, copias independientes, y un
//     clip real (H.264 + 3 pistas de audio) leido de vuelta con FFmpeg.
//   * Config: guardar/leer, migracion de ajustes viejos, valores raros, y
//     nombres de atajos.
//   * GpuConverter: colores BT.709, escalado, HDR y cursor (si hay GPU).
//   * VideoEncoder: AV1 por hardware o, si la GPU no puede, vuelta a H.264.
//   * Editor: recorte sin recodificar (pistas, keyframe, tiempos) y el
//     reproductor de la vista previa.
//
// Se corre con:  ctest --test-dir build   (o build\recorder_tests.exe)
// ============================================================================
#include "audio_encoder.h"
#include "clip_buffer.h"
#include "clip_editor.h"
#include "config.h"
#include "game_detect.h"
#include "gpu_convert.h"
#include "video_encoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

namespace fs = std::filesystem;

static void nap(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---------------------------------------------------------------------------
// Mini runner
// ---------------------------------------------------------------------------
static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            printf("    FALLA %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                          \
    do {                                                                               \
        g_checks++;                                                                    \
        double va_ = (double)(a), vb_ = (double)(b);                                   \
        if (std::fabs(va_ - vb_) > (tol)) {                                            \
            g_failures++;                                                              \
            printf("    FALLA %s:%d: %s = %g, esperado %g\n", __FILE__, __LINE__, #a, va_, \
                   vb_);                                                               \
        }                                                                              \
    } while (0)

static fs::path temp_dir() {
    fs::path d = fs::temp_directory_path() / "recorder_moment_tests";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

// Paquete falso de video (solo importan los tiempos y el flag de keyframe).
static AVPacket *fake_packet(int64_t pts, bool key) {
    AVPacket *p = av_packet_alloc();
    av_new_packet(p, 16);
    p->pts = p->dts = pts;
    if (key) p->flags |= AV_PKT_FLAG_KEY;
    return p;
}

// ---------------------------------------------------------------------------
// ClipBuffer
// ---------------------------------------------------------------------------
static void test_prune_keeps_keyframe() {
    // Un solo keyframe al principio y 30 s de frames comunes: aunque la ventana
    // sea de 5 s, no se puede tirar el keyframe (ni lo que depende de el).
    ClipBuffer buf(5);
    const int fps = 30, total = 30 * fps;
    for (int i = 0; i < total; i++) {
        int64_t pts = (int64_t)i * 1000000 / fps;
        AVPacket *p = fake_packet(pts, i == 0);
        buf.push_video(p, pts, i == 0);
        av_packet_free(&p);
    }
    CHECK(buf.count() == (size_t)total);

    // Con keyframes cada 2 s, en cambio, si se poda (queda la ventana + margen).
    ClipBuffer buf2(5);
    for (int i = 0; i < total; i++) {
        int64_t pts = (int64_t)i * 1000000 / fps;
        bool key = (i % (2 * fps)) == 0;
        AVPacket *p = fake_packet(pts, key);
        buf2.push_video(p, pts, key);
        av_packet_free(&p);
    }
    CHECK(buf2.count() < (size_t)total);
    CHECK(buf2.buffered_seconds() >= 5.0);
    CHECK(buf2.buffered_seconds() <= 5.0 + 15.0 + 2.0);
}

static void test_snapshot_is_independent() {
    ClipBuffer buf(10);
    for (int i = 0; i < 60; i++) {
        AVPacket *p = fake_packet(i * 33333, i % 30 == 0);
        buf.push_video(p, i * 33333, i % 30 == 0);
        av_packet_free(&p);
    }
    std::unique_ptr<ClipBuffer> snap = buf.snapshot();
    buf.clear();
    CHECK(buf.count() == 0);
    CHECK(snap->count() == 60);
}

// Clip de verdad: video H.264 + 3 pistas AAC, guardado y leido con FFmpeg.
static void test_clip_with_three_audio_tracks() {
    const int W = 320, H = 180, FPS = 30;
    VideoEncoder venc;
    if (!venc.open(W, H, FPS, 2000, 0, VideoCodec::H264)) {
        printf("    (sin codificador H.264: se saltea)\n");
        return;
    }
    AudioEncoder aenc[3];
    for (AudioEncoder &a : aenc) CHECK(a.open(48000, 2, 128));

    ClipBuffer buf(4);
    buf.set_audio_tracks(3);
    CHECK(buf.audio_tracks() == 3);

    std::vector<uint8_t> y((size_t)W * H, 100), uv((size_t)W * H / 2, 128);
    std::vector<int16_t> pcm(1600 * 2); // 1/30 s de audio estereo
    for (int f = 0; f < FPS * 8; f++) {
        int64_t pts = (int64_t)f * 1000000 / FPS;
        memset(y.data(), 16 + (f % 200), y.size());
        venc.encode_nv12(y.data(), W, uv.data(), W, pts, [&](const AVPacket *p) {
            buf.push_video(p, p->pts, (p->flags & AV_PKT_FLAG_KEY) != 0);
        });
        for (int t = 0; t < 3; t++) {
            for (size_t i = 0; i < pcm.size(); i++)
                pcm[i] = (int16_t)(3000 * std::sin((f * 1600 + (int)i / 2) * 0.05 * (t + 1)));
            aenc[t].encode(pcm.data(), 1600, [&](const AVPacket *p) {
                buf.push_audio(t, p, (int64_t)p->pts * 1000000 / 48000);
            });
        }
    }

    std::string path = (temp_dir() / "tres_pistas.mp4").u8string();
    std::vector<ClipBuffer::AudioTrack> tracks = {
        {aenc[0].codecpar(), "Mezcla"}, {aenc[1].codecpar(), "Sistema"},
        {aenc[2].codecpar(), "Microfono"}};
    CHECK(buf.save_to(path, venc.codecpar(), venc.time_base(), tracks, aenc[0].time_base()));

    AVFormatContext *in = nullptr;
    CHECK(avformat_open_input(&in, path.c_str(), nullptr, nullptr) == 0);
    if (!in) return;
    avformat_find_stream_info(in, nullptr);
    CHECK(in->nb_streams == 4);
    int audio = 0, defaults = 0;
    std::vector<std::string> titles;
    for (unsigned i = 0; i < in->nb_streams; i++) {
        AVStream *st = in->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        audio++;
        if (st->disposition & AV_DISPOSITION_DEFAULT) defaults++;
        AVDictionaryEntry *e = av_dict_get(st->metadata, "handler_name", nullptr, 0);
        titles.push_back(e ? e->value : "");
    }
    CHECK(audio == 3);
    CHECK(defaults == 1);
    CHECK(titles.size() == 3 && titles[0] == "Mezcla" && titles[2] == "Microfono");

    // El clip arranca en un keyframe y dura lo pedido (4 s, hasta el keyframe
    // anterior, o sea un poco mas).
    double dur = in->duration / (double)AV_TIME_BASE;
    CHECK(dur >= 3.9 && dur <= 6.5);
    AVPacket *pkt = av_packet_alloc();
    bool first_video_is_key = false;
    while (av_read_frame(in, pkt) == 0) {
        bool video = in->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
        bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        av_packet_unref(pkt);
        if (video) {
            first_video_is_key = key;
            break;
        }
    }
    av_packet_free(&pkt);
    avformat_close_input(&in);
    CHECK(first_video_is_key);
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static void test_config_roundtrip() {
    std::string path = (temp_dir() / "config_test.ini").u8string();
    AppConfig a;
    a.out_dir = "D:\\Clips con espacios\\Ã±andÃº";
    a.fps = 30;
    a.kbps = 6000;
    a.video_codec = "av1";
    a.separate_audio_tracks = true;
    a.minimize_to_tray = true;
    a.record_on_start = false;
    a.hotkey_vk = 0x53; // S
    a.hotkey_mods = kModCtrl | kModShift;
    a.monitor = 1;
    CHECK(config_save(a, path));

    AppConfig b;
    CHECK(config_load(b, path));
    CHECK(b.out_dir == a.out_dir);
    CHECK(b.fps == 30 && b.kbps == 6000);
    CHECK(b.video_codec == "av1");
    CHECK(b.separate_audio_tracks);
    CHECK(b.minimize_to_tray && !b.record_on_start);
    CHECK(b.hotkey_vk == 0x53 && b.hotkey_mods == (kModCtrl | kModShift));
    CHECK(b.monitor == 1);
    CHECK(!fs::exists(fs::u8path(path + ".tmp"))); // no queda el temporal
}

static void test_config_migration_and_sanitize() {
    std::string path = (temp_dir() / "config_viejo.ini").u8string();
    {
        FILE *f = fopen(path.c_str(), "w");
        // Config de antes: sin record_on_start, con valores raros.
        fputs("minimize_to_tray=1\nfps=-5\nmic_volume=999\nhotkey_vk=0\nvideo_codec=xyz\n", f);
        fclose(f);
    }
    AppConfig c;
    CHECK(config_load(c, path));
    CHECK(c.record_on_start);      // antes "bandeja" tambien grababa al abrir
    CHECK(c.fps == 60);
    CHECK(c.mic_volume == 200);
    CHECK(c.hotkey_vk == 0x78);    // F9
    CHECK(c.video_codec == "h264");

    AppConfig d;
    CHECK(!config_load(d, (temp_dir() / "no_existe.ini").u8string()));
}

static void test_hotkey_names() {
    CHECK(hotkey_name(0x78, 0) == "F9");
    CHECK(hotkey_name(0x7B, kModAlt) == "Alt+F12");
    std::string s = hotkey_name(0x53, kModCtrl | kModShift);
    CHECK(s.rfind("Ctrl+Shift+", 0) == 0);
    CHECK(s.size() > 11);
}

// ---------------------------------------------------------------------------
// GpuConverter
// ---------------------------------------------------------------------------
static void test_gpu_convert() {
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        printf("    (sin D3D11: se saltea)\n");
        return;
    }
    const int W = 64, H = 32;
    std::vector<uint8_t> px(W * H * 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = &px[(y * W + x) * 4];
            bool red = x < W / 2;
            p[0] = red ? 0 : 255;
            p[1] = red ? 0 : 255;
            p[2] = 255;
            p[3] = 255;
        }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = W;
    d.Height = H;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), W * 4, 0};
    ID3D11Texture2D *tex = nullptr;
    dev->CreateTexture2D(&d, &init, &tex);

    for (int scaled = 0; scaled < 2; scaled++) {
        int ow = scaled ? W / 2 : W, oh = scaled ? H / 2 : H;
        GpuConverter conv;
        CHECK(conv.open(dev, ctx, tex, W, H, ow, oh, 0.0f));
        int got = 0;
        int64_t last = -1;
        auto fn = [&](const uint8_t *y, int yp, const uint8_t *uv, int uvp, int64_t pts) {
            last = pts;
            if (got++ > 0) return true;
            int r = oh / 2;
            CHECK_NEAR(y[r * yp + 2], 63, 2);        // rojo
            CHECK_NEAR(y[r * yp + ow - 3], 235, 2);  // blanco
            CHECK_NEAR(uv[(r / 2) * uvp + 2], 102, 2);
            CHECK_NEAR(uv[(r / 2) * uvp + 3], 240, 2);
            return true;
        };
        for (int i = 0; i < 5; i++) conv.submit(1000 * i, fn);
        CHECK(got == 3);   // la cola entrega con 2 frames de atraso
        CHECK(last == 2000);

        if (!scaled) { // cursor: pixel negro opaco en (40,4)
            std::vector<uint8_t> cur(4 * 4 * 4, 0);
            cur[3] = 255;
            CHECK(conv.set_cursor_shape(cur.data(), 4, 4));
            conv.set_cursor_pos(40, 4, true);
            int seen = 0;
            auto fc = [&](const uint8_t *y, int yp, const uint8_t *, int, int64_t pts) {
                if (pts == 10000) {
                    seen++;
                    CHECK_NEAR(y[4 * yp + 40], 16, 2);
                    CHECK_NEAR(y[4 * yp + 44], 235, 2);
                }
                return true;
            };
            conv.submit(10000, fc);
            conv.submit(10001, fc);
            conv.submit(10002, fc);
            CHECK(seen == 1);
        }
    }
    // Otra proporcion (el juego cambio la resolucion): 64x32 dentro de 64x64 va
    // centrada, con franjas negras arriba y abajo, sin deformarse.
    {
        GpuConverter conv;
        CHECK(conv.open(dev, ctx, tex, W, H, 64, 64, 0.0f));
        int got = 0;
        auto fn = [&](const uint8_t *y, int yp, const uint8_t *uv, int uvp, int64_t) {
            if (got++ > 0) return true;
            CHECK_NEAR(y[4 * yp + 10], 16, 1);        // franja de arriba: negro
            CHECK_NEAR(y[60 * yp + 50], 16, 1);       // franja de abajo
            CHECK_NEAR(uv[2 * uvp + 10], 128, 1);     // sin color
            CHECK_NEAR(uv[2 * uvp + 11], 128, 1);
            CHECK_NEAR(y[32 * yp + 2], 63, 2);        // en el medio, la imagen: rojo
            CHECK_NEAR(y[32 * yp + 61], 235, 2);      // y blanco
            CHECK_NEAR(y[20 * yp + 2], 63, 2);        // la imagen ocupa de la fila 16 a la 47
            CHECK_NEAR(y[44 * yp + 61], 235, 2);
            return true;
        };
        for (int i = 0; i < 3; i++) conv.submit(i, fn);
        CHECK(got == 1);
    }
    tex->Release();
    ctx->Release();
    dev->Release();
}

// ---------------------------------------------------------------------------
// VideoEncoder: AV1 o vuelta a H.264
// ---------------------------------------------------------------------------
static void test_av1_or_fallback() {
    VideoEncoder enc;
    CHECK(enc.open(640, 360, 30, 3000, 0, VideoCodec::AV1));
    if (!enc.is_open()) return;
    printf("    codificador: %s (%s)\n", enc.name(), video_codec_name(enc.codec()));
    std::vector<uint8_t> y(640 * 360, 80), uv(640 * 180, 128);
    int packets = 0, keys = 0;
    for (int f = 0; f < 150; f++) { // 5 s
        memset(y.data(), 16 + (f * 3) % 200, y.size());
        enc.encode_nv12(y.data(), 640, uv.data(), 640, (int64_t)f * 33333, [&](const AVPacket *p) {
            packets++;
            if (p->flags & AV_PKT_FLAG_KEY) keys++;
        });
    }
    CHECK(packets > 100);
    CHECK(keys >= 2 && keys <= 4); // uno cada 2 s
}

// ---------------------------------------------------------------------------
// Editor: recorte sin recodificar (usa el clip de la prueba de 3 pistas)
// ---------------------------------------------------------------------------
static void test_trim_clip() {
    std::string in = (temp_dir() / "tres_pistas.mp4").u8string();
    ClipInfo info;
    if (!probe_clip(in, info)) {
        printf("    (no esta el clip de la prueba de 3 pistas: se saltea)\n");
        return;
    }
    CHECK(info.audio_tracks == 3);
    CHECK(info.width == 320 && info.height == 180);
    CHECK(info.keyframes.size() >= 2);      // uno cada 2 s
    CHECK_NEAR(info.keyframes[0], 0.0, 0.05);
    CHECK_NEAR(keyframe_at_or_before(info, 3.0), 2.0, 0.1);
    CHECK_NEAR(keyframe_at_or_before(info, 1.9), 0.0, 0.05);

    // El nombre del recorte no pisa nada.
    std::string out = make_trim_path(in);
    CHECK(out.find("tres_pistas_recorte") != std::string::npos);
    std::error_code ec;
    fs::remove(fs::u8path(out), ec);

    // Pido de 2.5 a 4.0: arranca en el keyframe de 2.0, o sea ~2 s.
    double dur = 0;
    std::string err;
    CHECK(trim_clip(in, out, 2.5, 4.0, &err, &dur));
    CHECK_NEAR(dur, 2.0, 0.15);

    ClipInfo o;
    CHECK(probe_clip(out, o));
    CHECK(o.audio_tracks == 3);
    CHECK_NEAR(o.duration, 2.0, 0.15);

    AVFormatContext *ic = nullptr;
    CHECK(avformat_open_input(&ic, out.c_str(), nullptr, nullptr) == 0);
    if (!ic) return;
    avformat_find_stream_info(ic, nullptr);
    std::vector<std::string> titles;
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        AVDictionaryEntry *e = av_dict_get(st->metadata, "handler_name", nullptr, 0);
        titles.push_back(e ? e->value : "");
    }
    CHECK(titles.size() == 3 && titles[0] == "Mezcla"); // los nombres de las pistas siguen
    AVPacket *pkt = av_packet_alloc();
    bool first_key = false, seen = false;
    double first_audio = -1;
    while (av_read_frame(ic, pkt) == 0) {
        AVStream *st = ic->streams[pkt->stream_index];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !seen) {
            seen = true;
            first_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && first_audio < 0)
            first_audio = pkt->pts * av_q2d(st->time_base);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&ic);
    CHECK(first_key);
    CHECK(first_audio >= -0.05 && first_audio < 0.1); // el audio arranca junto con el video

    // Tramos sin sentido: error y no queda archivo.
    std::string bad = (temp_dir() / "recorte_malo.mp4").u8string();
    CHECK(!trim_clip(in, bad, 3.0, 3.05, &err));
    CHECK(!fs::exists(fs::u8path(bad)));
    CHECK(!trim_clip(in, in, 0.0, 2.0, &err)); // nunca pisa el original
    fs::remove(fs::u8path(out), ec);
}

// Carpetas: nombre del juego y destino de los recortes.
static void test_folders() {
    CHECK(sanitize_folder_name("VALORANT  ") == "VALORANT");
    CHECK(sanitize_folder_name("Juego: \"El Regreso\"? <2>") == "Juego El Regreso 2");
    CHECK(sanitize_folder_name("Counter-Strike\xC2\xAE 2") == "Counter-Strike 2");
    CHECK(sanitize_folder_name("con") == "con_");
    CHECK(sanitize_folder_name("...") == "");
    CHECK(sanitize_folder_name(std::string(80, 'a')).size() == 50);

    fs::path root = temp_dir() / "carpetas";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "VALORANT", ec);
    std::string r = root.u8string();

    // Clip de un juego -> Clips Edit\VALORANT\mismo nombre.
    fs::path got = fs::u8path(edited_clip_path((root / "VALORANT" / "clip_1.mp4").u8string(), r));
    CHECK(got == root / "Clips Edit" / "VALORANT" / "clip_1.mp4");
    // Clip suelto en la carpeta -> Clips Edit\.
    got = fs::u8path(edited_clip_path((root / "clip_2.mp4").u8string(), r));
    CHECK(got == root / "Clips Edit" / "clip_2.mp4");
    // Un recorte que se vuelve a recortar queda en su carpeta, con otro nombre.
    fs::create_directories(root / "Clips Edit" / "VALORANT", ec);
    { FILE *f = fopen((root / "Clips Edit" / "VALORANT" / "clip_1.mp4").u8string().c_str(), "wb"); if (f) fclose(f); }
    got = fs::u8path(edited_clip_path((root / "Clips Edit" / "VALORANT" / "clip_1.mp4").u8string(), r));
    CHECK(got == root / "Clips Edit" / "VALORANT" / "clip_1_2.mp4");
    // Un clip de otra carpeta -> Clips Edit\ de la carpeta de clips.
    got = fs::u8path(edited_clip_path((temp_dir() / "otro.mp4").u8string(), r));
    CHECK(got == root / "Clips Edit" / "otro.mp4");
    fs::remove_all(root, ec);
}

// El reproductor abre, muestra el primer cuadro y salta.
static void test_player() {
    std::string in = (temp_dir() / "tres_pistas.mp4").u8string();
    ClipPlayer p;
    if (!p.open(in, 160, 90)) {
        printf("    (no esta el clip de prueba: se saltea)\n");
        return;
    }
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    bool got = false;
    for (int i = 0; i < 200 && !got; i++) {
        got = p.take_frame(px, w, h);
        if (!got) nap(10);
    }
    CHECK(got);
    CHECK(w == 160 && h == 90);
    CHECK(px.size() == (size_t)160 * 90 * 4);

    p.seek(3.0);
    got = false;
    for (int i = 0; i < 300 && !got; i++) {
        got = p.take_frame(px, w, h) && std::fabs(p.position() - 3.0) < 0.05;
        if (!got) nap(10);
    }
    CHECK(got);
    CHECK_NEAR(p.position(), 3.0, 0.05);

    p.play();
    nap(400);
    CHECK(p.position() > 3.1);
    p.pause();
    p.close();
    CHECK(!p.is_open());
}

// ---------------------------------------------------------------------------
int main() {
    struct {
        const char *name;
        void (*fn)();
    } tests[] = {
        {"buffer: la poda no tira el keyframe", test_prune_keeps_keyframe},
        {"buffer: la copia es independiente", test_snapshot_is_independent},
        {"buffer: clip H.264 con 3 pistas de audio", test_clip_with_three_audio_tracks},
        {"config: guardar y leer", test_config_roundtrip},
        {"config: ajustes viejos y valores raros", test_config_migration_and_sanitize},
        {"config: nombres de atajos", test_hotkey_names},
        {"gpu: colores, escalado y cursor", test_gpu_convert},
        {"video: AV1 o vuelta a H.264", test_av1_or_fallback},
        {"editor: recortar sin recodificar", test_trim_clip},
        {"editor: reproductor", test_player},
        {"carpetas: por juego y Clips Edit", test_folders},
    };
    av_log_set_level(AV_LOG_ERROR);
    for (const auto &t : tests) {
        int before = g_failures;
        printf("- %s\n", t.name);
        t.fn();
        printf("  %s\n", g_failures == before ? "OK" : "FALLO");
    }
    printf("\n%d comprobaciones, %d fallas\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
