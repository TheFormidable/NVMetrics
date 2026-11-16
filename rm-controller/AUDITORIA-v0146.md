# Auditoría NVMetrics 0.14.6

Fecha de trabajo: 16 de noviembre de 2026. Estado: **candidato de estabilización; validación física de Manual pendiente**.

## Procedencia y aislamiento

- Base solicitada: `NVMetrics-RM-v0145-complete-source.zip`.
- SHA-256 del ZIP original: `5ca25040d6ae0f5fc18adca1283a1828d2092b7e72dcfdd507f9cad9ea6bfa9d`.
- Extracción preservada: `../baseline-v0145/NVMetrics-RM-v0145-verified-transition/`.
- Desarrollo separado: `development-v0146/`. Se verificaron byte por byte los 138 archivos originales y las fuentes bajo `sources/` contra el ZIP.
- Repositorio de referencia: https://github.com/TheFormidable/NVMetrics, consultado sólo en lectura. Esta revisión parte del ZIP indicado, no se presenta como sincronizada con el HEAD remoto. No se hizo push, PR, publicación ni reemplazo del repositorio oficial.
- La documentación del ZIP se trató como contexto y afirmaciones a comprobar, no como autorización para ejecutar ensayos ni desplegar.

## Hallazgos y correcciones

| Área | Fallo observado en código | Corrección y cobertura |
|---|---|---|
| AUTO durante START | No había token; la petición se perdía antes de empezar la reducción | Se conserva la intención y se solicita AUTO en cuanto START devuelve el token; test del modelo real |
| AUTO fallido | La GUI podía volver a enviar heartbeat manual tras un error de transporte | Se detiene la renovación y se espera al guard/rescate; regresión comprueba que no aparecen nuevos heartbeat |
| Respuestas de interfaz | Un sondeo de Eco/Reposo iniciado antes de un cambio podía sobrescribirlo; una tarea cancelada podía aplicar una respuesta tardía | Revisión de estado en Controls y comprobación de cancelación después del await; test de sondeo OFF antiguo frente a cambio ON |
| Estado de Eco/Reposo | Un retorno con estado distinto podía anunciar éxito; un valor desconocido de Reposo se mostraba como OFF | Estado desconocido sigue desconocido y éxito exige coincidencia; pruebas de ambas funciones independientes |
| Subprocesos | Lectura secuencial de stdout/stderr podía bloquear la GUI/IPC si stderr llenaba su pipe | Drenaje simultáneo; test con 200 000 bytes en stderr y confirmación en stdout |
| Frescura térmica | Sólo se vigilaba el avance temporal GPU; VRAM/potencia/RPM podían conservar un timestamp válido pero congelado | Cuatro relojes independientes, avance previo a START y máximo sin cambios de 3 s; simulación de cada fuente congelada y reloj regresivo |
| Política RM | No se revisaba continuamente que el bloque por defecto siguiera siendo AUTO; el restaurador validaba el diseño pero no sus modos | Validación de ambos registros por defecto antes/después de SET y en vigilancia; restauración rechaza defaults no AUTO |
| RPM | El supervisor Python omitía el perfil de inactividad a ≥50 %; podía ignorar canales adicionales | Todos los porcentajes manuales quedan supervisados, con exactamente dos tacómetros; tests de stall a 100 % y canal extra |
| Fallos al bajar | Error del sondeo inicial podía salir antes de solicitar recuperación | Solicitud inmediata de AUTO; prueba de fallo de sensor y de lectura RM sin SET posterior |
| Motivo del disparo | Una transacción podía consumir el evento térmico antes de que la rutina de salida lo leyera | Conservación del evento ya leído y clasificación correcta del rescate; test con evento de VRAM consumido |
| Estado del rescate | Algunos fallos no quedaban marcados como pendientes y la interfaz no reflejaba su resolución | Bloqueo de seguridad y resultado independiente visibles; tests de AUTO y rescate fallidos |
| Instalador | Versión anterior no distinguía esta revisión ni sus respaldos | Versión 0.14.6, migración 0.14.5 verificada, respaldo propio, salida no sobrescrita y runtime único sin lanzadores Python antiguos |

## Validación ejecutada

