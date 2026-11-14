// ============================================================================
// clip_editor.h — recortar clips y verlos antes de recortar
//
//   * probe_clip / trim_clip: leen un .mp4 y escriben un pedazo [inicio, fin]
//     sin volver a codificar (copia los paquetes tal cual). Es instantaneo y no
//     pierde calidad; a cambio, el inicio cae en un cuadro clave (keyframe), que
//     en nuestros clips hay uno cada 2 s. Se copian todas las pistas de audio.
//   * ClipPlayer: reproductor sencillo para la pestaña "Editor" (video a una
//     resolucion chica para la vista previa + la primera pista de audio por los
//     parlantes). Decodifica en su propio hilo, asi la ventana no se traba.
//
// No depende de la interfaz: las pruebas automaticas lo usan directo.
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ClipInfo {
    double duration = 0.0;        // segundos
    int width = 0, height = 0;    // del video original
    double fps = 0.0;
    std::vector<double> keyframes; // segundos desde el inicio, ordenados
    int audio_tracks = 0;
    std::string video_codec;      // "h264", "av1"...
};

// Lee la informacion de un clip. false (y err) si no se puede abrir.
bool probe_clip(const std::string &path, ClipInfo &info, std::string *err = nullptr);

// El ultimo keyframe que no pasa de t (donde de verdad va a empezar un recorte
// que empiece en t).
double keyframe_at_or_before(const ClipInfo &info, double t);

// "clip_x.mp4" -> "clip_x_recorte.mp4" (o _recorte_2, _recorte_3... si ya existe).
std::string make_trim_path(const std::string &in);

// Donde va el recorte de `clip`: <clips>\Clips Edit\<juego>\<mismo nombre>.mp4
// (el juego sale de la subcarpeta donde estaba el clip). Si el clip ya estaba en
// "Clips Edit", queda en esa misma carpeta. Nunca devuelve un archivo que ya
// exista (agrega _2, _3...).
std::string edited_clip_path(const std::string &clip, const std::string &clips_dir);

// Escribe en `out` el pedazo [start, end] (segundos) de `in`. El inicio se
// corre al keyframe anterior. out_duration = cuanto dura lo que se escribio.
bool trim_clip(const std::string &in, const std::string &out, double start, double end,
               std::string *err = nullptr, double *out_duration = nullptr);

class ClipPlayer {
public:
    ClipPlayer();
    ~ClipPlayer();
    ClipPlayer(const ClipPlayer &) = delete;
    ClipPlayer &operator=(const ClipPlayer &) = delete;

    // Abre el clip y muestra el primer cuadro. La vista previa se escala para
    // entrar en max_w x max_h.
    bool open(const std::string &path, int max_w, int max_h, std::string *err = nullptr);
    void close();
    bool is_open() const;
    const ClipInfo &info() const;

    void play();
    void pause();
    bool playing() const;
    // Reproduce de `start` a `end` y se pausa solo al llegar.
    void play_range(double start, double end);
    // Salta a t (segundos). Se pueden pedir muchos seguidos (al arrastrar): se
    // atiende siempre el ultimo.
    void seek(double t);
    double position() const;
    bool has_audio() const; // hay pista de audio y se pudo abrir la salida

    // Si hay un cuadro nuevo desde la ultima vez, lo copia (BGRA) y devuelve true.
    bool take_frame(std::vector<uint8_t> &bgra, int &w, int &h);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
