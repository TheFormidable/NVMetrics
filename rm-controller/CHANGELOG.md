# 0.16.0 — candidato de robustez y diagnóstico

- Doctor de solo lectura desde la barra de menús, con reporte resumido sin tokens, UID ni identificadores de hardware.
- Telemetría: descarta callbacks ajenos a la generación activa, limita tramas e historial, invalida lecturas obsoletas, no abre otro cliente RM hasta terminar el anterior y reconecta al despertar.
- IPC cliente: plazo total por solicitud, validación estricta de versión y pruebas con respuestas parciales.
- Migración de 0.15.0 con backups nuevos, sin cambiar ninguna política de ventiladores.
- CI nativa: typecheck del overlay completo que realmente compila el paquete.
- Pendiente de validación nativa y física; no es un release estable.

# 0.15.0 — Manual continuo (candidato de prueba local)

- Perfil continuo explícito: sin vencimiento de sesión ni de servicio; el guard C, watchdog del cliente y rescate independiente siguen activos. Se mantiene el mínimo de 30%, inicio a 50%, lectura RM y comprobación de tacómetros.
- Carga hasta 100% admitida; el consumo no es un límite de utilización, solo se rechazan valores inválidos o superiores a 1000 W como techo de plausibilidad. Mantiene límites térmicos conservadores y margen adicional al reducir por debajo de 50%.
- GPU no cualificada: instalación nueva con control de ventiladores de solo lectura. No implica soporte universal de escritura ni telemetría garantizada. La ruta física sigue restringida a la plataforma RTX 5050 y layout conocidos.
- GPU Caps fase 1: lectura de ABI versionada del driver. Fase 2C: descubrimiento de canales observados mediante RUSD. Fase 3: perfil incluido en telemetría y presentación dinámica de slots en SwiftUI, con compatibilidad con agentes anteriores.
- Validación nativa reportada el 17-11-2026: paquete Intel compilado e instalado, aplicación funcionando con RTX 5050 y driver fase 2B. No equivale a validación física de todo el rango manual, Fan Stop, otros modelos o todas las versiones de macOS. [Evidencia y límites](GPU-CAPS.md).

# 0.14.9 — Endurecimiento de auditoría (candidato)

- Límite absoluto de lectura IPC y timeout de respuesta; una conexión lenta no renueva el plazo.
- Eco y Reposo ejecutan únicamente las herramientas instaladas bajo la ruta privilegiada fija, con entorno limpio.
- Registro del helper de ajustes dentro del recibo y migración desde 0.14.8 con respaldos propios.
- Rotación de auditoría y rescate; archivo stderr del guard verificado y reiniciado por sesión.
- Sesiones Manual limitadas a una hora, también por el rescate independiente; se conserva el rango 30–100%, AUTO y la protección térmica.
- CI sin hardware y pruebas de regresión. Validación física y compilación nativa aún pendientes.

# 0.14.8 — Reposo al momento y menú de la barra (2026-11-16)

**Reposo al momento.** Con el driver NullMoth 3.0.1 o posterior, el interruptor de Reposo escribe el archivo y, en la misma petición de contraseña, cambia la política de boost del driver: las apps ya abiertas dejan de pedir relojes máximos, o vuelven a pedirlos, en cuanto vuelven a trabajar y sin cerrar sesión. Con un driver anterior se comporta como antes. El agente gana la orden `policy [status|idle|normal|undecided]`. La respuesta de las dos órdenes se lee con cualquier fin de línea, porque `do shell script` las devuelve unidas por un retorno de carro. Cuando el archivo y la política no coinciden, el interruptor muestra lo que el driver aplica, la ventana lo avisa al abrirse y el registro de la curva AUTO etiqueta Reposo con ese mismo estado. Si el driver no acepta el cambio de política, la ventana vuelve a leer los dos: el interruptor muestra lo que el driver aplica y, si el archivo y la política no coinciden, avisa de ello (si la política no está decidida, dice que el driver no aceptó el cambio). Cancelar la petición de contraseña ya no quita ese aviso.

**Menú de la barra.** La selección azul ya no salta cuando llega una lectura con el menú abierto: el menú y la cifra de la barra leen una copia de los valores que se queda quieta mientras hay un menú abierto y se pone al día al cerrarlo (`app/MenuHold.swift`, `app/MenuLines.swift`). Con el menú abierto sus cifras no cambian; la ventana sigue en vivo.

