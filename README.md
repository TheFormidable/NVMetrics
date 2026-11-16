# NVMetrics

Monitor de la gráfica para el driver [NullMoth](https://github.com/TheFormidable/nvidia-macos-driver) en macOS: un icono en la barra de menús con la temperatura y una ventana con
relojes, consumo, temperaturas, ventilador, VRAM, los controles que el driver de verdad permite, avisos cuando la tarjeta se
calienta y, con el driver 3.0, su salud. Sigue el modo claro u oscuro de macOS.

## Qué lee

Del bloque de datos compartidos de NVIDIA (clase `0xde`, la misma vía que `tools/nvmtl-smi` del driver), sin permisos especiales:
temperatura del núcleo, la memoria y la placa; consumo instantáneo y medio y límite de potencia; reloj de la GPU, la
memoria y el vídeo; estado de energía (P0 a P8); uso de la GPU, la memoria y el codificador y decodificador de vídeo;
motivos por los que la tarjeta está frenando; RPM de los ventiladores; VRAM usada y total. Un dato que el driver no da
se muestra como `--`, nunca como un cero.

## Qué controla

- **Modo reposo**: escribe `NVRM_610_BOOST=0` en el bloque "my settings" de `/Library/GPUBundles/nvmtl/nvrm610.conf`
  (`agent/nvmetrics-setting.sh`), para que el escritorio deje de pedir relojes máximos y la tarjeta pueda bajar a reposo. Al
  quitarlo se borra solo esa línea. Con el driver 3.0.1 o posterior, en la misma petición de contraseña cambia también la
  política de boost del driver (`nvmetrics-agent policy idle|normal`): las apps que ya están abiertas dejan de pedir
  relojes máximos (o vuelven a pedirlos) en menos de un segundo, sin cerrar sesión. El archivo sigue mandando después de
  reiniciar. Con un driver anterior solo se escribe el archivo y se aplica al cerrar sesión y volver a entrar. El modo
  reposo no quita el boost que pide el propio NVMetrics (Automático o Máximo), que es una petición aparte.
  El cambio al momento lo hace el agente de esta app con su orden `policy`. Un NVMetrics cuyo agente no la tiene solo
  escribe el archivo: si en este arranque ya se eligió una política, su interruptor de reposo no cambia nada hasta
  reiniciar, tampoco al cerrar sesión, porque el driver sigue la política y no el archivo. Esta app lo avisa al abrir
  su ventana cuando el archivo y la política no coinciden. Reiniciar lo arregla. Sin reiniciar,
  `sudo NVMetrics.app/Contents/Resources/nvmetrics-agent policy undecided`, con el agente de esta app, hace que cada
  app vuelva a lo que leyó del archivo al arrancar: las que ya estaban abiertas siguen con el valor de entonces hasta
  cerrar sesión y volver a entrar.
- **Modo eco**: pide a la tarjeta que no pase de su potencia nominal (`NV2080_CTRL_CMD_PERF_RATED_TDP_SET_CONTROL`,
  cliente GLOBAL, acción FORCE_LIMIT, el interruptor que NVIDIA documenta para herramientas de usuario). Al quitarlo se
  envía DEFAULT y decide el driver. Se aplica al momento.

Estos dos piden la contraseña de administrador.

- **Rendimiento de la GPU** (Ahorro, Automático o Máximo): cuándo NVMetrics pide a la tarjeta sus relojes máximos
  (`NV2080_CTRL_CMD_PERF_BOOST`, "boost to max", la misma orden que `tools/nvmtl-boost` del driver). *Ahorro* nunca
  los pide. *Automático* (el modo de partida) los pide cuando la GPU lleva unos 2 s trabajando de verdad (35 % de uso
  o más, un juego por ejemplo) y deja de pedirlos cuando lleva 15 s tranquila (menos del 15 %). *Máximo* los pide todo
  el rato. Se aplica al momento y **no pide contraseña**: el driver deja hacer esa petición a cualquier usuario. Dura
  solo mientras NVMetrics está en marcha (cerrar la ventana no basta: sigue en la barra de menús); al salir con Salir del
  menú (también si se cierra de golpe o lo fuerzas a salir) el driver la retira solo. Solo cambia algo con el
  modo reposo activado; sin él, el propio driver ya tiene la tarjeta a tope casi siempre. La tarjeta sigue respetando
  sus límites de temperatura y potencia. Mientras la protección por calor está activa, NVMetrics no pide relojes
  máximos en ningún modo (ver *Protección*).

## Avisos

Si hay algo que mirar, la ventana lo muestra arriba, bajo el título, y el menú de la barra lo pone como primera línea (el más
grave); para un peligro o un aviso, la barra de menús muestra además `⚠︎` delante de la temperatura:

- **Peligro**: el núcleo a 88 °C o más (cierra el juego o la carga y revisa la refrigeración).
- **Peligro**: los ventiladores parados con el núcleo a 70 °C o más (comprueba que nada los bloquee).
- **Aviso**: el núcleo a 80 °C o más, o la memoria de vídeo a 95 °C o más.
- **Información**: la protección está activa (salvo en *Ahorro*, donde no cambia nada; ver abajo).

Los umbrales se comparan con la temperatura en grados enteros, la que muestra la app. Un aviso de temperatura, una vez
puesto, sigue hasta que la lectura baja más de 2 °C por debajo de su umbral (el de 80 °C se quita por debajo de 78 °C),
para que no parpadee si la lectura ronda el límite.

Son solo avisos: no cambian nada en la tarjeta. Sin avisos, la ventana no muestra ese bloque.

## Protección

Mientras NVMetrics está en marcha, el agente vigila cada lectura. Si durante 3 s seguidos el núcleo está a 83 °C o más,
la memoria de vídeo a 95 °C o más, o los ventiladores parados con el núcleo a 70 °C o más, activa la protección:
**NVMetrics deja de pedir relojes máximos** (el *Rendimiento de la GPU* elegido se queda como está y vuelve a aplicarse
solo cuando termina). La quita cuando lleva 30 s seguidos con el núcleo por debajo de 75 °C, la memoria por debajo de
88 °C (o sin lectura) y, si los ventiladores están parados, el núcleo por debajo de 65 °C. Si falta la lectura de la
temperatura del núcleo, no cambia de estado. Mientras está activa, salvo en *Ahorro*, el panel de rendimiento dice
"Ahora: en pausa por protección".

Qué no hace: no mueve los ventiladores (no se puede, ver *Ventiladores*), no activa el modo eco (pide la contraseña y lo
decides tú; es lo otro que baja el calor con carga), no baja el límite de potencia ni apaga nada. La tarjeta sigue
aplicando por su cuenta sus propios límites de temperatura.

Cuándo sirve: solo con el **modo reposo activado**. Sin él, el propio driver sigue pidiendo relojes máximos y que
NVMetrics deje de pedirlos no baja nada. En *Ahorro* tampoco cambia nada, porque NVMetrics ya no los pide. Y solo
funciona mientras NVMetrics está abierto (basta con que siga en la barra de menús); al salir de la app no queda nada
vigilando.

## Ventiladores

Los ventiladores los controla el firmware de la tarjeta con la curva de su BIOS. Muchas tarjetas los paran del todo por
debajo de unos 50-60 °C, así que verlos a 0 RPM con la tarjeta tranquila es normal. NVMetrics muestra sus RPM pero no
puede cambiarlos: NVIDIA no publica las órdenes para hacerlo. En su gestor de recursos abierto (open-gpu-kernel-modules)
la cabecera de esas órdenes, `ctrl2080fan.h`, está vacía, y el bloque de métricas solo trae la velocidad actual, para
leer. En Linux, la velocidad se cambia con el software cerrado de NVIDIA (su biblioteca NVML, por ejemplo), no con órdenes
publicadas.

Para que esto cambie, NVIDIA tendría que publicar en open-gpu-kernel-modules las definiciones de esas órdenes: leer y
fijar la velocidad, la política de control y la vuelta a la curva automática (las que hay detrás de las funciones de
ventilador de NVML). Se le puede pedir en ese repositorio, en Discussions, categoría Ideas (no acepta peticiones como
issues). Si las publica, NVMetrics podría ofrecer control de ventiladores; hasta entonces no lo ofrece.

## Salud del driver

Con el driver 3.0, la ventana muestra los contadores que el propio driver lleva desde el último arranque del Mac
(sysctls `debug.nvrm_*`, se leen sin permisos): errores de la GPU (Xid) y el último, apps que el driver rescató tras un
fallo, apps que perdieron la GPU, fallos del firmware de la tarjeta, avisos internos del driver y ajustes de arranque
del driver fuera de lo normal (con la lista, donde `!` marca los que una instalación normal no necesita). Cero es lo
normal; un número mayor sale en ámbar. Con el driver 2.9 esos contadores no existen y el panel dice "Disponible con el
driver 3.0.".

## Qué no puede hacer

Velocidad o curva de ventiladores (ver *Ventiladores*), límite de potencia en %, límite de temperatura, mover los relojes
por encima o por debajo de los de fábrica (offsets) y voltaje. El gestor de recursos abierto de NVIDIA
(open-gpu-kernel-modules 620.70.06) no tiene esas órdenes: NVIDIA las guarda en su parte cerrada, y los ventiladores los
lleva el firmware de la tarjeta. La app no muestra controles que no funcionarían.

## El agente desde Terminal

    nvmetrics-agent watch [ms] [--control]   una línea JSON por lectura (por defecto cada 1000 ms)
    nvmetrics-agent once                     una sola línea
    nvmetrics-agent boost hold               pide relojes máximos hasta Ctrl-C (para probar; sin contraseña)
    nvmetrics-agent policy [status]          la política de boost del driver: undecided, idle o normal; `null`, con
                                             el motivo en `why`, si el driver cargado no la tiene (es del 3.0.1)
    sudo nvmetrics-agent policy idle|normal|undecided   la cambia (solo root); undecided es como queda al arrancar

Cada línea lleva `"health"` (`null` antes del driver 3.0). Con `--control`, que es como lo lanza la app, el agente lee
órdenes de su entrada estándar, una por línea: `boost off`, `boost auto` o `boost max`, y cada línea lleva también
`"boost":{"mode":...,"held":true|false,"error":null|"0x..."}` y
`"protect":{"active":true|false,"why":"hot"|"mem_hot"|"fans_stopped"|null}` (la protección: núcleo caliente, memoria
caliente o ventiladores parados; `null` mientras no está activa). Al cerrarse su entrada estándar (la app se fue) o al
recibir SIGTERM retira la petición y termina.

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
