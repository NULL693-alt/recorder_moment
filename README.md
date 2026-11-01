# recorder_moment

Grabador de clips estilo Medal: mantiene en memoria los ultimos N segundos de
lo que pasa en el monitor y, al apretar una tecla, guarda ese pedazo en un
`.mp4`. Graba **video + audio** (el sonido del sistema y tu microfono, en la
misma pista), tambien graba HDR (convirtiendolo a SDR) y funciona al mismo
tiempo que OBS, Discord o la Xbox Game Bar.

Trae una **interfaz grafica** (Dear ImGui sobre Direct3D 11): se abre una
ventana con el panel de ajustes y los botones de **Iniciar / Detener /
Guardar clip**, y se puede mandar a la **bandeja del sistema** (junto al
reloj). No hace falta consola ni tocar la linea de comandos.

Originalmente este proyecto era para Linux (CachyOS): la captura se hacia con
el portal XDG (`libportal` + PipeWire) y se codificaba en la GPU Intel con
VAAPI (`h264_vaapi`). Esa version ya no existe; el proyecto ahora es **solo
para Windows**.

Lo que cambio al migrar:

| Linux                                | Windows                                        |
| ------------------------------------ | ---------------------------------------------- |
| portal XDG + PipeWire (captura)      | DXGI Desktop Duplication (DirectX 11)          |
| `h264_vaapi` (GPU Intel)             | `h264_qsv` / `h264_nvenc` / `h264_amf` / `libx264` |
| `signal(SIGINT)` para cortar         | `SetConsoleCtrlHandler` (Ctrl+C)               |

El codificador se elige solo: prueba `h264_qsv` (Intel Quick Sync), despues
`h264_nvenc` (NVIDIA), despues `h264_amf` (AMD) y, si ninguno de esos anda,
cae a `libx264` (CPU).

## Como funciona

Mientras corre, el programa:

1. Captura cada frame con **DXGI Desktop Duplication** (`dxgi_capture.*`).
2. Lo codifica con la GPU usando **FFmpeg** (`video_encoder.*`). Si el monitor
   esta en **HDR** pide el frame en formato scRGB (`R16G16B16A16_FLOAT`) y lo
   convierte a SDR 8 bits con una curva sRGB.
3. En paralelo captura el **audio** con **WASAPI** (`audio_capture.*`): el
   sonido del sistema (loopback, o sea todo lo que suena) y el microfono, los
   mezcla en una sola pista (48 kHz, estereo) y la codifica en **AAC**
   (`audio_encoder.*`).
4. Guarda los paquetes H.264 y AAC ya comprimidos en un **buffer circular**
   (`clip_buffer.*`) con dos colas, que se podan solas para no crecer mas alla
   de la ventana pedida.
5. Cuando aprietas **F9**, busca el keyframe mas cercano al inicio de la ventana
   y escribe los ultimos N segundos de video y audio sincronizados en un `.mp4`
   nuevo.

`recorder.*` es el que junta todo: corre el bucle, maneja el atajo y el Ctrl+C.

## Estructura

```
src/
  main.cpp          arranque, modo GUI y modo --cli (linea de comandos)
  gui.*             ventana (Dear ImGui + Direct3D 11) y bandeja del sistema
  config.*          ajustes persistentes (recorder_config.ini)
  audio_devices.*   enumerar microfonos y dispositivos de salida
  dxgi_capture.*    captura de pantalla (DirectX 11)
  video_encoder.*   codificacion y conversion de color (FFmpeg)
  audio_capture.*   captura de audio (WASAPI: sistema + microfono)
  audio_encoder.*   codificacion de audio (AAC, FFmpeg)
  clip_buffer.*     buffer circular de los ultimos N segundos
  recorder.*        orquesta todo y maneja los atajos
third_party/
  imgui/            Dear ImGui (solo el core + backends de Win32 y D3D11)
resources/
  recorder.rc       icono y metadatos de version del .exe
  app.ico           icono (ventana, bandeja y explorador)
```