**Versión e instalador.** Marcador de agente `NVMETRICS_RM_V0148_BOOST_POLICY`, copias de respaldo propias (`.v0148-package-backup`, `.NVMetrics.app.before-v0148`) y actualización desde 0.14.7; el servicio responde `revision` 0.14.8. Hace falta una versión nueva porque el instalador no reemplaza una 0.14.7 por otra 0.14.7.

**Compilación.** La copia del driver pasa a la 3.0.1 (cuatro parches nuevos de NVK, `build/build_nvk.sh`, `NOTICE` y dos licencias) y la compilación usa `agent/extract_rm_layer.py` de esta revisión, que extrae la cabecera de la política. `build_package.sh` se detiene si el agente queda sin la orden `policy` o sin la cabecera de la política del driver (copia del driver anterior a 3.0.1).

**Pruebas.** Nuevas: `tests/test_policy_cli.c`, `tests/test_menu_hold.swift`, cinco casos en `tests/test_ui_state.swift` y tres en `tests/test_package.py` (actualización desde 0.14.7, agente equivocado bajo un recibo 0.14.7 y que el número de versión es el mismo en instalador, servicio, scripts y agente). En Linux pasan las 104 pruebas Python y las suites C. Nada de Swift se ha compilado y nada de esta versión se ha ejecutado en un Mac; no hay instalador 0.14.8 publicado.

# 0.14.7 — menor demora al bajar (2026-11-16)

Lector RUSD persistente sólo durante Manual, correlación de peticiones y avance independiente de sensores. Conserva todas las pausas y límites de seguridad. Mantiene el destino al rechazar una bajada e informa del valor que la bloqueó. Pruebas de errores, interfaz y migración desde 0.14.6. Medición real del lector documentada en AUDITORIA-v0147.md.

# 0.14.6 — estabilización local (2026-11-16)

- AUTO mantiene prioridad durante START y las transiciones; una respuesta fallida no reanuda el heartbeat manual.
- Frescura independiente y avance previo a START para GPU, VRAM, potencia y RPM; política por defecto AUTO verificada durante el control.
- Supervisión de ambos tacómetros también en el perfil de inactividad a 50–100 %, rechazo de canales inesperados y recuperación ante fallos del sondeo.
- Motivos de seguridad y estado del rescate preservados; rearme conservador.
- Eco/Reposo independientes, respuesta antigua descartada y éxito condicionado al estado devuelto. Drenaje concurrente de salida/error del subproceso.
- Modelo Swift probado directamente: selección, recorrido completo 100→30, aumento 30→100, AUTO concurrente y fallo IPC.
- Instalador Intel autónomo con runtime incluido, migración desde 0.14.5, respaldo separado y eliminación de lanzadores antiguos.
- Candidato local: falta validación física de Manual y del ciclo de instalación/recuperación antes de declararlo estable.

---

# Changelog

## 0.14.5 — Verified manual transitions

- Fix the reported 51.48 W / 21% preflight and 51.56 W / 18% post-write incidents. Moderate transient electrical/utilization excursions receive bounded read-only retries (no downward SET on grace; three consecutive strict post-write samples required).
- Preserve a safe, verified manual step at 30–100% when a lower step is refused without writing. No longer returns to AUTO merely because the confirmed intermediate step was below 50%.
- When post-write validation runs out of electrical margin, attempt a native verified RAISE to at least 50%, verify three physical tach samples, and stop further lowering. If recovery fails or a critical fault occurs, restore AUTO through the unchanged guard/rescue path. No automatic reapplication.
- Keep native C limits, RM wire format, safety profile, heartbeat, hardware floor and independent rescue unchanged; preserve read-only observation. Upgrade from verified v0.14.4 package receipt.

## 0.14.4 — Firmware AUTO observations

- Passive AUTO sampling with `inspect → status → inspect`, rejected during Manual or a pending rescue; no RM SET, no automatic workload, no safety policy changes.
- New firmware-behavior window, actual per-fan RPM versus GPU temperature, valid zero-RPM observations, four independent Eco/Reposo configurations, unknown state category, local bounded history and CSV export. Logs power, VRAM temperature, utilization, P-state, clocks and selected app boost mode.
- Reposo is labeled as configured, not necessarily effective until logout/login. The observations do not identify the exact VBIOS fan curve; temperature, load, hysteresis and other sensors can affect the result.
- Upgrade from a verified v0.14.3 receipt; unchanged native agent marker, guard and rescue, separate rollback backups.

