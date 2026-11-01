// ============================================================================
// recorder_moment — grabador de clips estilo Medal (Windows)
//
// Captura la pantalla con DXGI Desktop Duplication, la codifica con la GPU
// (FFmpeg) y mantiene en memoria los ultimos N segundos. Al apretar F9 guarda
// ese pedazo en un .mp4. Ctrl+C para salir.
//
// Este archivo es solo el arranque: el trabajo esta repartido en clases:
//   dxgi_capture.*   captura de pantalla (DirectX)
//   video_encoder.*  codificacion y conversion de color (FFmpeg)
//   audio_capture.*  captura de audio (WASAPI: sistema + microfono)
//   audio_encoder.*  codificacion de audio (AAC, FFmpeg)
//   clip_buffer.*    buffer circular de los ultimos N segundos
//   recorder.*       junta todo y maneja los atajos
//
// Uso:  recorder.exe [carpeta_salida] [monitor] [fps] [kbps] [segundos] [--no-audio]
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "recorder.h"

static void print_usage() {
    fprintf(stderr,
            "Uso: recorder.exe [carpeta_salida] [monitor] [fps] [kbps] [segundos] "
            "[--no-audio]\n"
            "  carpeta_salida  donde guardar los clips (por defecto: carpeta actual)\n"
            "  monitor         indice del monitor (por defecto 0)\n"
            "  fps             cuadros por segundo objetivo (por defecto 60)\n"
            "  kbps            bitrate de video en kbps (por defecto 12000)\n"
            "  segundos        cuantos segundos guarda el clip (por defecto 120)\n"
            "  --no-audio      graba solo video (por defecto graba sistema + microfono)\n");
}

int main(int argc, char **argv) {
    bool audio = true;
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            print_usage();
            return 0;
        }
        if (a == "--no-audio") {
            audio = false;
            continue;
        }
        args.push_back(a);
    }

    std::string out_dir = ".";
    int monitor = 0;
    int fps = 60;
    int kbps = 12000;
    int secs = 120;

    if (args.size() > 0) out_dir = args[0];
    if (args.size() > 1) monitor = atoi(args[1].c_str());
    if (args.size() > 2) { int v = atoi(args[2].c_str()); if (v > 0) fps = v; }
    if (args.size() > 3) { int v = atoi(args[3].c_str()); if (v > 0) kbps = v; }
    if (args.size() > 4) { int v = atoi(args[4].c_str()); if (v > 0) secs = v; }

    Recorder::install_signal_handler();

    Recorder recorder(monitor, fps, kbps, secs, out_dir, audio);
    return recorder.run() ? 0 : 1;
}
