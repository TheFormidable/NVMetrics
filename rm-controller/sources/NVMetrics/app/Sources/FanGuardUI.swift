// NVMetrics RM v0.12: on-demand IPC + staged low-speed fan calibration, supervised by root C guard.
// Session lasts while NVMetrics communicates and service remains active (up to 8 h).
// launchd owns the on-demand IPC socket. Physical control remains supervised and fail-closed.
import Foundation
import SwiftUI
import AppKit

@MainActor
final class FanGuardUIModel: ObservableObject {
    @Published private(set) var rmStatus = "Servicio no consultado"
    @Published private(set) var available = false
    @Published private(set) var physicalEnabled = false
    @Published private(set) var loadProfile = false
    @Published private(set) var active = false
    @Published private(set) var oneShotUsed = false
    @Published private(set) var currentPercent = 0
    @Published private(set) var manualFloor = 30
    @Published private(set) var secondsLeft = 0.0
    @Published private(set) var busy = false
    @Published var targetPercent = 55
    @Published var message: String?

    private var session: String?
    private var pulse: Task<Void, Never>?
    private var refreshing = false
    private var ending = false
    private var rearmWatch: Task<Void, Never>?

    nonisolated private static var clientPath: String {
        Agent.resources.appendingPathComponent("GuardTools").appendingPathComponent("nvmetrics_ipc_v08.py").path
    }

