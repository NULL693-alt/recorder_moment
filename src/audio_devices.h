// ============================================================================
// audio_devices.h — lista de dispositivos de audio (WASAPI)
//
// Sirve para que la interfaz grafica pueda ofrecer una lista de microfonos y
// de dispositivos de salida (de donde se saca el sonido del sistema) en vez de
// usar siempre los predeterminados.
// ============================================================================
#pragma once

#include <string>
#include <vector>

struct AudioDeviceInfo {
    std::string id;   // id de WASAPI; vacio significa "el predeterminado"
    std::string name; // nombre para mostrar (UTF-8)
};

// Dispositivos de salida (para el sonido del sistema). El primero es siempre
// "(Predeterminado)".
std::vector<AudioDeviceInfo> list_output_devices();

// Dispositivos de entrada (microfonos). El primero es siempre
// "(Predeterminado)".
std::vector<AudioDeviceInfo> list_input_devices();
