# Gaming Lab y optimización 0.17.0

La interfaz Carbon principal no cambia. Abre Gaming Lab desde el menú de NVMetrics o Ajustes. Las funciones nuevas no invocan RM SET de ventiladores, no reactivan Manual, no cambian los límites de potencia y no instalan el driver.

## Perfiles

| Perfil | Cadencia | Solicitud de boost |
|---|---|---|
| Escritorio | 1 s | Automático según carga |
| Gaming | 0,5 s | Automático según carga |
| Ahorro | 2 s | No pide boost |
| Rendimiento | 0,5 s | Solicitud máxima mientras la app está en ejecución; protección térmica existente |

Los perfiles son elecciones explícitas, no cambios privilegiados de Eco o Reposo. Se guardan las preferencias de cadencia y boost. La cadencia adaptativa opcional sube a 0,5 s tras 2 s de carga sostenida de al menos 25%, usa 1 s en actividad intermedia tras 5 s, y baja a 2 s tras 15 s de utilización de hasta 5%. No cambia la frecuencia del guard privilegiado. Cambiar de cadencia no recrea el proceso ni su sesión RM.

## Sesiones GPU

Pulsa Iniciar sesión, realiza una actividad y Terminar sesión. Exporta el CSV para comparar condiciones. La energía es una estimación trapezoidal basada en las lecturas de potencia, únicamente en intervalos válidos de hasta 5 s. La cobertura indica la duración realmente integrada; los huecos, reinicios o lecturas ausentes no se integran. Máximo 7200 muestras por sesión. No inicia cargas ni escribe configuraciones del driver.

El contador opcional `debug.nvaccel_iop_flips` es global: **no equivale a FPS de un juego**. Si falta o se reinicia, el delta queda desconocido. No se estiman frametimes a partir de ese contador.

## FPS y frametime reales

Importa un CSV UTF-8 con una columna `frametime_ms`, una duración medida por fotograma:

```csv
frametime_ms
16.67
17.00
33.33
```

El ejemplo ilustra el formato, no una captura de hardware. El analizador calcula FPS como número de fotogramas dividido por tiempo total; 1% low como inverso del promedio del 1% más lento (redondeado hacia arriba); percentiles P95/P99 de duración por nearest rank. Rechaza datos inválidos, más de 16 MiB o un millón de fotogramas. No hay captura per-game nativa mediante esta ABI: requiere una herramienta de instrumentación que entregue esos tiempos. No confundir métricas globales con rendimiento del juego.

CLI:

```sh
/Applications/NVMetrics.app/Contents/Resources/GuardTools/Runtime/nvmetrics-runtime benchmark captura.csv
```

## Medición de recursos y criterios de aceptación

Gaming Lab ofrece una medición bajo demanda de CPU y RSS por proceso instalado, sin mantener un segundo monitor residente. La medición es una instantánea de ps, no un benchmark; las páginas compartidas pueden aparecer en más de un RSS.

Compara 0.16.2 y 0.17.0 con la misma carga, configuración, cadencia y ventana abierta/cerrada. Mide CPU y RSS del conjunto app, agente y helper, por ejemplo con Monitor de Actividad. El registro AUTO requiere consentimiento si no existe una preferencia previa: su verificación doble de política y AUTO permanece independiente; activarlo desde Comportamiento AUTO permite seguir recogiendo curvas. No se promete una reducción porcentual hasta medirla en el Mac. Valida cambios de cadencia sin cambio de PID del agente, recuperación tras wake, exportación de sesión y pruebas supervisadas de Manual por separado.