## Requisitos

- Windows 10 u 11 (64 bits).
- Una GPU. Con la Intel Arc B580 que esta en esta maquina se usa `h264_qsv`
  (Quick Sync), o sea que la codificacion no carga la CPU.
- Un dispositivo de salida de audio (para el sonido del sistema) y, si queres
  que se escuche tu voz, un microfono. Si no hay microfono igual graba, solo
  que sin tu voz.
- **No hace falta instalar nada mas**: el compilador y FFmpeg ya vienen
  dentro de `tools/` (ver abajo).

## Herramientas incluidas en `tools/`

El proyecto trae todo lo que necesita para compilar, asi no hay que instalar
nada ni tocar variables de entorno del sistema:

- `tools/mingw64/` — MinGW-w64 (g++ 16.2.0) con `mingw32-make` y `cmake`.
- `tools/ffmpeg/` — FFmpeg: headers en `include/`, librerias de importacion en
  `lib/` y las DLLs en `bin/`.

Si alguna vez borras `tools/` y queres reconstruirlo, se pueden bajar de:

- MinGW-w64: <https://github.com/brechtsanders/winlibs_mingw/releases>
  (build `x86_64-posix-seh-ucrt`, la variante `.7z`).
- FFmpeg: <https://github.com/BtbN/FFmpeg-Builds/releases> (build *shared*).

## Compilar

Desde PowerShell, parado en la carpeta del proyecto:

```powershell
# 1. Poner el compilador que viene en tools/ al frente del PATH
$env:Path = "$PWD\tools\mingw64\bin;" + $env:Path

# 2. Configurar el proyecto (hay que rehacerlo si borras la carpeta build/)
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++ -DCMAKE_MAKE_PROGRAM=mingw32-make

# 3. Compilar
cmake --build build
```

Queda `build\recorder.exe`, y las DLLs de FFmpeg se copian solas al lado del
ejecutable (por eso se puede mover la carpeta `build/` a otra maquina y corre
igual).

## Ejecutar

### Interfaz grafica (por defecto)

Doble clic en `build\recorder.exe` (o desde PowerShell, `.\build\recorder.exe`).
Se abre una ventana con:

- **Carpeta de clips**: donde se guardan los `.mp4` (boton *Carpeta...* para
  elegirla).
- **Monitor**, **FPS**, **Bitrate de video** y **Duracion del buffer**
  (segundos).
- **Resolucion de salida**: *Nativa* (la del monitor) o una altura menor
  (2160p/1440p/1080p/720p/480p/360p) para que los clips pesen menos.
- **Grabar audio** y **Grabar microfono**, mas dos desplegables para elegir el
  dispositivo de salida y el **microfono** (con *Refrescar dispositivos*),
  un **volumen del microfono** (0–200%) y el **bitrate de audio** (64–320
  kbps).
- **Iniciar** / **Detener** y **Guardar clip (F9)**, con el estado en vivo
  (frames capturados y cuanto llena el buffer).
- **Minimizar a la bandeja al cerrar**: si esta marcado, cerrar la ventana no
  sale del programa, lo deja en la **bandeja del sistema** (icono junto al
  reloj). Desde ahi, click derecho para *Mostrar*, *Iniciar/Detener*,
  *Guardar clip*, *Abrir carpeta de clips* o *Salir*.
- **Iniciar con Windows**: si esta marcado, el programa se abre solo al
  encender la PC. Para que ademas arranque escondido y grabando, marca tambien
  *Minimizar a la bandeja al cerrar*. Se puede desactivar destildando la
  casilla (no toca nada mas de tu sistema).

Los ajustes se guardan solos en `recorder_config.ini`, al lado del `.exe`, asi
que la proxima vez arranca como lo dejaste.

### Linea de comandos (`--cli`)

Si preferis la version de consola (sin ventana), pasa `--cli`:

