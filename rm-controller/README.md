# NVMetrics 0.14.7 — candidato de estabilización para Intel

Continuación de `NVMetrics-RM-v0145-complete-source.zip`. **No es todavía una versión certificada como estable en control físico.** Se conserva la base integrada bajo `sources/` y se aplican las correcciones de esta revisión durante la compilación en una copia temporal. Esta entrega se incorpora bajo `rm-controller/` para preservar la implementación previa de la raíz del repositorio. El instalador está en [`packages/0.14.7/`](../packages/0.14.7/).

## Resultado

- Instalador autónomo `NVMetrics-0.14.7-Candidate-Intel.pkg`, compilado para macOS x86_64, mínimo declarado macOS 14.
- Aplicación SwiftUI, agente C y servicio/rescate/cliente Python congelados con PyInstaller 6.22.3. El usuario final no necesita Python, Homebrew, Xcode ni descargar paquetes.
- Se requiere el driver NullMoth compatible, ya instalado. Este paquete no instala ni modifica el driver.
- Se mantiene la restricción de la base a una sola RTX 5050 y al diseño RM observado de dos ventiladores, con candidatos de límites RPM 1150/3700. No se amplía a otros modelos.
- Aplicación con firma ad hoc local. El `.pkg` no tiene firma Developer ID ni notarización; no se presenta como una distribución pública aprobada.

## Reducción de la demora al bajar (0.14.7)

La 0.14.6 abría y calentaba un lector RUSD durante 1,2 s por cada sondeo. El descenso real registrado tardó unos 8 s por escalón. La nueva revisión utiliza un lector de sólo lectura con una conexión que dura únicamente durante Manual. Cada respuesta debe corresponder a la petición actual y demostrar avance de GPU, VRAM, potencia y RPM desde la respuesta anterior. Un timeout, EOF, fuente congelada o respuesta incorrecta invalida el lector y activa la ruta de fallo existente.

Se mantienen los pasos de 5 %, las tres muestras de RPM, sus pausas de 450 ms, la pausa entre escalones de 700 ms, los límites y el rescate independiente. El lector no recibe comandos de boost, Eco ni SET. En AUTO se cierra para no mantener una suscripción de sensores innecesaria.

Cuando se rechaza una bajada, se conserva el destino seleccionado y se indica el dato que la bloqueó (por ejemplo, 50,10 W frente al límite de 50 W). El límite no se amplía. Esta revisión añade migración desde 0.14.6; no modifica una sesión ni instalación activa.

## Correcciones heredadas

1. **AUTO conserva prioridad.** Puede solicitarse mientras START todavía no devuelve el token y durante una reducción. Tras una respuesta AUTO fallida, la GUI deja de renovar Manual y espera al guard/rescate. No vuelve a mantener Manual contra la intención del usuario.
2. **Interfaz sincronizada.** Seleccionar no escribe hardware. Aplicado y seleccionado siguen separados. Se descartan respuestas de un observador cancelado. Eco y Reposo descartan lecturas anteriores a un cambio, verifican el estado devuelto y continúan siendo funciones independientes.
3. **Procesos sin bloqueo por stderr.** La GUI drena stdout y stderr simultáneamente. Una salida de error grande ya no deja bloqueado el cliente IPC esperando stdout.
4. **Frescura térmica independiente.** El guard exige que GPU, VRAM, potencia y tacómetros avancen antes de autorizar START. Vigila cada fuente con el límite existente de 3 s; la actualización GPU no oculta una VRAM, potencia o RPM congelada.
5. **Verificación RM.** Comprueba que la política por defecto sigue en AUTO antes y después de un cambio manual y durante la vigilancia. El restaurador rechaza un bloque por defecto cuyo contenido no sea AUTO. SET aceptado no se considera recuperación confirmada.
6. **RPM y fallos.** La vigilancia Python también cubre el perfil de inactividad a 50–100 %. Rechaza canales adicionales y exige exactamente los dos tacómetros esperados. Un fallo de sondeo previo a bajar solicita AUTO. Se conserva el motivo de un disparo nativo aunque la transacción ya haya consumido su primer evento.
7. **Recuperación visible.** Los fallos de AUTO y otros cortes mantienen el bloqueo de seguridad y muestran el resultado de la verificación independiente del rescate. No se confunde “rearme bloqueado” con “AUTO confirmado”.
8. **Empaquetado.** Versión y copias de respaldo propias de 0.14.6; actualización verificada desde 0.14.5. El paquete elimina los antiguos lanzadores experimentales dependientes de Python externo. No sobrescribe un `.pkg` de salida existente.

