// ============================================================================
// config.h — ajustes del programa (se guardan en un archivo al lado del .exe)
//
// Todo lo que se puede tocar desde la interfaz: carpeta de salida, monitor,
// fps, bitrate, duracion del clip, audio (sistema/microfono) y que
// dispositivos usar. Se guarda en recorder_config.ini para no tener que
// reconfigurar cada vez.
// ============================================================================
#pragma once

#include <string>

struct AppConfig {
    std::string out_dir = ".";   // carpeta donde se guardan los clips
    int monitor = 0;             // indice del monitor
    int fps = 60;                // cuadros por segundo objetivo
    int kbps = 12000;            // bitrate de video
    int clip_seconds = 120;      // cuantos segundos guarda el clip
    int audio_kbps = 160;        // bitrate del audio (AAC)
    int mic_volume = 100;        // volumen del microfono, en % (0..200)
    int out_height = 0;          // altura del video de salida; 0 = nativa
    std::string video_codec = "h264"; // "h264" o "av1"
    bool audio_enabled = true;   // grabar audio (sistema + microfono)
    bool mic_enabled = true;     // sumar el microfono a la mezcla
    std::string output_device;   // id WASAPI de salida; vacio = predeterminado
    std::string mic_device;      // id WASAPI del microfono; vacio = predeterminado
    bool minimize_to_tray = false; // arrancar minimizado en la bandeja
    bool clip_sound = true;        // sonido al guardar un clip (o al fallar)
    bool record_cursor = true;     // dibujar el cursor del mouse en el video
};

// Ruta del archivo de config (recorder_config.ini, al lado del ejecutable).
std::string config_default_path();

// Si el archivo no existe (o esta corrupto) se dejan los valores por defecto.
void config_load(AppConfig &cfg, const std::string &path);
void config_save(const AppConfig &cfg, const std::string &path);
