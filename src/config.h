// ============================================================================
// config.h â€” ajustes del programa
//
// Todo lo que se puede tocar desde la interfaz: carpeta de salida, monitor,
// fps, bitrate, duracion del clip, audio (sistema/microfono), que dispositivos
// usar, el atajo para guardar, etc. Se guarda en
// %APPDATA%\recorder_moment\recorder_config.ini para no tener que reconfigurar
// cada vez (y para que funcione aunque el .exe este en una carpeta donde no se
// puede escribir, como Archivos de programa).
// ============================================================================
#pragma once

#include <string>

// Teclas modificadoras del atajo (se combinan con |).
enum HotkeyMods { kModCtrl = 1, kModAlt = 2, kModShift = 4 };

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
    bool separate_audio_tracks = false; // ademas de la mezcla, sistema y micro por separado
    std::string output_device;   // id WASAPI de salida; vacio = predeterminado
    std::string mic_device;      // id WASAPI del microfono; vacio = predeterminado
    bool minimize_to_tray = false; // cerrar la ventana la manda a la bandeja
    bool record_on_start = false;  // empezar a grabar apenas se abre el programa
    bool clip_sound = true;        // sonido al guardar un clip (o al fallar)
    bool record_cursor = true;     // dibujar el cursor del mouse en el video
    bool clips_by_game = true;     // cada clip en una carpeta con el nombre del juego
    bool trim_delete_original = true; // al recortar, el original va a la Papelera
    int hotkey_vk = 0x78;          // tecla para guardar un clip (VK_F9)
    int hotkey_mods = 0;           // HotkeyMods que hay que tener apretados
};

// Ruta del archivo de config: %APPDATA%\recorder_moment\recorder_config.ini.
std::string config_default_path();

// La ruta vieja (recorder_config.ini al lado del .exe), de antes de usar
// %APPDATA%. Solo se lee, para no perder los ajustes al actualizar.
std::string config_legacy_path();

// Carpeta de clips por defecto para quien arranca de cero: Videos\recorder_moment.
std::string config_default_clips_dir();

// Lee el archivo. Si no existe (o esta corrupto) deja los valores que haya y
// devuelve false.
bool config_load(AppConfig &cfg, const std::string &path);

// Lee los ajustes de donde esten: %APPDATA%, o si todavia no hay, los viejos al
// lado del .exe; si no hay ninguno, valores de fabrica (con la carpeta de clips
// en Videos). Siempre se guardan despues en config_default_path().
void config_load_default(AppConfig &cfg);

// Escribe el archivo entero de una vez (si se corta a la mitad, queda el
// anterior). Devuelve false si no se pudo.
bool config_save(const AppConfig &cfg, const std::string &path);

// Nombre legible de un atajo, por ejemplo "F9" o "Ctrl+Shift+S".
std::string hotkey_name(int vk, int mods);
