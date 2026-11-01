// ============================================================================
// gui.h — interfaz grafica (Dear ImGui + D3D11 + bandeja del sistema)
//
// Es el modo por defecto del programa: abre una ventana con los ajustes, los
// botones de Iniciar/Detener/Guardar y el estado de la grabacion. Se puede
// cerrar a la bandeja del sistema para que siga grabando de fondo.
// ============================================================================
#pragma once

// Abre la ventana y no vuelve hasta que el usuario sale. Devuelve el codigo de
// salida del proceso (0 = todo bien).
int run_gui();
