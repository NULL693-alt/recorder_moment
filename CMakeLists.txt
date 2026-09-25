cmake_minimum_required(VERSION 3.16)
project(medal_clone_fase1 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Genera build/compile_commands.json, que usan VS Code (y otros editores)
# para que el IntelliSense sepa exactamente con que flags se compila cada
# archivo y no marque los #include como "no encontrados".
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(LIBPORTAL REQUIRED libportal)
pkg_check_modules(GLIB REQUIRED glib-2.0 gio-2.0)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale)

add_executable(hello_capture src/main.cpp)

target_include_directories(hello_capture PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${LIBPORTAL_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
)
target_link_libraries(hello_capture PRIVATE
    ${PIPEWIRE_LIBRARIES}
    ${LIBPORTAL_LIBRARIES}
    ${GLIB_LIBRARIES}
    ${FFMPEG_LIBRARIES}
)
target_compile_options(hello_capture PRIVATE
    ${PIPEWIRE_CFLAGS_OTHER}
    ${LIBPORTAL_CFLAGS_OTHER}
    ${GLIB_CFLAGS_OTHER}
    ${FFMPEG_CFLAGS_OTHER}
)
