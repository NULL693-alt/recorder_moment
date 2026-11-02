# Empaqueta el grabador en una carpeta portable (y un .zip) para compartir.
#
# Uso (parado en la carpeta del proyecto):
#   $env:Path = "$PWD\tools\mingw64\bin;" + $env:Path
#   cmake --build build
#   .\package.ps1
#
# Deja el resultado en dist\recorder_moment\ y dist\recorder_moment.zip.
# Esa carpeta se copia a cualquier Windows 10/11 de 64 bits y anda sin
# instalar nada (el .exe ya lleva el runtime estatico y las DLLs de FFmpeg
# van al lado).

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$build = Join-Path $root "build"
$exe = Join-Path $build "recorder.exe"

if (-not (Test-Path $exe)) {
    throw "No existe $exe. Compila primero con 'cmake --build build'."
}

$distRoot = Join-Path $root "dist"
$pkg = Join-Path $distRoot "recorder_moment"
$zip = Join-Path $distRoot "recorder_moment.zip"

# La copia de dist\ es la que se usa (y la que abre Windows al iniciar): sus
# ajustes se conservan al reempaquetar, pero no van dentro del .zip.
$cfgFile = Join-Path $pkg "recorder_config.ini"
$savedCfg = $null
if (Test-Path $cfgFile) { $savedCfg = [IO.File]::ReadAllBytes($cfgFile) }

# Limpia el empaquetado anterior.
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force -Path $pkg | Out-Null

# El ejecutable.
Copy-Item $exe $pkg

# Las DLLs de FFmpeg que el .exe necesita (estan al lado del build).
Copy-Item (Join-Path $build "*.dll") $pkg

# Una guia corta para quien lo recibe.
@"
recorder_moment
===============

Grabador de clips estilo Medal para Windows (video + audio).

COMO USARLO
  1. Descomprime esta carpeta en cualquier lado.
  2. Haz doble clic en recorder.exe.
  3. Se abre una ventana. Ajusta la carpeta donde guardar, el monitor,
     los fps, el bitrate y los segundos del buffer.
  4. Apreta "Iniciar". Mientras graba, apreta "Guardar clip" (o F9) para
     guardar los ultimos N segundos en un .mp4.

  La ventana se puede cerrar (queda en la bandeja del sistema, junto al
  reloj). Click derecho en su icono para volver a abrirla o salir.

  En "Arranque" podes marcar "Iniciar con Windows" para que se abra solo
  al encender la PC.

REQUISITOS
  - Windows 10 u 11 (64 bits).
  - Si no hay microfono igual graba; solo se pierde tu voz.

Los clips se guardan en la carpeta que elijas (por defecto, la carpeta
del programa), con nombres como clip_20261101_153000.mp4.

No hace falta instalar nada: todo lo necesario esta en esta carpeta.
"@ | Set-Content -Encoding UTF8 (Join-Path $pkg "LEEME.txt")

# El zip (sin ajustes personales: quien lo reciba arranca con los de fabrica).
Compress-Archive -Path (Join-Path $pkg "*") -DestinationPath $zip

# Devolvemos los ajustes a la carpeta portable.
if ($savedCfg) { [IO.File]::WriteAllBytes($cfgFile, $savedCfg) }

Write-Host ""
Write-Host "Listo:"
Write-Host "  carpeta portable: $pkg"
Write-Host "  zip para compartir: $zip"
