// NVMetrics RM v0.14.3: adaptive thermal entry, staged selection, verified transitions and root supervision.
// Session lasts while NVMetrics communicates and service remains active (up to 8 h).
// launchd owns the on-demand IPC socket. Physical control remains supervised and fail-closed.
import Foundation
import Combine

@MainActor
final class FanGuardUIModel: ObservableObject {
    @Published private(set) var rmStatus = "Servicio no consultado"
    @Published private(set) var available = false
    @Published private(set) var autoVerified = false
    @Published private(set) var physicalEnabled = false
    @Published private(set) var loadProfile = false
    @Published private(set) var thermalProfile = false
    @Published private(set) var active = false
    @Published private(set) var oneShotUsed = false
    @Published private(set) var currentPercent = 0
    @Published private(set) var manualFloor = 30
    @Published private(set) var secondsLeft = 0.0
    @Published private(set) var busy = false
    @Published private(set) var targetPercent = 30
    @Published private(set) var readbackConfirmed = false
    @Published var message: String?

    private let queryIPC: ([String]) async -> [String: Any]
    private let pause: (UInt64) async -> Void
    init(query: (([String]) async -> [String: Any])? = nil,
         pause: @escaping (UInt64) async -> Void = { try? await Task.sleep(nanoseconds: $0) }) {
        self.queryIPC = query ?? Self.asyncQuery
        self.pause = pause
    }
    private var session: String?
    private var pulse: Task<Void, Never>?
    @Published private(set) var refreshing = false
    private var ending = false
    private var heartbeatInFlight = false
    private var autoRequested = false
    private var lowMarginRefused = false
    private var rearmWatch: Task<Void, Never>?

    var selectionFloor: Int {
        FanSelectionPolicy.minimum(floor: manualFloor, load: loadProfile, thermal: thermalProfile, current: currentPercent)
    }
    var canApply: Bool {
        active && available && physicalEnabled && readbackConfirmed && !busy && !ending &&
        FanSelectionPolicy.nextStep(current: currentPercent, target: targetPercent, floor: manualFloor,
                                    load: loadProfile, thermal: thermalProfile) != nil
    }
    var canBegin: Bool { !busy && !refreshing && !active && autoVerified && physicalEnabled && !oneShotUsed }
    func selectPercent(_ percent: Int) {
        guard active && !busy && !ending else { return }
        targetPercent = FanSelectionPolicy.clamp(percent, floor: selectionFloor)
    }

    nonisolated private static var clientPath: String {
        Agent.resources.appendingPathComponent("GuardTools").appendingPathComponent("Runtime").appendingPathComponent("nvmetrics-runtime").path
    }

