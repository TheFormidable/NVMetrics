# NVMetrics

Monitor y controlador supervisado de GPU NVIDIA para macOS Intel con NullMoth. **El código de desarrollo actual reside en [`rm-controller/`](rm-controller/README.md).**

## Candidato 0.16.1

Sobre Carbon 0.15.8: conserva el dashboard, icono, guard RUSD y Flight Recorder. Integra Doctor y recuperación de telemetría; admite migración verificada desde 0.15.8. No incluye el descenso directo experimental de 0.15.9.

## Estructura del proyecto

| Ruta | Finalidad |
| --- | --- |
| [`rm-controller/app/`](rm-controller/app/) | Interfaz SwiftUI actual: dashboard, ventiladores, diagnósticos y preferencias |
| [`rm-controller/agent/`](rm-controller/agent/) | Agente C y guard nativo del controlador |
| [`rm-controller/service/`](rm-controller/service/) | Servicio privilegiado Python, supervisión y recuperación a AUTO |
| [`rm-controller/client/`](rm-controller/client/) | Cliente IPC |
| [`rm-controller/tests/`](rm-controller/tests/) | Pruebas automatizadas sin escrituras en GPU |
| [`rm-controller/sources/NVMetrics/`](rm-controller/sources/NVMetrics/) | **Base histórica fijada**, necesaria para el overlay del empaquetador y sus comprobaciones de integridad |
| [`rm-controller/sources/nvidia-macos-driver/`](rm-controller/sources/nvidia-macos-driver/) | Fuente/parches del driver usados como insumos reproducibles de compilación; NVMetrics no instala el driver |

Las antiguas carpetas de raíz `app/` y `agent/` eran una implementación previa duplicada, **no** el código que genera el paquete actual. Se retiraron del árbol activo para evitar editar archivos equivocados. Su historial sigue disponible en Git.

## Compilar

En macOS **Intel x86_64**, con el checkout del driver compatible y las herramientas indicadas en [la guía](rm-controller/README.md):

```bash
bash rm-controller/Build.command \
  "$PWD/rm-controller/sources/NVMetrics" \
  "$HOME/gpu-caps-phase2B" \
  "$PWD/release-v0161"
```

El instalador resultante se genera **fuera del árbol versionado**, en el directorio de salida elegido. Antes de instalar o probar control manual, verifica el estado AUTO. No es una versión certificada para control de ventiladores universal; consulta [seguridad](SECURITY.md) y [GPU Caps](rm-controller/GPU-CAPS.md).

## Distribución

**No se suben `.pkg`, `.dmg`, aplicaciones compiladas ni cachés al repositorio de código fuente.** Los instaladores se deben publicar como archivos adjuntos a [GitHub Releases](https://github.com/TheFormidable/NVMetrics/releases), junto a su SHA-256. La distribución anterior 0.14.7 y sus hashes permanecen en el historial, sin ocupar el árbol actual.

## Validación

La automatización [`.github/workflows/rm-controller.yml`](.github/workflows/rm-controller.yml) ejecuta pruebas de regresión. También puedes utilizar:

```bash
bash rm-controller/run_tests.sh
```

En macOS, las pruebas no sustituyen la validación con GPU real del guard, del rescate independiente y de las lecturas RM. Los cambios en interfaz no otorgan permisos adicionales de control.
