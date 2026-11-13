# NVMetrics

Monitor de la gráfica para el driver [NullMoth](https://github.com/TheFormidable/nvidia-macos-driver) en macOS: un icono en la barra de menús con la temperatura y una ventana con
relojes, consumo, temperaturas, ventilador, VRAM y los dos interruptores que el driver de verdad permite. Sigue el modo
claro u oscuro de macOS.

## Qué lee

Del bloque de datos compartidos de NVIDIA (clase `0xde`, la misma vía que `tools/nvmtl-smi` del driver), sin permisos especiales:
temperatura del núcleo, la memoria y la placa; consumo instantáneo y medio y límite de potencia; reloj de la GPU, la
memoria y el vídeo; estado de energía (P0 a P8); uso de la GPU, la memoria y el codificador y decodificador de vídeo;
motivos por los que la tarjeta está frenando; RPM de los ventiladores; VRAM usada y total. Un dato que el driver no da
se muestra como `--`, nunca como un cero.

## Qué controla

- **Modo reposo**: escribe `NVRM_610_BOOST=0` en el bloque "my settings" de `/Library/GPUBundles/nvmtl/nvrm610.conf`
  (`agent/nvmetrics-setting.sh`), para que el escritorio deje de pedir relojes máximos y la tarjeta pueda bajar a reposo. Se
  aplica al cerrar sesión y volver a entrar. Al quitarlo se borra solo esa línea.
- **Modo eco**: pide a la tarjeta que no pase de su potencia nominal (`NV2080_CTRL_CMD_PERF_RATED_TDP_SET_CONTROL`,
  cliente GLOBAL, acción FORCE_LIMIT, el interruptor que NVIDIA documenta para herramientas de usuario). Al quitarlo se
  envía DEFAULT y decide el driver. Se aplica al momento.

Los dos piden la contraseña de administrador.

## Qué no puede hacer

Velocidad o curva de ventiladores, límite de potencia en %, límite de temperatura, subir o bajar relojes y voltaje. El
gestor de recursos abierto de NVIDIA (open-gpu-kernel-modules 620.70.06) no tiene esas órdenes: NVIDIA las guarda en su
parte cerrada, y los ventiladores los lleva el firmware de la tarjeta. La app no muestra controles que no funcionarían.

## Estructura

- `agent/`: `nvmetrics-agent` (C), el que habla con el driver, y `nvmetrics-setting.sh` (modo reposo), con sus pruebas.
- `app/`: la app de barra de menús (SwiftUI, macOS 14 o posterior) y `build_app.sh`.

## Compilar (en el Mac)

Solo hace falta Xcode, git y conexión, más una copia del repo del driver
([TheFormidable/nvidia-macos-driver](https://github.com/TheFormidable/nvidia-macos-driver)):

    app/build_app.sh <copia del repo del driver> <carpeta de salida>

No necesita el árbol de NVK ni Homebrew. `agent/extract_rm_layer.py` reconstruye la capa del driver que habla con la
tarjeta (`nvRmApi.c`, `nvrm_xnu.c` y sus cabeceras) a partir de los parches de NVK del propio driver, en el mismo
orden que `build/build_nvk.sh`. `agent/fetch_headers.sh` baja las cabeceras de NVIDIA igual que el driver (610.57.04
con sus parches) y, aparte, la disposición del bloque de métricas de 620.70.06, que es la que publica el kext: en 620
hay una sección nueva en medio del bloque, y con la de 610 el consumo y el ventilador se leerían de otro sitio. Lo
descargado queda en `build/` y se reutiliza.

## Pruebas (cualquier sistema, sin gráfica)

    ./run_tests.sh <copia del repo del driver>
