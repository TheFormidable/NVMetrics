# ROG Control como referencia técnica (no visual)

Referencia: [thanhnndev/rog-control-macos](https://github.com/thanhnndev/rog-control-macos), `AsusWmi`, `RogControl.swift`, `AmdCppcPolicy.c`, `docs/ASUSCTL-MACOS.md` y `docs/CPU-POWER.md`. No se copia código ASUS ni se introduce otro kext. El código de NVMetrics sigue usando NVRM, RUSD, GPU Caps y el supervisor RM existente. Se conserva el dashboard, icono y tema.

## Candidato 0.16.1

| Idea de referencia | Adaptación | Estado |
|---|---|---|
| Doctor centralizado, reporte exportable | Diagnóstico de RUSD, temperaturas/potencia, slots observados, contadores Xid/GSP/RM, ABI GPU Caps, Eco, boost e IPC | Implementado; portable probado; GUI pendiente de prueba nativa |
| Estado desconocido explícito | Datos ausentes no son cero ni demuestran Fan Stop; Doctor no infiere permisos | Implementado |
| Recuperación después de wake | Reinicio del agente de monitorización, sin reactivar Manual | Implementado; validar sueño real |
| Límite de trabajo del monitor | Tramas de hasta 64 KiB, historial de hasta 1200 muestras y 5 minutos; descartar datos antiguos | Implementado; validar macOS |
| Backend separado | Se mantiene C + supervisor Python/rescate y SwiftUI; plazo absoluto IPC | Implementado parcialmente |
| Versión y migración | Marcador nuevo, copias independientes, pruebas 0.15.8 → 0.16.1 | Simulación probada |
| Comprobaciones sin hardware | Python y typecheck del overlay real en macOS CI | Implementado |

Doctor se ejecuta desde el menú Diagnóstico o, sin sudo, mediante:

```sh
/Applications/NVMetrics.app/Contents/Resources/GuardTools/Runtime/nvmetrics-runtime doctor
```

No ejecuta pruebas de carga, RM SET, fan-inspect privilegiado ni cambios de perfil. No garantiza que la GPU esté calibrada; el informe IPC solo prueba una respuesta autenticada. Puede devolver avisos si un servicio ocupa el socket. Un informe de Doctor no certifica seguridad térmica.

## Próximas fases, con criterios de aceptación

1. **Observabilidad nativa**: comprobar componentes NVRM/NVAccel/NVRMFB, compatibilidad entre las versiones instaladas y activas, reinicios y fallos de Metal; ocultar identificadores y ofrecer pasos de corrección no destructivos. Medir overhead CPU/RSS del monitor y coste de cada proceso por configuración.
2. **Política persistente**: perfiles declarativos Eco/reposo/boost, representación separada de preferencias y readback; aplicar solo cambios verificados y reversibles. No guardar/reaplicar Manual al arrancar o despertar. Probar permisos, arranque, cierre y cambios concurrentes.
3. **Enlace nativo**: prototipo Swift/C para IPC con autenticación por identidad de firma, protocolo versionado y test de equivalencia. No retirar el supervisor Python ni rescate independiente hasta demostrar equivalencia bajo pérdida de proceso, bloqueo, sleep/wake, heartbeats y fallo de readback. No incorporar nuevas escrituras al kernel por comodidad.
4. **Ventilación**: ROG lee y escribe curvas en ACPI/WMI del portátil. Aquí RM no ofrece un contrato universal verificado de curvas; las observaciones AUTO no son consignas programables. Requerir cualificación de identificación/controlador, correspondencia de tacómetros, límites, lectura de curva y restauración AUTO antes de editar curvas o ampliar familias.
5. **Benchmark**: herramienta opcional que mida FPS y frametime de una carga elegida por el usuario junto con temperatura, energía y clocks. No introducir cargas en Doctor ni modificar Manual durante el benchmark. Comparar p50/p95/p99, medias y estabilidad bajo el mismo driver y configuración.
6. **Instancia única, login y distribución**: bloqueo por usuario, restauración de preferencias no peligrosas, firma/notarización, empaquetado y desinstalación verificable. No usar `sudo` recurrente ni relajar el modelo de autenticación.

## Validación requerida

Antes de etiquetar 0.16.1 como estable: CI macOS completa; compilar e instalar candidato; confirmar que el dashboard es idéntico; verificar Doctor con driver presente/ausente; comparar telemetría y consumo CPU/RSS; cambiar cadencia 0,5/1/2 s repetidamente; dormir/despertar con AUTO; probar interrupción del agente en AUTO; después la regresión de Manual supervisado y retorno AUTO con hardware cualificado. No ejecutar SET desde una suite automatizada ni publicar un paquete no probado.