    // All IPC requests execute away from the main actor. The client validates that
    // the Unix socket's peer is UID 0 before sending any request.
    nonisolated private static func query(_ args: [String]) -> [String: Any] {
        let (code, output, error) = Controls.runFull("/usr/bin/python3", [clientPath] + args)
        guard let line = output.split(separator: "\n").last,
              let data = String(line).data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return ["ok": false, "error": error.isEmpty ? "Sin respuesta IPC (comprueba el servicio root)" : error] }
        if code != 0 && (object["ok"] as? Bool) != false {
            return ["ok": false, "error": "El cliente IPC terminó con código \(code)"]
        }
        return object
    }

    nonisolated private static func asyncQuery(_ args: [String]) async -> [String: Any] {
        await Task.detached(priority: .userInitiated) { query(args) }.value
    }

    private func applyStatus(_ r: [String: Any]) {
        guard (r["ok"] as? Bool) == true, let data = r["data"] as? [String: Any] else {
            available = false;physicalEnabled = false
            rmStatus = "Servicio desconectado"
            message = r["error"] as? String ?? "IPC no disponible. Instala y habilita el helper launchd de NVMetrics."
            return
        }
        available = true
        physicalEnabled = (data["physical_enabled"] as? Bool) == true
        loadProfile = (data["safety_profile"] as? String) == "attended_load_test"
        manualFloor = loadProfile ? 50 : 30
        if (data["manual_floor"] as? Int) != manualFloor { physicalEnabled = false }
        oneShotUsed = (data["one_shot_used"] as? Bool) == true
        secondsLeft = (data["seconds_left"] as? Double) ?? 0
        let running = (data["active"] as? Bool) == true
        // A root session created by another GUI instance must never be adopted
        // without its unguessable session token. Only its own owner can control
        // it; the guard/client watchdog restores AUTO when the owner is gone.
        if running && session == nil {
            active = false; oneShotUsed = true; currentPercent = 0
            rmStatus = "Sesión de otra instancia · esperando AUTO seguro"
            message = "No se puede tomar control de una sesión ajena. Espera al guard y al rescate."
            return
        }
        if running {
            active = true
            currentPercent = (data["percent"] as? Int) ?? currentPercent
            rmStatus = "Manual \(currentPercent) % · \(loadProfile ? "Carga supervisada" : "Reposo") · \(Int(secondsLeft.rounded(.up))) s restantes"
        } else {
            if active { pulse?.cancel();pulse = nil;session = nil }
            active = false;currentPercent = 0
            let endReason = data["last_end_reason"] as? String ?? "unknown"
            if endReason == "auto_independently_verified_ready" || endReason.contains("rescue_verified_restart_service") {
                rmStatus = "AUTO verificado por rescate"
            } else if endReason.contains("rescue_unverified") || endReason.contains("rescue_missing") {
                rmStatus = "Estado RM no verificado · detener pruebas"
            } else if endReason == "never_started" {
                rmStatus = "Sin sesión manual · comprueba AUTO antes de iniciar"
            } else {
                rmStatus = "Manual finalizado · confirmación AUTO pendiente"
            }
            if let reason = data["last_end_reason"] as? String,
               reason != "never_started" && reason != "auto_independently_verified_ready" &&
               reason != "user_auto_verified" && reason != "client_finished" {
                if let tx = data["last_rm_transaction"] as? [String: Any],
                   (tx["event"] as? String) == "failure",
                   let rmReason = tx["reason"] as? String {
                    message = "Control finalizado: \(reason). RM: \(rmReason). Comprueba AUTO antes de reintentar."
                } else {
                    message = "Control finalizado: \(reason). Comprueba AUTO antes de reintentar."
                }
            }
        }
    }

    func refresh() {
        guard !refreshing && !active && !busy else { return }
        refreshing = true
        Task {
            var r = await Self.asyncQuery(["control_status"])
            // launchd may still be starting the on-demand root service.
            // Retry a bounded number of times; no privileged process is spawned
            // directly from the unprivileged GUI.
            for _ in 0..<3 where (r["ok"] as? Bool) != true {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                r = await Self.asyncQuery(["control_status"])
            }
            applyStatus(r)
            refreshing = false
        }
    }

    func begin() {
        guard !busy && !active && physicalEnabled && !oneShotUsed else {
            message = oneShotUsed ? "Esperando a que el rescate root confirme AUTO antes de otra sesión." :
                                    "Inicia un servicio root con control físico habilitado.";return
        }
        busy = true;message = "Validando sensores y armando rescate…"
        Task {
            let r = await Self.asyncQuery(["manual_begin"])
            busy = false
            guard (r["ok"] as? Bool) == true,
                  let data = r["data"] as? [String: Any],
                  let token = data["session"] as? String, token.count == 32 else {
                let refusal = r["error"] as? String ?? "Manual no autorizado"
                if refusal == "preflight_power_above_35W" {
                    message = "Para iniciar Manual, el consumo debe ser ≤35 W. Detén el vídeo o juego y espera al reposo."
                } else if refusal == "preflight_utilization_above_20pct_or_unavailable" {
                    message = "Para iniciar Manual, la utilización debe ser ≤20 % y su sensor estar disponible."
                } else if refusal == "preflight_requires_AUTO" {
                    message = "El modo anterior todavía no se ha verificado como AUTO. Comprueba RM antes de reintentar."
                } else if refusal == "preflight_telemetry_not_fresh" {
                    message = "Telemetría sin actualizar: no se permite activar Manual."
                } else {
                    message = "Manual rechazado: " + refusal
                }
                refresh();return
            }
            session = token
            applyStatus(r)
            active = true
            currentPercent = 50
            oneShotUsed = true
            rearmWatch?.cancel();rearmWatch = nil
            targetPercent = 55
            message = loadProfile ? "Carga experimental: AUTO si GPU ≥65 °C, VRAM ≥88 °C, potencia >145 W, sensores o heartbeat fallan." : "Modo reposo: AUTO si GPU ≥65 °C, VRAM ≥88 °C, >35 W o >20 % de uso."
            heartbeat(token)
        }
    }

    // A dedicated task keeps the C guard alive. Closing NVMetrics stops the
    // heartbeat; the guard then requests AUTO. A second root rescue is armed.
    private func heartbeat(_ token: String) {
        pulse?.cancel()
        pulse = Task { @MainActor in
            while !Task.isCancelled && session == token && !ending {
                try? await Task.sleep(nanoseconds: 900_000_000)
                // A RAISE is a synchronous RM SET + GET transaction in the root
                // service. Do not enqueue PINGs behind that operation.
                if busy { continue }
                if Task.isCancelled || session != token || ending { break }
                let r = await Self.asyncQuery(["manual_heartbeat", token])
                guard (r["ok"] as? Bool) == true else {
                    message = "Sesión terminada o sin comunicación; AUTO por guard/rescate. " +
                              (r["error"] as? String ?? "")
                    session = nil;active = false
                    rmStatus = "Sin sesión · comprobar AUTO"
                    // The guard may have gone to AUTO on thermal / heartbeat trip.
                    // Show the real ending reason and never falsely imply verified AUTO.
                    let state = await Self.asyncQuery(["control_status"])
                    applyStatus(state)
                    watchForRescueCompletion()
                    break
                }
                applyStatus(r)
            }
        }
    }

    func raisePercent(_ requested: Int) {
        guard active && !busy && !ending, let token = session else { return }
        guard requested > currentPercent && requested <= 100 && requested % 5 == 0 else {
            message = "Solo se permiten aumentos en pasos de 5 %.";return
        }
        busy = true
        Task {
            let r = await Self.asyncQuery(["manual_raise",token,String(requested)])
            busy = false
            if (r["ok"] as? Bool) == true {
                applyStatus(r)
                targetPercent = min(100,currentPercent + 5)
                message = "Solicitud \(requested) % verificada por RM."
            } else {
                message = "Aumento rechazado: " + (r["error"] as? String ?? "desconocido")
                // Reconcile after failure even when the model still thinks MANUAL
                // is active (refresh() deliberately skips active sessions).
                let state = await Self.asyncQuery(["control_status"])
                applyStatus(state)
            }
        }
    }

    func lowerPercent(_ requested: Int) {
        guard active && !busy && !ending && !loadProfile, let token = session else { return }
        guard currentPercent <= 50 && currentPercent > manualFloor && requested == currentPercent - 5 else {
            message = "Calibración experimental: solo bajar 5 % desde 50 %, en reposo.";return
        }
        busy = true;message = "Verificando RPM de ambos ventiladores a \(requested) %…"
        Task {
            let r = await Self.asyncQuery(["manual_lower",token,String(requested)])
            busy = false
            if (r["ok"] as? Bool) == true {
                applyStatus(r)
                targetPercent = currentPercent > manualFloor ? currentPercent - 5 : manualFloor + 5
                message = "\(requested) % confirmado por RM y tacómetros."
            } else {
                let reason = r["error"] as? String ?? "AUTO solicitado"
                message = reason == "low_step_wait_five_seconds" ? "Espera al menos cinco segundos en este escalón antes de bajar." : "Calibración interrumpida: " + reason
                let state = await Self.asyncQuery(["control_status"])
                applyStatus(state)
                if !active {watchForRescueCompletion()}
            }
        }
    }

    func adjustPercent(_ requested: Int) {
        if requested < currentPercent { lowerPercent(requested) }
        else if requested > currentPercent { raisePercent(requested) }
    }

    func restoreAuto() {
        guard let token = session, !ending else { return }
        ending = true;busy = true
        pulse?.cancel();pulse = nil
        Task {
            let r = await Self.asyncQuery(["manual_auto",token])
            busy = false;ending = false
            if (r["ok"] as? Bool) == true {
                session = nil;active = false;currentPercent = 0
                rmStatus = "AUTO verificado por RM"
                oneShotUsed = true  // guard against premature restart while root rescue is active
                message = "AUTO confirmado por RM. Esperando verificación independiente del rescate…"
                watchForRescueCompletion()
            } else {
                message = "AUTO sin confirmar: " + (r["error"] as? String ?? "comprobar RM")
                let state = await Self.asyncQuery(["control_status"])
                applyStatus(state)
                // The guard may still be alive; do not accidentally abandon
                // the only heartbeat without checking its actual state.
                if active, let liveToken = session { heartbeat(liveToken) }
            }
            refresh()
        }
    }

    private func watchForRescueCompletion() {
        rearmWatch?.cancel()
        rearmWatch = Task { @MainActor in
            // The rescue detects guard exit and checks RM independently. A brief
            // polling window gives feedback without allowing overlapping sessions.
            for _ in 0..<20 {
                if Task.isCancelled || active || session != nil { break }
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                if Task.isCancelled || active || session != nil { break }
                let r = await Self.asyncQuery(["control_status"])
                applyStatus(r)
                if available && physicalEnabled && !oneShotUsed {
                    message = "Rescate root completado: puedes activar Manual de nuevo."
                    break
                }
                if let d = r["data"] as? [String: Any],
                   let reason = d["last_end_reason"] as? String,
                   reason.contains("restart_service") {
                    if reason.contains("rescue_verified_restart_service") {
                        message = "AUTO confirmado por rescate root, pero rearme bloqueado por un corte de seguridad. Reinicia el servicio después de verificar las temperaturas."
                    } else {
                        message = "Rescate sin verificación de AUTO. Detén los ensayos; comprueba RM antes de reiniciar."
                    }
                    break
                }
            }
        }
    }
}

