# RUSD Flight Recorder y reintento RUSD: informe técnico (rama `fix/rusd-flight-recorder`, 0.15.8)

Incidente investigado: `guard_terminated_gpu_temperature_timestamp_invalid_rescue_verified_restart_service`
durante Manual al 30 %, con `gpu_ts_ok=false`, `gpu_temp_age_ms=UINT64_MAX` y el resto de fuentes válidas.

**Estado:** el Flight Recorder está implementado y probado sin hardware, y hay una corrección mínima de lectura con
reintento acotado. **El problema NO está declarado resuelto**: falta una sesión física real con este build.

## 00. Versión 0.15.8: el fallo reproducido en hardware y la corrección del reintento

**Dato real (RTX 5050 Blackwell, macOS, 0.15.7):** `first_ok=0`, `first_cause=write_in_progress`, `retries=0`,
`retry_us=13388`.

**Qué significa.** La primera lectura de la sección GPU encontró al productor publicando (`WRITE_IN_PROGRESS`).
El reintento de 0.15.7 dormía 250 µs con `nanosleep()` antes de cada intento. Ese `nanosleep` volvió tras
**13 388 µs**: el guard se lanza con QoS de fondo (`spawn type = background (5)` en `launchd`), y macOS puede
agrupar y retrasar sus temporizadores. Al despertar, el presupuesto de 2 ms ya estaba agotado y el código salió
**sin hacer ningún reintento**. La copia se quedó en `UINT64_MAX` y el guard volvió a AUTO. Es exactamente el caso
R6 de las pruebas de 0.15.7.

Esto confirma tu valoración: es un problema de **adquisición de telemetría**. El control RM no interviene en él.

| # | Petición | Implementación | Prueba |
|---|---|---|---|
| 1 | Sin `nanosleep()` en el camino crítico | `rusd_guard_reader.h` no duerme: `pause` de CPU + `clock_gettime(CLOCK_MONOTONIC)`. `run_tests.sh` falla si el binario de prueba del lector enlaza `nanosleep`, `usleep` o `sleep`. | `nm -u`; prueba con reloj real: WIP permanente termina a los 2000–2002 µs. |
| 2 | Primer reintento inmediato, después espera activa acotada | 1.º reintento sin espera; los siguientes cada 40 µs de espera activa. Tras una interrupción del planificador (dos lecturas del reloj separadas ≥500 µs) se hace **exactamente un** intento inmediato más, aunque el presupuesto haya vencido: no espera nada y una interrupción no es observación. | R3a/R3b, R6, R12, prueba con reloj real (publicación a 200 µs recuperada). |
| 3 | 2 ms por sección; nunca datos antiguos ni copias inconsistentes | Ningún intento empieza pasados 2 ms de observación. Tope de 64 llamadas y de 200 000 iteraciones de espera (si el reloj se detuviera: `clock_anomaly`). Solo se acepta la copia si `RusdReadData()` vio el mismo timestamp antes y después; después pasa por `nvm_ok()` y por el reloj de frescura de 3 s. | R5 (WIP: 2000–2005 µs, inválido), R13 (timestamp siempre cambiando: ninguna copia aceptada), R14 (reloj detenido), R3/R12 (dato nuevo, no el anterior). |
| 4 | Distinguir presupuesto agotado / interrupción del planificador / escritura persistente | Final del camino de reintento: `recovered`, `budget_exhausted`, `scheduler_preempted`, `producer_write_persistent` (WIP en cada observación durante todo el presupuesto y sin interrupciones), `section_withdrawn`, `clock_anomaly`. Causas nuevas en recorder, `safety_metrics.rusd_cause` y analizador, cada una con su grado de certeza. | R5/R6/R13, escenarios `gpu_preempted`, `gpu_producer`, `gpu_budget`. |
| 5 | `retry_max_us` con cero reintentos | Cuenta todo camino de reintento iniciado (`retry_paths`), aunque tenga 0 reintentos; `retry_us` pasa a 32 bits (0.15.7 lo guardaba en 16). Resumen nuevo: `retry_paths`, `preempted_paths`, `max_gap_us`. | `incident_record_tests`: `retry_max_us=13388`, `retry_paths=1`, `seqlock_retried=0`. |
| 6 | Atribución del TRIP con una ventana anterior abierta | El TRIP se atribuye a la muestra que disparó; la ventana abierta se cierra con su propio disparador. Un trip sin fuente crítica fallida (térmico, heartbeat, RM) es `none`. | Recorder: ventana del ventilador abierta → TRIP de la GPU. Escenario `trip_attribution`. |
| 7 | Falso negativo del analizador con el log vacío | Log vacío, sin sesiones o sin muestras → **"No concluyente"** y código de salida 1, nunca "sin anomalías". | `test_empty_log_is_inconclusive_not_clean` (3 variantes). |
| 8 | Regresión con los datos reales | Tres niveles: lector real (R12: WIP, interrupción de 13 388 µs, el productor termina durante ella → recuperado con copia nueva), registro exacto en el recorder y guard completo (`incident_0157` dispara como en hardware; `incident_0158` continúa en Manual). El analizador marca el registro 0.15.7 como "camino de reintento sin ningún reintento", no atribuible al productor. | R12, `incident_record_tests`, escenarios `incident_0157`/`incident_0158`, `test_incident_0157_record_is_flagged…` |
| 9 | Versión 0.15.8 | `VERSION`, `.v0158-package-backup`, marcador `NVMETRICS_RM_V0158_CONTINUOUS`, `V0157_MARKER` como versión previa, ping `revision 0.15.8`, `Info.plist`, `pkgbuild`, nombre del `.pkg`, `Build/Verify.command`, `verify_package.sh`, README. Banner `NVMETRICS_RM_V0158_RUSD_SPIN_RETRY` verificado por `build_package.sh`. | `test_upgrade_from_0157_is_not_same…`, `test_0157_receipt_with_wrong_agent_is_refused`, `test_one_revision_everywhere`. |
| 10 | Protecciones intactas | `fan_guard_core_v04.h` (límites térmicos), `rescue.py`, `physical.py`, `fan_rm_wire.h` y `fan_rm_experiment.h` **sin cambios** respecto a 0.15.7. En `fan_rm_guard_v04.h` solo cambia una línea de banner. AUTO sigue precediendo a toda E/S diagnóstica. | `git diff 1af8ee7`; los 23 escenarios mantienen los invariantes de 0.15.7. |