```powershell
.\build\recorder.exe --cli                                 # carpeta actual, monitor 0, 60 fps, 12000 kbps, 120 s, con audio
.\build\recorder.exe --cli clips                           # guarda en la carpeta clips\
.\build\recorder.exe --cli clips 1                         # el monitor 1
.\build\recorder.exe --cli clips 1 60 20000 60             # monitor 1, 60 fps, 20 Mbps, clips de 60 s
.\build\recorder.exe --cli clips 0 60 12000 120 --no-audio # solo video, sin audio
```

Los argumentos son, en orden:

```
recorder.exe --cli [carpeta_salida] [monitor] [fps] [kbps] [segundos] [--no-audio]
```

Todos son opcionales; los valores por defecto son la **carpeta actual**,
monitor `0`, `60` fps, `12000` kbps y clips de **120 segundos**. El audio
(sistema + microfono) viene **activado por defecto**; `--no-audio` lo apaga.

Mientras graba te muestra el estado en la consola:

```
[captura] monitor #0: 1600x900, formato SDR
Capturando el monitor #0. Con audio (sistema + microfono). Apreta F9 para guardar los ultimos 120 s en un clip. Ctrl+C para salir.
[audio] 48000 Hz, 2 canales (sistema + microfono)
[audio] codificador: aac @ 48000 Hz, 160 kbps
[ffmpeg] listo: 1600x900 @ 60 fps, 12000 kbps (codificador: h264_qsv)
[grabando] 3600 frames | buffer 60 s / 120 s | F9 = guardar clip | Ctrl+C = salir
```

- **F9** → guarda los ultimos N segundos en un `.mp4` con nombre
  `clip_AAAAMMDD_HHMMSS.mp4`.
- **Ctrl+C** → sale limpiamente.

```
[clip] guardando ultimos 120 s en .\clip_20261031_220630.mp4 ...
[clip] listo: .\clip_20261031_220630.mp4 (120 s, 7200 frames)
```

## Compartir (carpeta portable)

Para pasarle el programa a alguien, corre `package.ps1`: arma una carpeta
portable con el `.exe` y las DLLs de FFmpeg, y un `.zip` listo para mandar:

```powershell
$env:Path = "$PWD\tools\mingw64\bin;" + $env:Path
cmake --build build
.\package.ps1
```

Queda en `dist\recorder_moment\` (carpeta) y `dist\recorder_moment.zip`. La otra
persona solo descomprime y hace doble clic en `recorder.exe`: no necesita
instalar nada.

## Cosas a tener en cuenta

- **Desktop Duplication captura monitores enteros, no ventanas sueltas.** No
  se puede grabar "solo el juego" sin agregar recorte despues.
- Si un juego esta en **pantalla completa exclusiva**, algunos drivers no
  entregan los frames. Usa **pantalla completa sin bordes** (borderless).
- **HDR:** si el monitor esta en HDR, el programa pide el frame en scRGB y lo
  convierte a SDR (H.264). No es un HDR "real" (no se guarda con metadatos
  HDR), pero el video sale con los colores correctos y se ve en cualquier
  reproductor.
- **Audio:** se graba el sonido del sistema (todo lo que suena: juego, musica,
  Discord, videos) mas el microfono por defecto, mezclados en **una sola pista
  AAC**. Si no hay microfono, sigue andando solo con el sistema. Si no hay
  ningun dispositivo de audio, el programa avisa y graba solo video. Con
  `--no-audio` ni siquiera intenta capturar audio.
- **Varias capturas a la vez:** en Windows 10/11 se pueden tener varias
  capturas del mismo monitor simultaneamente, asi que el programa puede correr
  mientras OBS, Discord o la Game Bar estan capturando. Si el sistema responde
  "no se puede ahora" (`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`), el programa
  reintenta solo unas cuantas veces en vez de abortar.
- Si cambia la resolucion del monitor en medio de una grabacion, la captura se
  reconecta sola; si el tamano cambio, avisa y corta (no se puede cambiar el
  tamano del video a mitad de camino).