    // All IPC requests execute away from the main actor. The client validates that
    // the Unix socket's peer is UID 0 before sending any request.
    nonisolated private static func query(_ args: [String]) -> [String: Any] {
        let (code, output, error) = Controls.runFull(clientPath, ["client"] + args)
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
            available = false;physicalEnabled = false;autoVerified = false;readbackConfirmed = false
            rmStatus = "Servicio desconectado"
            message = r["error"] as? String ?? "IPC no disponible. Instala y habilita el helper launchd de NVMetrics."
            return
        }
        available = true
        autoVerified = false
        physicalEnabled = (data["physical_enabled"] as? Bool) == true
        loadProfile = (data["safety_profile"] as? String) == "attended_load_test"
        thermalProfile = (data["safety_profile"] as? String) == "thermal_supervised"
        manualFloor = loadProfile ? 50 : 30
        if !runningSelection { targetPercent = manualFloor }
        if (data["manual_floor"] as? Int) != manualFloor { physicalEnabled = false }
        oneShotUsed = (data["one_shot_used"] as? Bool) == true
        secondsLeft = (data["seconds_left"] as? Double) ?? 0
        let running = (data["active"] as? Bool) == true
        // A root session created by another GUI instance must never be adopted
        // without its unguessable session token. Only its own owner can control
        // it; the guard/client watchdog restores AUTO when the owner is gone.
        if running && session == nil {
            active = false; oneShotUsed = true; currentPercent = 0; readbackConfirmed = false
            rmStatus = "Sesión de otra instancia · esperando AUTO seguro"
            message = "No se puede tomar control de una sesión ajena. Espera al guard y al rescate."
            return
        }
        if running {
            active = true
            let reported = data["percent"] as? Int ?? 0
            readbackConfirmed = (data["readback_verified"] as? Bool) == true && reported >= manualFloor && reported <= 100 && reported % 5 == 0
            if readbackConfirmed { currentPercent = reported }
            rmStatus = "Manual \(currentPercent) % · \(thermalProfile ? "Supervisor térmico" : loadProfile ? "Carga supervisada" : "Sólo inactividad") · \(Int(secondsLeft.rounded(.up))) s restantes"
            if !readbackConfirmed { rmStatus = "Manual · porcentaje RM sin confirmar" }
        } else {
            pulse?.cancel();pulse = nil;session = nil
            active = false;currentPercent = 0;readbackConfirmed = false;targetPercent = manualFloor
            let endReason = data["last_end_reason"] as? String ?? "unknown"
            if endReason == "auto_independently_verified_ready" || endReason.contains("rescue_verified_restart_service") {
                rmStatus = "AUTO verificado por rescate"
                autoVerified = endReason == "auto_independently_verified_ready"
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

    private var runningSelection: Bool { active || busy || session != nil }

    func refresh() {
        guard !refreshing && !active && !busy && rearmWatch == nil else { return }
        refreshing = true
        Task {
            var r = await queryIPC(["control_status"])
            // launchd may still be starting the on-demand root service.
            // Retry a bounded number of times; no privileged process is spawned
            // directly from the unprivileged GUI.
            for _ in 0..<3 where (r["ok"] as? Bool) != true {
                await pause(1_000_000_000)
                r = await queryIPC(["control_status"])
            }
            applyStatus(r)
            if available && !active && !oneShotUsed {
                let inspection = await queryIPC(["inspect"])
                let data = inspection["data"] as? [String: Any]
                autoVerified = (inspection["ok"] as? Bool) == true && (data?["mode"] as? String) == "auto"
                rmStatus = autoVerified ? "Automático · verificado" : "AUTO no verificado · Manual no disponible"
            }
            refreshing = false
        }
    }

    func begin() {
        guard canBegin else {
            message = oneShotUsed ? "Esperando a que el rescate root confirme AUTO antes de otra sesión." :
                                    "Espera la conexión y verificación automática del servicio.";return
        }
        busy = true; autoRequested = false; autoVerified = false; targetPercent = manualFloor
        rearmWatch?.cancel(); rearmWatch = nil
        message = "Validando sensores y armando rescate…"
        Task {
            let r = await queryIPC(["manual_begin"])
            guard (r["ok"] as? Bool) == true,
                  let data = r["data"] as? [String: Any],
                  let token = data["session"] as? String, token.count == 32 else {
                let refusal = r["error"] as? String ?? "Manual no autorizado"
                if refusal.hasPrefix("preflight_power_above_") {
                    message = "Consumo por encima de \(thermalProfile ? 60 : 35) W para iniciar Manual. Espera a que baje."
                } else if refusal.hasPrefix("preflight_gpu_temperature_not_below_") {
                    message = "GPU demasiado caliente para iniciar Manual: debe estar por debajo de \(thermalProfile ? 60 : 55) °C."
                } else if refusal.hasPrefix("preflight_memory_temperature_not_below_") {
                    message = "VRAM demasiado caliente para iniciar Manual: debe estar por debajo de \(thermalProfile ? 84 : 82) °C."
                } else if refusal == "preflight_utilization_above_20pct_or_unavailable" {
                    message = "Para iniciar Manual, la utilización debe ser ≤20 % y su sensor estar disponible."
                } else if refusal == "preflight_requires_AUTO" {
                    message = "El modo anterior todavía no se ha verificado como AUTO. Comprueba RM antes de reintentar."
                } else if refusal == "preflight_telemetry_not_fresh" {
                    message = "Telemetría sin actualizar: no se permite activar Manual."
                } else {
                    message = "Manual rechazado: " + refusal
                }
                busy = false; refresh();return
            }
            session = token
            applyStatus(r)
            if autoRequested { busy = false; restoreAuto(); return }
            rearmWatch?.cancel();rearmWatch = nil
            // The only physically validated spin-up is START 50. Automatically
            // walk down to the requested floor, not a direct 0 RPM -> 30 write.
            // Each step retains the original RM and tachometer verification.
            let reached = await transition(to: manualFloor, token: token)
            if !reached || autoRequested {
                if !autoRequested && lowMarginRefused && active && available && readbackConfirmed && session == token && FanSelectionPolicy.canKeepVerifiedStep(current: currentPercent, floor: manualFloor) {
                    targetPercent = currentPercent; busy = false
                    message = "Manual activo al \(currentPercent) %. El 30 % requiere más margen térmico/eléctrico. Puedes aumentar o intentar bajar cuando mejore."
                    heartbeat(token); return
                }
                if active && session == token {
                    message = "No se confirmó el mínimo manual; regresando a AUTO."
                    busy = false; restoreAuto(); return
                }
                busy = false; autoRequested = false; watchForRescueCompletion(); return
            }
            targetPercent = currentPercent
            busy = false
            message = "Manual activo al \(currentPercent) % · RM verificado."
            heartbeat(token)
        }
    }

    // A dedicated task keeps the C guard alive. Closing NVMetrics stops the
    // heartbeat; the guard then requests AUTO. A second root rescue is armed.
    private func heartbeat(_ token: String) {
        pulse?.cancel()
        pulse = Task { @MainActor in
            while !Task.isCancelled && session == token && !ending {
                await pause(900_000_000)
                // A RAISE is a synchronous RM SET + GET transaction in the root
                // service. Do not enqueue PINGs behind that operation.
                if busy { continue }
                if Task.isCancelled || session != token || ending { break }
                heartbeatInFlight = true
                let r = await queryIPC(["manual_heartbeat", token])
                guard session == token else { heartbeatInFlight = false; break }
                guard (r["ok"] as? Bool) == true else {
                    message = "Sesión terminada o sin comunicación; AUTO por guard/rescate. " +
                              (r["error"] as? String ?? "")
                    session = nil;active = false;readbackConfirmed = false
                    rmStatus = "Sin sesión · comprobar AUTO"
                    // The guard may have gone to AUTO on thermal / heartbeat trip.
                    // Show the real ending reason and never falsely imply verified AUTO.
                    let state = await queryIPC(["control_status"])
                    applyStatus(state)
                    watchForRescueCompletion()
                    heartbeatInFlight = false
                    break
                }
                applyStatus(r)
                heartbeatInFlight = false
            }
        }
    }

    private func waitForHeartbeat() async {
        // A completed PING cannot overwrite a more recent SET/GET result.
        while heartbeatInFlight { await pause(30_000_000) }
    }

    private func reconcile(_ reason: String, details: [String: Any]? = nil) async {
        let state = await queryIPC(["control_status"])
        applyStatus(state)
        if !active { watchForRescueCompletion() }
        let explanation = reason == "lower_recovered_verified_higher" ? "Reducción cancelada por carga; velocidad superior recuperada y verificada." : lowMarginRefused ? FanSelectionPolicy.lowerRefusal(details) : reason == "lower_requires_idle_and_two_spinning_fans" ?
            "No se verificaron las condiciones para bajar o ambos ventiladores." :
            "Ajuste interrumpido: " + reason
        message = explanation + (active && readbackConfirmed ? " Sigue aplicado \(currentPercent) %." : " Comprueba AUTO.")
    }

    private func transition(to requested: Int, token: String) async -> Bool {
        lowMarginRefused = false
        await waitForHeartbeat()
        guard !autoRequested && active && session == token && available && readbackConfirmed else { return false }
        let initial = await queryIPC(["manual_heartbeat", token])
        guard (initial["ok"] as? Bool) == true else {
            await reconcile(initial["error"] as? String ?? "Heartbeat perdido"); return false
        }
        applyStatus(initial)
        for _ in 0..<15 {
            guard !autoRequested && active && session == token && available && readbackConfirmed else { return false }
            if currentPercent == requested { return true }
            guard let step = FanSelectionPolicy.nextStep(current: currentPercent, target: requested, floor: manualFloor,
                                                        load: loadProfile, thermal: thermalProfile) else { return false }
            let lower = step < currentPercent
            message = "Aplicando \(requested) % · verificando \(step) %…"
            let reply = await queryIPC([lower ? "manual_lower" : "manual_raise", token, String(step)])
            guard (reply["ok"] as? Bool) == true else {
                let reason = reply["error"] as? String ?? "Sin respuesta IPC"
                lowMarginRefused = ["lower_thermal_margin_insufficient_no_write", "lower_recovered_verified_higher"].contains(reason)
                await reconcile(reason, details: reply["details"] as? [String: Any])
                return false
            }
            applyStatus(reply)
            guard active && session == token && readbackConfirmed && currentPercent == step else {
                await reconcile("RM no confirmó el escalón solicitado"); return false
            }
            if currentPercent == requested { return !autoRequested }
            if autoRequested { return false }
            // Lower already has three tach samples. Keep the heartbeat alive
            // between steps and leave a short stabilization interval.
            await pause(700_000_000)
            if autoRequested { return false }
            let keepalive = await queryIPC(["manual_heartbeat", token])
            guard (keepalive["ok"] as? Bool) == true else {
                await reconcile(keepalive["error"] as? String ?? "Heartbeat perdido"); return false
            }
            applyStatus(keepalive)
            guard active && session == token && readbackConfirmed && currentPercent == step else {
                await reconcile("El escalón verificado cambió durante la transición"); return false
            }
        }
        return false
    }

    func applySelection() {
        guard canApply, let token = session else { return }
        let requested = targetPercent
        busy = true; autoRequested = false
        Task {
            let reached = await transition(to: requested, token: token)
            busy = false
            if autoRequested { autoRequested = false; restoreAuto(); return }
            if reached { message = "Velocidad aplicada: \(currentPercent) % · RM confirmado." }
            // Keep the user's requested target visible and available for an
            // explicit retry; never silently replace it after a refused step.
        }
    }

    func restoreAuto() {
        guard !ending else { return }
        // The user can cancel even while START has not returned its session token.
        if busy { autoRequested = true; message = "AUTO solicitado · finalizando el escalón en curso…"; return }
        guard let token = session else { return }
        ending = true;busy = true
        autoRequested = false
        Task {
            await waitForHeartbeat()
            pulse?.cancel();pulse = nil
            guard active && session == token else {
                busy = false;ending = false;watchForRescueCompletion();return
            }
            let r = await queryIPC(["manual_auto",token])
            if (r["ok"] as? Bool) == true {
                session = nil;active = false;currentPercent = 0
                targetPercent = manualFloor;readbackConfirmed = false
                rmStatus = "AUTO verificado por RM"
                oneShotUsed = true  // guard against premature restart while root rescue is active
                message = "AUTO confirmado por RM. Esperando verificación independiente del rescate…"
                watchForRescueCompletion()
            } else {
                message = "AUTO sin confirmar: " + (r["error"] as? String ?? "comprobar RM")
                // AUTO is the user's final intent. Never renew a manual lease
                // after an unconfirmed AUTO: let the native guard/rescue take over.
                session = nil; active = false; currentPercent = 0
                readbackConfirmed = false; autoVerified = false; oneShotUsed = true
                rmStatus = "AUTO pendiente · esperando guard y rescate"
                watchForRescueCompletion()
            }
            busy = false;ending = false
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
                await pause(1_000_000_000)
                if Task.isCancelled || active || session != nil { break }
                let r = await queryIPC(["control_status"])
                if Task.isCancelled || active || session != nil { break }
                applyStatus(r)
                if available && physicalEnabled && !oneShotUsed && autoVerified {
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
            if !Task.isCancelled { rearmWatch = nil; refresh() }
        }
    }
}