**Comprobación de las pruebas.** Inyecté cuatro regresiones en copias temporales y las cuatro hacen fallar las
pruebas:
- sin intento tras una interrupción;
- volver a esperar antes del primer reintento (comportamiento 0.15.7: 0 reintentos);
- contabilidad antigua de `retry_max_us`;
- atribución antigua del TRIP.

**Coste.** En el caso normal, nada. Solo cuando una lectura choca con una publicación, el guard hace espera activa
como máximo ~2 ms por sección (una CPU, 2 ms, de forma esporádica). Antes, la espera podía durar 13 ms o más y
además no servía.

**Lo que esto no demuestra.** Que el fallo desaparezca en hardware. Si tras 0.15.8 aparecen:
- **`seqlock_retry_recovered > 0`:** la corrección está absorbiendo colisiones.
- **`producer_write_persistent`:** el productor mantuvo la escritura ≥2 ms; eso apuntaría al driver/GSP.
- **`scheduler_preempted` repetidos:** el siguiente paso sería subir la QoS del guard. No lo he hecho, porque
  cambia el arranque del servicio.

## 0. Revisión 0.15.7: las cuatro correcciones de la revisión externa

| # | Hallazgo | Corrección | Prueba que lo detecta |
|---|---|---|---|
| 1 | `nvm_fr_event("termination")` y otras escrituras ocurrían antes de `nvm_v04_auto()`, y había E/S después de un AUTO fallido. | AUTO es lo primero en la salida. Todo diagnóstico de esa ruta (`termination`, `safety_metrics`, `DIAG`, registros de fallo RM, recorder) se formatea en buffers fijos en memoria (`struct nvm_out`, sin syscalls ni malloc) y se escribe después del intento de AUTO, **en el mismo orden de siempre**. Con AUTO **no verificado**: solo las líneas de protocolo en stdout, en modo no bloqueante si es una tubería; ni stderr ni recorder; salida inmediata al rescate. | Escenarios: la salida no crece entre el último dato y AUTO; AUTO ≤5 ms tras el dato decisivo. `auto_unverified`: 0 escrituras del recorder y sink vacío. Flush sobre una tubería llena: no bloquea. |
| 2 | `nvm_fr_record()` llamaba a `nvm_fr_dump()` al cerrar una ventana: 11 `write()` con Manual activo. | `nvm_fr_record()` no hace E/S. Cada ventana (24 muestras previas, la anomalía y 8 posteriores) se copia a uno de 6 huecos en memoria: **las 3 primeras de la sesión se conservan siempre y las demás rotan**, de modo que también quedan las más recientes. Los contadores por causa cubren el 100 % de las muestras. `nvm_fr_finish()` es el único escritor y solo se llama tras AUTO verificado o si nunca hubo escritura RM. Las notas `RUSD_CONFIRM` y `telemetry_gap` de stderr durante Manual también se difieren. | `write()` del recorder interceptada en todos los escenarios: 0 con el firmware en Manual y 0 antes de AUTO; stderr no crece durante Manual; stdout durante Manual solo contiene `ready`, `manual` y `lower_refused`. `long_transients`: 16 ventanas, se conservan 0–2 y 13–15 y se cuentan 10 sobrescritas. |
| 3 | El paquete seguía en 0.15.6: con 0.15.6 instalada, `installed_state()` devolvía `same` y salía sin instalar nada. | 0.15.7 en todos los puntos (patrón del commit `1dbfd58`): `VERSION`, `BACK=.v0157-package-backup`, marcador `NVMETRICS_RM_V0157_CONTINUOUS` en agente/instalador/`build_package.sh`, `V0156_MARKER` aceptado como versión previa, `server.py` (`revision`), `Info.plist`, `pkgbuild --version`, nombre del `.pkg`, `Build/Verify.command`, `verify_package.sh`, README. | `test_upgrade_from_0156_is_not_same_and_preserves_its_backups` (falla con `VERSION='0.15.6'`: `'same' != 'upgrade'`), `test_0156_receipt_with_wrong_agent_is_refused`, `test_one_revision_everywhere`. |
| 4 | El analizador daba falsos negativos y veredictos demasiado tajantes. | Las cifras salen de `session_summary.causes` (todas las muestras). Las muestras volcadas solo ilustran: sin resumen se marca la sesión como incompleta y no se concluye ausencia. Cada causa lleva origen y certeza: solo `ts_zero_producer_invalid` es "alta". `seqlock_timeout` y las relecturas recuperadas se declaran "no atribuibles en exclusiva". | 6 pruebas en `FlightReport`: solo resumen, solo muestras, duplicados, limpio, vacío, y certeza por causa. |