## 0.14.3 — Adaptive thermal entry preview

- Split thermal entry (<60°C GPU, <84°C VRAM, ≤60 W, ≤20% use) from below-50% transition (<58°C GPU, <82°C VRAM, ≤50 W, ≤20% use). The reported 47.81 W idle sample passes both in simulation; native validation is required.
- Preserve 30% preferred startup, or explicitly retain verified 50% on insufficient low-speed margin before any downward write. Other failures still request AUTO.
- Bounded probe progress pulses prevent read-only subprocess waits monopolizing the guard heartbeat; background probes do not renew the authenticated client lease. Keep existing hard thermal cutoffs, independent rescue, and deadlines.
- Upgrade receipts support v0.14.2; pin new native agent marker and keep prior backups.

## 0.14.2 — Fan controls preview

- Manual automatically reaches the 30% minimum after verified spin-up; the user no longer needs to request the successive reductions.
- Independent selected and applied percentages, a discrete slider, dropdown and consistent ±5 staging. Apply is enabled for valid pending changes; only Apply writes to RM.
- One Apply command handles multi-step reductions and displays progress; synchronized heartbeat and RM replies prevent stale status from overwriting confirmed settings. AUTO can interrupt after the current bounded step.
- Retain thermal supervisor and independent rescue; no C guard or driver changes. Fresh installer and verified upgrades from 0.14.0/0.14.1, with separate rollback backups.
- Add native Swift selection-policy tests to the one-click builder. Native/physical validation pending.

## 0.14.1 — Thermal Supervisor preview

- Builder revision r2: includes reconstructed v0.13.4 integrated NVMetrics tree and pinned driver RM patch inputs; no prior local checkout needed. Validate sources before dependency installation. No runtime policy changes.

- Active thermal supervision accepts valid 0–100% utilization without the old 20% idle cutoff; retains 145 W experimental ceiling, 65°C GPU / 88°C VRAM limits, early thermal-rise handoff, verified spinning fans, freshness, RM state, heartbeat and independent rescue.
- Permit 5% downward steps from 100% to 30% (below 50% only after idle preflight), with physical tach verification. Start at 50% in safe idle.
- Known thermal trips can rearm after verified independent AUTO and minimum cooldown, without automatic Manual reapplication. Critical faults remain fail-closed.
- Native update supports verified v0.14.0 package receipts, backs up previous app/runtime and rolls back on handshake failure. Inherits r3 ZIP transport.
- 51 Python tests and two simulated C suites passed; real macOS build and physical validation pending.

## 0.14.0 — Installer preview

- Packaging revision r3: preserve the entire signed application and service tree in a native `ditto` ZIP, embed its SHA-256 in `postinstall`, verify a native ZIP round-trip before pkgbuild and compare the ZIP bytes after pkgbuild. Validate the app and service tree plus signature again. The installer checks the archive and app signature before executing its private staged runtime. Only successful builds publish the new `-r3.pkg` filename. Fan guard logic and IPC revision are unchanged.
- Package verifier hotfix: handle `pkgutil`'s expanded Scripts directory instead of treating it as a tar archive. Verify extracted app and service hashes, native marker, executable permissions, script content and code signature. `Verify.command` checks an existing package without recompiling or installing. No runtime or guard changes.
- Builder hotfix: locate non-system CPython automatically instead of failing when the first `python3` in PATH is Apple's older version; guide the developer to Python.org if missing. The runtime and fan-control logic are unchanged.

- Native macOS Installer package builder; single administrator authorization for the installation, GUI remains ordinary-user.
- Bundled Python onedir runtime for IPC, launchd service and independent rescue; no existing Python installation on end-user machine.
- Automated firmware AUTO verification and actual IPC connectivity test as the authorized console UID, with safe lease checks and rollback on installation failure.
- Fresh helper installation and exact v0.13.4 idle-helper migration on the tested RTX 5050 platform; same-package reinstall verifies integrity.
- GUI automatically queries and verifies RM AUTO, refreshes the status while its fan card is visible and gates Manual on verified readiness.
- Existing 0.13.4 protocol, guard thresholds, 30% floor and supervised idle restrictions preserved. Backend revision intentionally unchanged.
- 42 simulated/hardware-free tests passed. Earlier native build produced a package that failed integrity verification; the new r3 native ZIP round-trip and native install remain to be validated on macOS.