struct FanGuardMenu: View {
    @EnvironmentObject private var fanGuard: FanGuardUIModel
    var body: some View {
        Menu("Ventiladores · Control RM") {
            Text(fanGuard.rmStatus)
            if fanGuard.active {
                Text("Solicitado: \(fanGuard.currentPercent) %")
                Button("Subir 5 %") { fanGuard.raisePercent(fanGuard.currentPercent + 5) }
                    .disabled(fanGuard.busy || fanGuard.currentPercent >= 100)
                if !fanGuard.loadProfile {
                    Button("Bajar 5 % · experimental") { fanGuard.lowerPercent(fanGuard.currentPercent - 5) }
                        .disabled(fanGuard.busy || fanGuard.currentPercent > 50 || fanGuard.currentPercent <= fanGuard.manualFloor)
                }
                Button("Restaurar Automático") { fanGuard.restoreAuto() }
            } else {
                Text(fanGuard.physicalEnabled ? (fanGuard.oneShotUsed ? "Rescate pendiente o sesión no rearmada" : "Sesión de prueba disponible") : "Servicio físico no habilitado")
                Text("Abre la ventana para iniciar Manual 50 %")
            }
            Button("Consultar servicio") { fanGuard.refresh() }
        }
        .onAppear { fanGuard.refresh() }
    }
}

