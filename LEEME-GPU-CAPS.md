# NVMetrics 0.15.9 — icono macOS y GPU Caps

Código completo de NVMetrics con el agente y la interfaz integrados. No incluye un nuevo kext. Consulta [arquitectura y evidencia](rm-controller/GPU-CAPS.md).

En macOS Intel, con el driver Phase2B cargado y su checkout en `~/gpu-caps-phase2B`, ejecuta desde esta carpeta `bash PROBAR-FASE2C.command`. Reutiliza la caché de encabezados de la sonda cuando está disponible. De lo contrario descarga los encabezados en `out/header-cache`. No compila LLVM ni Mesa, no usa sudo y no cambia la instalación activa.

El script crea un directorio único `out/run.*` con el agente y dos resultados JSON: `caps.jsonl` (selector 14 del driver) y `caps-live.json` (observaciones de RUSD). Envía ambos resultados para la validación.

Las máscaras identifican canales observados, no límites físicos ni autorización de control. Un ventilador detenido no implica ausencia del canal. El diagnóstico no altera la política térmica ni activa acceso manual; `manual_authorized` es siempre false. La interfaz está integrada; la lista de autorización manual no se ha sustituido.

La compilación e instalación nativas y la validación RUSD fueron confirmadas en macOS 26.7.3 Intel con RTX 5050. Para reproducir desde la raíz del código:

```bash
bash PROBAR-FASE3.command
bash rm-controller/Build.command "$PWD/rm-controller/sources/NVMetrics" "$HOME/gpu-caps-phase2B" "$PWD/release-v0159"
```

Usa un directorio de salida nuevo si ya existe el paquete. El resultado es `NVMetrics-0.15.9-Candidate-Intel.pkg`; la prueba previa no instala nada y el paquete no modifica el driver. Esta revisión requiere validación nativa; antes de actualizar, selecciona AUTO y verifica su restauración. Conserva los respaldos anteriores.
