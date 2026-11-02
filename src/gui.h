// ============================================================================
// gui.h — interfaz grafica (Dear ImGui + D3D11 + bandeja del sistema)
//
// Es el modo por defecto del programa: abre una ventana con los ajustes, los
// botones de Iniciar/Detener/Guardar y el estado de la grabacion. Se puede
// cerrar a la bandeja del sistema para que siga grabando de fondo.
// ============================================================================
#pragma once

// Clase de la ventana principal y mensaje para traerla al frente. Los usa
// main.cpp cuando el programa ya estaba abierto (solo corre una instancia).
#define RECORDER_WINDOW_CLASS L"RecorderMomentWindow"
#define WM_RECORDER_SHOW (0x8000 + 2) // WM_APP + 2

// Abre la ventana y no vuelve hasta que el usuario sale. Devuelve el codigo de
// salida del proceso (0 = todo bien).
int run_gui();
