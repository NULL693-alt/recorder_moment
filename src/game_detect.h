// ============================================================================
// game_detect.h — de que juego (o programa) es el clip
//
// Mira la ventana que tiene el foco y saca un nombre lindo para la carpeta:
// el nombre del producto del .exe ("VALORANT", "Minecraft"...), o si no tiene,
// el titulo de la ventana, o si no el nombre del .exe. El escritorio y el
// Explorador quedan como "Escritorio".
// ============================================================================
#pragma once

#include <string>

// Nombre para la carpeta de la ventana que tiene el foco ahora. Devuelve ""
// si el foco lo tiene este mismo programa (o no hay ventana).
std::string foreground_app_name();

// Deja un nombre apto para carpeta de Windows: saca \ / : * ? " < > | y los
// simbolos (R) (TM), espacios y puntos de los bordes, y lo acorta.
std::string sanitize_folder_name(const std::string &name);

// Nombre de la carpeta de los recortes (dentro de la carpeta de clips).
inline const char *kEditedFolder = "Clips Edit";
