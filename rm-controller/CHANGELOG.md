# 0.14.7, añadido tras compilar el paquete — Reposo al momento (2026-11-16)

Con el driver NullMoth 3.0.1 o posterior, el interruptor de Reposo escribe el archivo y, en la misma petición de contraseña, cambia la política de boost del driver: las apps ya abiertas dejan de pedir relojes máximos, o vuelven a pedirlos, sin cerrar sesión. Con un driver anterior se comporta como antes. El agente gana la orden `policy [status|idle|normal|undecided]`. La respuesta de las dos órdenes se lee con cualquier fin de línea, porque `do shell script` las devuelve unidas por un retorno de carro. Cuando el archivo y la política no coinciden, el interruptor muestra lo que el driver aplica, la ventana lo avisa y el registro de la curva AUTO etiqueta Reposo con ese mismo estado. No cambia nada del control de ventiladores, del guard ni del rescate.

La copia del driver pasa a la 3.0.1 (cuatro parches nuevos de NVK, `build/build_nvk.sh`, `NOTICE` y dos licencias) y la compilación usa `agent/extract_rm_layer.py` de esta revisión, que extrae la cabecera de la política. Pruebas nuevas: `tests/test_policy_cli.c` y tres casos en `tests/test_ui_state.swift`. `build_package.sh` se detiene si el agente queda sin la orden `policy`.

El instalador de `packages/0.14.7/` es anterior a este cambio y no lo incluye: hay que volver a compilar el paquete. Nada de esto se ha compilado ni ejecutado en un Mac; las medidas de `AUDITORIA-v0147.md` son del paquete anterior.

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