**Comprobación de las pruebas.** Inyecté en copias temporales tres regresiones: volcado antes de AUTO, `write()`
dentro de una ventana durante Manual y `nvm_fr_finish()` tras un AUTO fallido. Las tres hacen fallar
`test_guard_rusd_scenarios`.

**Sin cambios:** límites térmicos, RM SET/GET, heartbeat, watchdog, rescate independiente, `reason` de los
eventos y orden de los mensajes en el cable.

## 1. Causa raíz: qué está demostrado y qué no

### Demostrado en el código (lado consumidor, NVMetrics)

1. `rusd_snapshot()` (`agent/nvmetrics_agent.c`) llama a `RUSD_READ_DATA()` y **descarta su resultado booleano**.
2. `RusdReadData()` (NVIDIA `cl00de.h`, 620.70.06) hace como mucho **10 intentos seguidos, sin espera**, lo que
   dura nanosegundos. Si en esa ventana el productor está publicando (el timestamp vale `WRITE_IN_PROGRESS`) o el
   timestamp cambia entre la primera y la segunda lectura, devuelve `false` y **escribe `UINT64_MAX` en el
   timestamp de la copia**. Esa sustitución está en el propio código de NVIDIA, que la marca así:
   *"TODO: Remove this hack once all sites depend on the bool result of this function"*.