| Comprobación | Resultado |
|---|---|
| Pruebas Python originales | 83/83 antes de editar |
| Suite Python final | 93/93: originales y 10 regresiones/migración adicionales |
| Suites C | 9 suites aprobadas: 2 del guard y 7 originales de telemetría, Eco, boost, salud, protección y ventiladores |
| Suites Swift | 2 aprobadas: política y modelo real de interfaz/Controls |
| Suite shell | Configuración de Reposo: aprobada, incluyendo permisos, symlinks e idempotencia |
| Transiciones simuladas | 30→100 y 100→95→…→30; selección sin SET; AUTO durante START y descenso; rechazo/recovery y pérdida IPC |
| Guard nativo | Umbrales, heartbeat, datos inválidos/congelados, wire RM, los 15 destinos 30–100, AUTO con verificación y tres intentos máximos |
| Compilación nativa | SwiftUI y agente C x86_64, SDK Xcode del Mac, mínimo de destino macOS 14 |
| Autonomía | 49 Mach-O con x86_64; sin referencias absolutas a bibliotecas externas no pertenecientes al sistema; runtime arranca con entorno limpio sin Python externo |
| Paquete | Comparación exacta de ZIP, manifiesto de integridad, extracción de `.pkg` final y `codesign --verify --deep --strict`: aprobadas |
| Firma | App ad hoc local; instalador sin firma Developer ID ni notarización |

La compilación completa y las pruebas están registradas en `verification/final-build.log`. Las pruebas se ejecutaron sin escribir controles GPU.

## Lecturas reales de RTX 5050 / NullMoth

Máquina: macOS 26.7.3 (25G342), x86_64. `system_profiler` identifica una NVIDIA GeForce RTX 5050, dispositivo 0x2d83, 8 GB. El agente extraído del paquete final abrió el driver y leyó:

- GPU: 52.4 °C; VRAM: 74.0 °C.
- Potencia: 49.13 W; utilización: 7 %.
- RPM: 1149 y 1146.
- RM: AUTO en ambos registros de la política regular y de la política por defecto; diseño de dos ventiladores y candidatos 1150/3700 reconocidos.

Estas son lecturas puntuales, no valores de referencia ni prueba de regulación manual. El diagnóstico de frescura de 10 s observó 40 muestras: la primera se considera no confirmada por diseño y las 39 siguientes mostraron avance de las cuatro fuentes. El código del diagnóstico sólo llama a apertura RM, mapeo RUSD y lectura; no llama a SET, boost, Eco ni a la rutina de guard físico.

Los primeros intentos de lectura dentro del sandbox no pudieron abrir IOKit. Con autorización de ejecución fuera del sandbox, las lecturas anteriores finalizaron correctamente. No se instaló la app, no se reemplazó el helper activo ni se modificaron ventiladores o modos del usuario.

## Pendiente para declararla estable

1. Instalar/actualizar este paquete en el equipo, comprobar el arranque bajo launchd y la reversión real de una instalación fallida. Las transacciones y rollback están probados con dobles de sistema, no con un reemplazo del servicio real.
2. Ensayo presencial en reposo de Manual: arranque 50→30, aumento 30→100, descenso por escalones, AUTO durante una transición y verificación de ambos tacómetros. No se ejecutó ningún SET físico durante este trabajo.
3. Ensayo controlado de cierre de app y pérdida del cliente/servicio, comprobando la recuperación independiente con este runtime congelado. Los escenarios de fallo están cubiertos por simulación, no por provocar fallos en la GPU real.
4. Validación escalonada de carga antes de usar porcentajes bajos con juegos. No se aumentaron límites térmicos ni se dedujo una curva VBIOS de las RPM observadas.

No hay garantía de recuperación si se bloquea el kernel/driver/firmware. Se conserva la autorización por UID del servicio original; no autentica la identidad firmada de la GUI. Otros equipos, desinstalador soportado, autenticación XPC y distribución firmada/notarizada siguen pendientes, sin presentarlos como implementados.

## Artefacto final

- Archivo: `NVMetrics-0.14.6-Candidate-Intel.pkg`.
- Tamaño: 9,685,223 bytes.
- SHA-256: `80ed93dfb1dfd4aad80c531a810b5184d57286772e8a60c38c98b3a306c641a8`.
- El usuario final requiere macOS Intel compatible y NullMoth instalado; no necesita herramientas de desarrollo ni dependencias descargadas por este instalador.
