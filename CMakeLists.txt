cmake_minimum_required(VERSION 3.16)
project(recorder_moment CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Genera build/compile_commands.json, que usan VS Code (y otros editores)
# para que el IntelliSense sepa exactamente con que flags se compila cada
# archivo y no marque los #include como "no encontrados".
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT WIN32)
    message(FATAL_ERROR
        "Este proyecto ahora es solo para Windows: la captura usa DXGI Desktop "
        "Duplication y la codificacion usa los codificadores de FFmpeg para "
        "Windows (h264_qsv/nvenc/amf). La version vieja de Linux (PipeWire + "
        "libportal + VAAPI) ya no se compila.")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo de build" FORCE)
endif()

# ---------------------------------------------------------------------------
# FFmpeg
# ---------------------------------------------------------------------------
# Por defecto se usa el FFmpeg que viene dentro de tools/ffmpeg (headers en
# include/, librerias de importacion en lib/ y las DLLs en bin/). Si tenes
# FFmpeg instalado en otro lado, pasale -DFFMPEG_ROOT=C:/ruta/a/ffmpeg.
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg"
    CACHE PATH "Raiz de FFmpeg (debe tener include/, lib/ y bin/)")

set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib")
set(FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin")

if(NOT EXISTS "${FFMPEG_INCLUDE_DIR}/libavformat/avformat.h")
    message(FATAL_ERROR
        "No encuentro FFmpeg en ${FFMPEG_ROOT}.\n"
        "Asegurate de que exista ${FFMPEG_INCLUDE_DIR}/libavformat/avformat.h, "
        "o pasale -DFFMPEG_ROOT=C:/ruta/a/tu/ffmpeg.")
endif()

# MinGW usa libavformat.dll.a y MSVC usa avformat.lib: aceptamos las dos.
set(FFMPEG_LIBS "")
foreach(component avformat avcodec avutil swscale swresample)
    if(EXISTS "${FFMPEG_LIB_DIR}/lib${component}.dll.a")
        list(APPEND FFMPEG_LIBS "${FFMPEG_LIB_DIR}/lib${component}.dll.a")
    elseif(EXISTS "${FFMPEG_LIB_DIR}/${component}.lib")
        list(APPEND FFMPEG_LIBS "${FFMPEG_LIB_DIR}/${component}.lib")
    else()
        message(FATAL_ERROR "No encuentro la libreria ${component} en ${FFMPEG_LIB_DIR}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Ejecutable
# ---------------------------------------------------------------------------
add_executable(recorder
    src/main.cpp
    src/dxgi_capture.cpp
    src/video_encoder.cpp
    src/audio_capture.cpp
    src/audio_encoder.cpp
    src/clip_buffer.cpp
    src/recorder.cpp
)

target_include_directories(recorder PRIVATE "${FFMPEG_INCLUDE_DIR}")

# d3d11/dxgi: la captura.  dxguid/uuid: los IID de COM que usamos con
# __uuidof.  ole32: inicializacion de COM que hace DirectX y WASAPI por
# debajo.  mmdevapi: los endpoints de audio (WASAPI) que usa audio_capture.
target_link_libraries(recorder PRIVATE
    ${FFMPEG_LIBS}
    d3d11
    dxgi
    dxguid
    uuid
    ole32
    mmdevapi
    user32
)

# Linkeamos estaticamente el runtime de MinGW (libstdc++, libgcc, pthread)
# para que el .exe no dependa de DLLs del compilador. FFmpeg sigue siendo
# dinamico (sus .dll se copian al lado del .exe, ver mas abajo).
if(MINGW)
    target_link_options(recorder PRIVATE -static)
endif()

target_compile_options(recorder PRIVATE -Wall)

# ---------------------------------------------------------------------------
# Copiar las DLLs de FFmpeg al lado del .exe, asi corre sin tocar el PATH.
# ---------------------------------------------------------------------------
file(GLOB FFMPEG_DLLS
    "${FFMPEG_BIN_DIR}/avcodec-*.dll"
    "${FFMPEG_BIN_DIR}/avformat-*.dll"
    "${FFMPEG_BIN_DIR}/avutil-*.dll"
    "${FFMPEG_BIN_DIR}/swscale-*.dll"
    "${FFMPEG_BIN_DIR}/swresample-*.dll"
)

if(FFMPEG_DLLS)
    add_custom_command(TARGET recorder POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FFMPEG_DLLS} "$<TARGET_FILE_DIR:recorder>"
        COMMENT "Copiando DLLs de FFmpeg al lado del ejecutable")
endif()