3. `nvm_ok(UINT64_MAX)` da 0, así que `gpu_ts_ok=false`. `nvm_guard_sensor_age()` devuelve entonces `UINT64_MAX`,
   que es exactamente el `gpu_temp_age_ms` registrado.
4. El guard trata esa única muestra como pérdida del sensor y restaura AUTO. `RUSD_CONFIRM` (v0.13.2) solo cubre
   el ventilador.
5. El propio `cl00de.h` documenta el contrato para las secciones polled: *"If timestamp is
   RUSD_TIMESTAMP_WRITE_IN_PROGRESS, data was edited during the read, retry."* NVMetrics no reintentaba.

**Precedente en el repositorio.** El caso v0.13.3 del ventilador (`tests/test_guard_header_compile.c`: *"isolated
UINT64_MAX at 45%, 46.54 C"*) registró `fan_ts_raw = UINT64_MAX`, que es exactamente la salida de un
`RusdReadData()` fallido, no un `0` publicado por el productor. Es la misma firma en otra sección del mismo
bloque y en el mismo hardware. No he podido revisar ese registro original: el dato procede del comentario del test.

### No demostrado (falta evidencia)

- **Que el incidente de la GPU tuviera esta causa.** El diagnóstico anterior guardaba `fan_ts_raw` pero no el
  timestamp bruto de la GPU. Con los datos del incidente, `gpu_ts_ok=false` es idéntico en estos tres casos:
  - **`ts = 0`:** el productor retiró la sección. El origen es el driver/GSP.
  - **`ts = UINT64_MAX` con el timestamp vivo válido justo después:** contención en la lectura (consumidor).
  - **`ts = UINT64_MAX` con el timestamp vivo todavía en `WRITE_IN_PROGRESS`:** el productor se quedó a medio
    publicar (driver/GSP).
- **El comportamiento del productor.** En Blackwell (GSP) las secciones polled las escribe el **firmware GSP**,
  que es código cerrado (`gpu_user_shared_data.c` solo envía la máscara por RPC). Ningún parche del kext
  (`kexts/NVRM/patches/*`) toca RUSD. No se puede saber leyendo código cuánto dura su ventana de publicación ni
  si alguna vez publica `0` de forma transitoria.

El Flight Recorder registra exactamente lo que falta: el timestamp bruto de la copia, el timestamp vivo
antes y después de cada lectura, el booleano de `RusdReadData()` y los reintentos.

## 2. Respuestas a la auditoría (§2 del encargo)

| Pregunta | Respuesta |
|---|---|
| ¿Datos parcialmente actualizados? | No, dentro de una sección. `RusdReadData()` compara el timestamp antes y después de la copia, con `__atomic_thread_fence(ACQUIRE)` (`metrics_json.h`) y orden TSO en x86. Se probó con un productor que publica a mitad de la copia (caso R9): la copia sale coherente. |
| ¿Copia del agente vs. del guard? | Son procesos distintos con mapeos propios del mismo bloque y tiempos distintos. Uno puede ver una sección inválida mientras el otro no. No comparten copia. |
| ¿El seqlock es por sección o global? | **Por sección.** Cada sección tiene su propio timestamp; un snapshot no es atómico entre secciones. Así está diseñado y el guard no lo necesita. |
| ¿Productor o consumidor? | El camino del consumidor está demostrado; el del productor no se puede excluir. El recorder lo distingue (`ts_zero_producer_invalid` / `seqlock_timeout` / `write_in_progress`). |
| ¿Tipos, tamaños, ABI? | No hay errores. `NvTemp` es 24.8 con signo; potencia en mW `NvU32`; timestamps `NvU64` sin truncar. El layout 620 está forzado por `metrics_json.h`. Un error de ABI sería permanente, no intermitente. El recorder registra `sizeof`/offsets reales: 3184, temperaturas 2312, ventilador 2584 (build de prueba). |
| ¿El driver puede publicar un timestamp inválido de forma transitoria? | **Sí, por diseño:** `WRITE_IN_PROGRESS` mientras escribe. Si también publica `0` de forma transitoria no se puede saber sin hardware. |

## 3. Flight Recorder

- **`agent/rusd_flight_recorder.h`** (sin tipos NVIDIA):
  - Anillo fijo de 128 muestras (32 s a 250 ms). Grabar una muestra es copiar un struct y clasificar con enteros:
    **35–63 ns por muestra, sin E/S** (medido en la prueba F9).
  - Causas diferenciadas: `ts_zero_producer_invalid`, `seqlock_timeout`, `write_in_progress`, `seq_marker_abi`,
    `read_inconsistent`, `ts_frozen`, `ts_warmup`, `ts_backwards`, `value_out_of_range`, `clock_anomaly`.
  - **Sin E/S durante la sesión (0.15.7).** Cada ventana de anomalía (24 muestras previas, la anomalía y 8
    posteriores) se copia a uno de 6 huecos en memoria: las 3 primeras ventanas se conservan y el resto rota.
    Los eventos del guard se guardan en un anillo de 48.
  - `nvm_fr_finish()` escribe todo de una vez al terminar, después de AUTO verificado (o si nunca hubo
    escritura RM): `session_begin`, eventos, ventanas, `trip`/`post_trip` y `session_summary`.
  - Con AUTO no verificado no escribe nada.
- **`agent/rusd_guard_reader.h`**:
  - Lector instrumentado solo para el guard. Lee las mismas 5 secciones con el mismo `RusdReadData()` y guarda
    su booleano y el timestamp vivo antes y después.
  - `rusd_snapshot()` no cambia: la GUI y el session reader siguen igual.
- **Guard (`fan_rm_guard_v04.h`):**
  - `safety_metrics` añade `rusd_cause` (p. ej. `gpu:seqlock_timeout`) y `rusd.{gpu,mem,power,fan,util}` con
    `ts`, `ts_s64`, `first_ok` y `retries`. `reason` se mantiene igual (compatibilidad con GUI y servicio).
  - La línea `DIAG` de `guard-last.stderr.log` añade la causa y los timestamps brutos.
  - En un trip por frescura de sensores, y **solo después de AUTO verificado**, toma 6 muestras de solo lectura
    (~1,5 s, en memoria) para ver si la fuente se recupera. No cuentan en las estadísticas de la sesión.
  - **0.15.7:** las líneas `safety_metrics`, `DIAG`, `termination` y los registros de fallo RM se formatean en
    memoria y se escriben después del intento de AUTO, en el mismo orden.
- **Servicio (`physical.py`):**
  - Abre `rusd-flight.jsonl` (root, 0600, `O_NOFOLLOW`, rotación a `.1` a partir de 8 MiB) y lo pasa como fd
    heredado (`NVMETRICS_RUSD_FR_FD`) junto con `run_id` (`NVMETRICS_RUSD_FR_RUN`) para correlacionarlo con
    `controller-events.jsonl`.
  - `guard_terminated_unexpected` añade `rusd_cause`.
  - Cualquier fallo del sink lo desactiva, nunca la sesión.
- **`diagnostics/rusd_flight_report.py`:** resume el log a partir de `session_summary.causes` e indica, para cada
  causa, el origen posible y su grado de certeza.

## 4. Corrección mínima

Si `RusdReadData()` falla y deja `WRITE_IN_PROGRESS`, la sección se relee completa, con la misma función:

- **Límites (0.15.8):**
  - 1.er reintento inmediato; después, espera activa de 40 µs entre intentos.
  - Ningún intento empieza pasados 2 ms de observación (reloj monotónico).
  - Tope de 64 llamadas.
  - Un único intento inmediato adicional tras una interrupción del planificador.
  - **Sin `nanosleep()`:** el de 0.15.7 tardó 13 388 µs en hardware y dejó el reintento en 0 (sección 00).
- **Sin datos antiguos:** un reintento correcto es una copia nueva y su timestamp sigue pasando `nvm_ok()` y el
  reloj de frescura de 3 s del guard (pruebas R3/R4: dato nuevo, no el anterior).
- **No oculta una pérdida real:** `ts = 0` nunca se reintenta (R2). Un `WRITE_IN_PROGRESS` persistente sigue
  siendo inválido y el guard dispara igual que antes (R5, escenarios 3 y 3b).
- **Sin cambios de límites ni de AUTO:** no se toca ningún umbral, el heartbeat, el watchdog, el rescate ni el
  protocolo RM SET/GET.
- **Presupuesto:** 2 ms de observación por sección, más como máximo una interrupción del planificador seguida de
  un intento inmediato (R5, R6, R12).

Cada reintento, recuperado o no, queda registrado. Cómo interpretarlo:

- **`seqlock_retry_recovered > 0` en hardware:** muestra que hay colisiones de lectura con este productor y que la
  relectura las absorbe. No separa por sí solo consumidor y productor: la ventana de escritura la fija el
  productor.
- **`ts_zero_producer_invalid`:** es la única causa atribuible con certeza alta (al productor). Esta corrección
  no aplica y el guard seguirá restaurando AUTO de forma segura.
- **`producer_write_persistent` (0.15.8):** WIP en cada observación durante ≥2 ms sin interrupciones. Apunta al
  productor con certeza alta en cuanto a la duración observada. La corrección no aplica y el guard restaura AUTO.
- **`write_in_progress` / `seqlock_timeout` como causa final:** solo aparecen en registros 0.15.7. Son no
  atribuibles, sobre todo si `retries=0`.

## 5. Pruebas

Las nuevas están en `run_tests.sh` (las usa `Build.command`):

- **`tests/test_rusd_flight_recorder.c`:** compila contra el **`cl00de.h` real** y su `RusdReadData()`, con un
  productor simulado que publica durante la copia o mantiene `WRITE_IN_PROGRESS`.
  - **Lector, 11 casos:** normal, ts 0, WIP recuperado, publicación durante las 10 copias, WIP permanente acotado,
    presupuesto, marcador seq, sección retirada, copia coherente, regla de validez idéntica a la de NVIDIA,
    snapshot del guard.
  - **Recorder:**
    - todas las causas y la evidencia de reintento recuperado;
    - ventanas en memoria: 44 ventanas, se conservan las 3 primeras y las 3 últimas, sobrescrituras contadas;
    - **0 `write()` hasta `finish`**, que es escritor único e idempotente;
    - trip/`post_trip`;
    - coste de ~50 ns por muestra.
- **`tests/test_guard_rusd_scenarios.c`:** ejecuta el **`nvm_v04_guard_cli()` real** en un proceso hijo, con RM
  falso de dos ventiladores, reloj falso y productor RUSD con guion. Cubre:
  - lecturas normales;
  - GPU con ts 0;
  - GPU con ts inválido (seqlock agotado);
  - productor en WIP;
  - ts congelado;
  - cambio durante la copia recuperado;
  - ts hacia atrás;
  - varios sensores a la vez;
  - transitorio y recuperación;
  - fallo persistente con AUTO;
  - salida y resumen;
  - **ningún SET con datos críticos no verificados** (START y LOWER);
  - RM rechaza un SET de RAISE (el registro del fallo sale después de AUTO, en el orden de siempre);
  - AUTO no verificado (0 escrituras del recorder, sink vacío, sin `DIAG`);
  - sesión larga con 16 transitorios;
  - flush no bloqueante sobre una tubería llena.

  **Invariantes en las 16 ejecuciones** (puntos 1 y 2 de la revisión):
  - 0 `write()` del recorder con el firmware en Manual o antes de AUTO;
  - stdout y stderr no crecen entre el último dato y AUTO;
  - AUTO llega en ≤5 ms;
  - stderr no crece durante Manual;
  - stdout durante Manual solo contiene `ready`, `manual` y `lower_refused`.
- **`tests/test_rusd_flight_service.py`:** sink root-only con `run_id`; causa auditada; el flujo de rescate tras
  `gpu_temperature_timestamp_invalid` termina en el mismo estado bloqueado que antes; rotación; rechazo de
  ficheros inseguros y symlinks; analizador (cifras del resumen, sesiones incompletas, certeza por causa).
- **`tests/test_package.py`:**
  - `test_upgrade_from_0156_is_not_same_and_preserves_its_backups`, que falla con la versión anterior;
  - `test_0156_receipt_with_wrong_agent_is_refused`;
  - `test_one_revision_everywhere` pasa con 0.15.7.
- **`tests/test_guard_header_compile.c`:** solo se añade el fake del nuevo lector. Sus 22 casos RUSD_CONFIRM y
  las aserciones RM no cambian.

Resultado en Linux (clang 18 y gcc 13; `run_tests.sh` con shim de `xcrun`, `cp` en lugar de `ditto`):

- **148 pruebas Python OK.**
- **Todas las suites C OK** (base y overlay, incluidas las 3 nuevas).
- **1244 líneas JSON válidas en 33 archivos.**
- **Swift no ejecutado** (no hay `swiftc` en Linux; no se tocó código Swift).

**Pendiente en tu Mac:** ejecutar el `run_tests.sh` real dentro de `Build.command`.

## 6. Archivos

**Nuevos:**
- `agent/rusd_flight_recorder.h`, `agent/rusd_guard_reader.h`
- `tests/test_rusd_flight_recorder.c`, `tests/test_guard_rusd_scenarios.c`, `tests/test_rusd_flight_service.py`
- `diagnostics/rusd_flight_report.py`
- `verification/rusd-flight-SIMULATED/*` (ejemplos)
- este informe

**Modificados en 0.15.7 (revisión):**
- `agent/rusd_flight_recorder.h` (sin E/S salvo `finish`, ventanas en memoria)
- `agent/fan_rm_guard_v04.h` (`struct nvm_out`, secuencia de salida con AUTO primero)
- `diagnostics/rusd_flight_report.py`
- tests (`test_rusd_flight_recorder.c`, `test_guard_rusd_scenarios.c`, `test_rusd_flight_service.py`,
  `test_package.py`)
- versión: `package_install.py`, `agent/nvmetrics_agent.c`, `service/server.py`, `build_package.sh`,
  `Build.command`, `Verify.command`, `verify_package.sh`, README y `LEEME-GPU-CAPS.md`, `PROBAR-FASE3.command`

**Modificados en la primera entrega:**
- `agent/fan_rm_guard_v04.h`, `agent/nvmetrics_agent.c` (1 include)
- `service/physical.py`
- `tests/test_guard_header_compile.c` (fake)
- `run_tests.sh`
- `build_package.sh` (comprueba que el binario lleva `NVMETRICS_RM_V0157_RUSD_FLIGHT_RECORDER`)
- `CHANGELOG.md`

## 7. Riesgos y compatibilidad

- **Latencia:**
  - En el peor caso, con las 5 secciones bloqueadas en WIP, una iteración del guard tarda ~10 ms más.
  - Con secciones sanas el coste es de 2 lecturas volátiles por sección.
  - Ninguna E/S diagnóstica precede a AUTO.
- **Diagnósticos que se pierden en algunos casos (0.15.7):**
  - Con AUTO no verificado no se escribe el recorder ni la línea `DIAG` (stdout sí, sin bloquear).
  - Si el guard muere por `SIGKILL`, se pierde lo que estaba en memoria.
  - La causa de esas sesiones queda en `controller-events.jsonl` (`auto_failed`, `guard_terminated_*`).
- **Notas diferidas:** `RUSD_CONFIRM` y `telemetry_gap` ya no aparecen en `guard-last.stderr.log` en el momento,
  sino al terminar. Un buffer de 32 KB por flujo; si se llena, se descartan mensajes completos y se cuentan.
- **~1,5 s más de vida del guard tras un trip de frescura:**
  - Solo con AUTO ya verificado, y los eventos de stdout ya están entregados.
  - En ese intervalo la GUI puede seguir mostrando Manual (es AUTO).
  - Un RAISE o LOWER en ese intervalo lee los eventos del trip y falla de forma cerrada.
  - El rescate independiente arranca ~1,5 s más tarde, con AUTO ya verificado.
- **Fallo RM en RAISE/LOWER/START:** el servicio recibe el evento `failure` tras el intento de AUTO, que en el
  peor caso tarda ~0,5 s; espera hasta 4 s.
- **Disco:**
  - Como máximo ~6×33 + 128 muestras por sesión (~0,5 MB), escritas una vez al final.
  - Rotación a 8 MiB con una generación anterior.
- **Compatibilidad:**
  - Los valores de `reason`, el orden de los mensajes en el cable y las rutas existentes no cambian; los campos
    nuevos son aditivos.
  - Si no hay sink, el guard deja el resumen en `guard-last.stderr.log`.
- **Versión e instalación:** 0.15.7. Migra desde 0.15.6 (y anteriores ya soportadas) como `upgrade`, conservando
  `.v0156-package-backup`. Tu script de auditoría externo debe aceptar la revisión IPC `0.15.7`.
- **Driver:** no hay parche. El productor es firmware GSP cerrado y no hay evidencia de un fallo en el kext.

## 8. Compilación

```bash
git clone --single-branch -b fix/rusd-flight-recorder <tu remoto>/NVMetrics.git
bash NVMetrics/rm-controller/Build.command NVMetrics/rm-controller/sources/NVMetrics <driver> <salida-nueva>
```

## 9. Validación en tu Mac (supervisada, 5–15 minutos)

No valida el fallo original: confirma que el build se comporta de forma segura, que el nuevo reintento está activo
y que el log se genera.

1. Antes de actualizar: AUTO activo y verificado. Instala `NVMetrics-0.15.8-Candidate-Intel.pkg`; debe hacer
   *upgrade* desde 0.15.7, no responder "already verified".
2. El ping del servicio debe dar `revision 0.15.8`, y `guard-last.stderr.log` debe mostrar
   `NVMETRICS_RM_V0158_RUSD_SPIN_RETRY` al iniciar Manual.
3. Manual 50 % → 30 % (las mismas condiciones que el incidente), un RAISE y un LOWER, y AUTO desde la GUI tras unos
   minutos. Comprueba que AUTO se verifica y que el rescate termina como antes.
4. Mientras Manual está activo, `rusd-flight.jsonl` no debe cambiar de tamaño (`ls -l`).
5. Al terminar: `sudo python3 diagnostics/rusd_flight_report.py /Library/PrivilegedHelperTools/org.nvmetrics.rmguard/rusd-flight.jsonl`.
   Mira `caminos_reintento`, `recuperados`, `interrumpidos` y `max_gap_us`:
   - **`caminos_reintento > 0` y `recuperados` igual:** las colisiones se absorbieron sin trip.
   - **Si aparece un trip:** la sección `TRIP` dice si fue `scheduler_preempted`, `producer_write_persistent` o
     `retry_budget_exhausted`.
   - **Si el comando devuelve "No concluyente" (código 1):** no hay datos todavía.
6. Una sesión corta no demuestra la corrección: el fallo es intermitente.

## 10. Ejemplos de log: **SIMULADOS** (generados por `test_guard_rusd_scenarios`, no por hardware)

En `verification/rusd-flight-SIMULATED/` (ver `LEEME.txt`):
- `incident_0157`: el registro exacto del 18-nov con 0.15.7 (dispara).
- `incident_0158`: el mismo arranque con 0.15.8 (recuperado; Manual continúa).
- `gpu_preempted`, `gpu_producer`: los nuevos finales de reintento.
- `trip_attribution`.
- `gpu_recovered`, `raise_rm_fail`, `auto_unverified`.
- Informes del analizador en `*.report.txt`.

Extracto de `incident_0157.report.txt` (simulado, datos del incidente):

```text
  salida=8 (safety_condition) ... caminos_reintento=1 con_reintentos=0 recuperados=0 retry_max_us=13388 ...
  TRIP gpu:write_in_progress en seq=30 (safety_condition)
    seq=30 ... gpu.cause=write_in_progress ts=18446744073709551615 ts_s64=-1 ... reintentos=0 retry_us=13388 fin=none
  ATENCIÓN: 1 camino(s) de reintento terminaron SIN ningún reintento (firma del 18-nov: el nanosleep de 0.15.7 ...)
Lectura (solo lo que permiten estos datos):
  1 camino(s) de reintento sin ningún reintento: la lectura no se repitió, así que esos fallos no informan sobre el
  productor (comportamiento corregido en 0.15.8).
```

Extracto de `incident_0158.rusd-flight.jsonl` (simulado): `"first_ok":0,"first_cause":"write_in_progress","retries":2,
"retry_us":13401,"retry_end":"recovered","preempted":1,"max_gap_us":13388`.
