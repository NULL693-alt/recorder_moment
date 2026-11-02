// ============================================================================
// recorder_moment — grabador de clips estilo Medal (Windows)
//
// Captura la pantalla con DXGI Desktop Duplication, la codifica con la GPU
// (FFmpeg), le suma el audio (WASAPI) y mantiene en memoria los ultimos N
// segundos. Al apretar F9 guarda ese pedazo en un .mp4.
//
// Por defecto abre la interfaz grafica. Con --cli corre sin ventana (util para
// scripts o si prefieres la consola).
//
// Uso:  recorder.exe                        abre la interfaz grafica
//       recorder.exe --cli [args]           modo consola
//       recorder.exe --cli [carpeta] [monitor] [fps] [kbps] [segundos] [--no-audio]
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "config.h"
#include "gui.h"
#include "recorder.h"

// El .exe es de subsistema "ventana" (no abre consola). Si nos invocan desde
// una consola (modo --cli o --help) nos pegamos a la del padre para poder
// escribir ahi.
static void attach_console() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    freopen("CONIN$", "r", stdin);
}

static void print_usage() {
    fprintf(stderr,
            "Uso: recorder.exe                        abre la interfaz grafica\n"
            "     recorder.exe --cli [args]           modo consola (sin ventana)\n"
            "\n"
            "Argumentos del modo consola:\n"
            "  carpeta_salida  donde guardar los clips (por defecto: el que este\n"
            "                  guardado en recorder_config.ini, o la carpeta actual)\n"
            "  monitor         indice del monitor (por defecto 0)\n"
            "  fps             cuadros por segundo objetivo (por defecto 60)\n"
            "  kbps            bitrate de video en kbps (por defecto 12000)\n"
            "  segundos        cuantos segundos guarda el clip (por defecto 120)\n"
            "  --no-audio      graba solo video (por defecto graba sistema + microfono)\n");
}

static int app_main(int argc, char **argv) {
    bool cli = false;
    bool help = false;
    bool audio = true;
    std::vector<std::string> args;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") help = true;
        else if (a == "--cli") cli = true;
        else if (a == "--no-audio") audio = false;
        else args.push_back(a);
    }

    if (help) {
        attach_console();
        print_usage();
        return 0;
    }

    // Una sola instancia: dos grabadores a la vez capturarian todo dos veces y
    // cada F9 guardaria dos clips. El mutex vive hasta que el proceso termina.
    HANDLE instance_mutex = CreateMutexW(nullptr, FALSE, L"Local\\recorder_moment_instance");
    if (instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (cli) {
            attach_console();
            fprintf(stderr, "recorder_moment ya esta abierto; cerralo antes de usar --cli.\n");
        } else {
            // Traemos al frente la ventana que ya estaba (aunque este en la bandeja).
            HWND other = FindWindowW(RECORDER_WINDOW_CLASS, nullptr);
            if (other) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, WM_RECORDER_SHOW, 0, 0);
            }
        }
        CloseHandle(instance_mutex);
        return 0;
    }

    // Modo por defecto: interfaz grafica.
    if (!cli) return run_gui();

    // Modo consola: los ajustes guardados son la base y los argumentos pisan
    // solo lo que se haya pasado.
    attach_console();

    AppConfig cfg;
    config_load(cfg, config_default_path());
    if (args.size() > 0) cfg.out_dir = args[0];
    if (args.size() > 1) { int v = atoi(args[1].c_str()); if (v >= 0) cfg.monitor = v; }
    if (args.size() > 2) { int v = atoi(args[2].c_str()); if (v > 0) cfg.fps = v; }
    if (args.size() > 3) { int v = atoi(args[3].c_str()); if (v > 0) cfg.kbps = v; }
    if (args.size() > 4) { int v = atoi(args[4].c_str()); if (v > 0) cfg.clip_seconds = v; }
    cfg.audio_enabled = audio;

    Recorder::install_signal_handler();

    Recorder recorder(cfg);
    return recorder.run() ? 0 : 1;
}

int main(int argc, char **argv) { return app_main(argc, argv); }

// El ejecutable se linkea como subsistema "ventana" (-mwindows), asi que
// Windows busca WinMain. Le pasamos los argumentos reales del proceso.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return app_main(__argc, __argv); }