struct FanGuardCard: View {
    @EnvironmentObject private var fanGuard: FanGuardUIModel
    @Environment(\.colorScheme) private var scheme
    @State private var confirmStart = false

    var body: some View {
        let th = Theme(scheme: scheme)
        return Card(icon: "fanblades.fill", title: "Control RM · sesión física protegida") {
            Text(fanGuard.rmStatus)
                .font(.system(size: 13, weight: .semibold)).foregroundStyle(th.text)
            HStack(spacing: 10) {
                if fanGuard.active {
                    Button("Automático") { fanGuard.restoreAuto() }
                        .disabled(fanGuard.busy)
                    Button("+5 %") { fanGuard.raisePercent(fanGuard.currentPercent + 5) }
                        .disabled(fanGuard.busy || fanGuard.currentPercent >= 100)
                    if !fanGuard.loadProfile {
                        Button("−5 %") { fanGuard.lowerPercent(fanGuard.currentPercent - 5) }
                            .disabled(fanGuard.busy || fanGuard.currentPercent > 50 || fanGuard.currentPercent <= fanGuard.manualFloor)
                    }
                    Picker("Ajustar a", selection: $fanGuard.targetPercent) {
                        ForEach(stride(from: fanGuard.manualFloor, through: 100, by: 5).map { $0 }, id: \.self) { pct in
                            Text("\(pct) %").tag(pct)
                        }
                    }.frame(width: 150)
                    Button("Aplicar") { fanGuard.adjustPercent(fanGuard.targetPercent) }
                        .disabled(fanGuard.busy || fanGuard.targetPercent == fanGuard.currentPercent ||
                                  (fanGuard.targetPercent < fanGuard.currentPercent &&
                                   (fanGuard.loadProfile || fanGuard.currentPercent > 50 ||
                                    fanGuard.targetPercent != fanGuard.currentPercent - 5)))
                } else {
                    Button("Manual 50 %") { confirmStart = true }
                        .disabled(fanGuard.busy || !fanGuard.physicalEnabled || fanGuard.oneShotUsed)
                    Button("Consultar IPC") { fanGuard.refresh() }
                }
            }
            Text(fanGuard.loadProfile ? "Carga supervisada: Manual 50–100 %. Bajar de 50 % está bloqueado durante carga. AUTO ante 65 °C GPU, 88 °C VRAM o fallos." : "Reposo: inicia al 50 %; baja de 5 en 5 hasta 30 %. Mínimo manual 30 %, ambos ventiladores ≥1000 RPM. Nunca uses <50 % jugando.")
                .font(.system(size: 11)).foregroundStyle(th.label).fixedSize(horizontal: false, vertical: true)
            if let text = fanGuard.message {
                Text(text).font(.system(size: 11)).foregroundStyle(th.label)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .confirmationDialog("Ensayo real de ventiladores", isPresented: $confirmStart, titleVisibility: .visible) {
            Button("Activar Manual 50 % supervisado") { fanGuard.begin() }
            Button("Cancelar", role: .cancel) { }
        } message: {
            Text(fanGuard.loadProfile ? "Prueba de carga EXPERIMENTAL: ambos ventiladores deben girar, los sensores deben responder y AUTO se activará por límites térmicos. Comienza en reposo, supervisa en persona y nunca confíes en recuperación durante un fallo kernel/driver." : "Escritura RM privada. Inicia en reposo con servicio root y supervisión. Sin garantía ante un fallo de driver/kernel.")
        }
        .onAppear { fanGuard.refresh() }
    }
}