## Política conservada

Manual comienza al 50 % y desciende por pasos verificados de 5 % hasta un mínimo de 30 %. Los aumentos admiten un destino verificado hasta 100 %. Cada reducción verifica RM y requiere tres muestras consecutivas válidas de ambos tacómetros. Ante carga transitoria puede conservar un paso seguro o recuperar al menos 50 %; otros fallos solicitan AUTO.

No se elevaron los límites: entrada térmica GPU <60 °C, VRAM <84 °C, ≤60 W y utilización ≤20 %; reducción por debajo de 50 % con GPU <58 °C, VRAM <82 °C, ≤50 W y utilización ≤20 %. En Manual, AUTO ante GPU ≥65 °C, VRAM ≥88 °C, >145 W, datos críticos inválidos o pérdida de supervisión. Se mantienen el guard a 250 ms, heartbeat nativo de 6 s, lease del cliente de 8 s y rescate separado. Son políticas experimentales de la aplicación, no límites certificados por el firmware.

Eco modifica el límite de potencia mediante su ruta existente. Reposo guarda la configuración de boost del usuario mediante su ruta existente. Ninguno activa Manual ni cambia la política térmica del controlador de ventiladores. Reposo puede requerir cerrar sesión para afectar procesos ya abiertos.

## Construcción y pruebas

En una máquina de desarrollo Intel con Xcode y Python 3.11–3.15:

```sh
python3 -m venv .build-venv
.build-venv/bin/python -m pip install -r requirements-build.txt
NVMETRICS_BUILD_PYTHON="$PWD/.build-venv/bin/python" ./run_tests.sh
NVMETRICS_BUILD_PYTHON="$PWD/.build-venv/bin/python" \
  PYINSTALLER_CONFIG_DIR="$PWD/.build-cache/pyinstaller" \
  ./build_package.sh sources/NVMetrics sources/nvidia-macos-driver /ruta/nueva/de/salida
```

La descarga inicial de encabezados NVIDIA requiere red. Se reutilizan los tags de la base: 610.57.04 con los parches NullMoth incluidos y el diseño RUSD de 620.70.06. `NVMETRICS_BUILD_CACHE` permite indicar otra caché de compilación. El código base fijado conserva sus hashes originales.

`run_tests.sh` ejecuta 101 pruebas Python, 10 suites C, 2 suites Swift y la suite shell de configuración. No escribe controles GPU. Incluye los tests originales de Eco, boost, protección, salud, telemetría y configuración, además de las regresiones nuevas. La compilación ejecuta todas las pruebas, valida el runtime congelado, verifica la firma de la app y comprueba la extracción exacta del paquete final.

```sh
NVMETRICS_BUILD_PYTHON="$PWD/.build-venv/bin/python" \
  ./verify_package.sh /ruta/NVMetrics-0.14.7-Candidate-Intel.pkg
```

## Validación y límites

Consulta `AUDITORIA-v0147.md` para los resultados reproducibles y los puntos pendientes. Se han leído telemetría y registros RM de la RTX 5050 real, sin SET de ventiladores, boost ni Eco. El diagnóstico `diagnostics/read_only_sensors.c` reutiliza el código C real para contrastar la actualización de las cuatro fuentes; no forma parte del instalador ni se ejecuta en el test automático.

No se ha instalado esta revisión en `/Applications`, reemplazado el helper activo ni probado físicamente su nueva secuencia 30–100 %. Antes de llamarla estable queda ejecutar una instalación/actualización real, un ensayo supervisado en reposo y los casos de salida de aplicación/recuperación sobre esta versión. No se debe asumir recuperación garantizada ante bloqueo de kernel, driver o firmware.

El servicio mantiene la autorización por UID del diseño original. Otros procesos de esa misma cuenta pueden enviar solicitudes; no se ha implementado autenticación de una GUI firmada por XPC. La firma/notarización pública, una desinstalación soportada y la cualificación de otros equipos siguen fuera de esta revisión de estabilización.
