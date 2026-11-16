// The two switches NVMetrics offers, both through an administrator prompt (macOS asks for the password):
//  - reposo: NVRM_610_BOOST=0 in the user's block of nvrm610.conf (nvmetrics-setting.sh), which every app reads when
//            it starts and which lasts across reboots, and in the same prompt the driver's boost policy
//            (nvmetrics-agent policy idle|normal, driver 3.0.1 on), which every running app follows within a second;
//            with an older driver only the file is written and it applies as apps restart. A tool that writes only
//            the file after a policy was chosen in this boot changes nothing until a reboot (reposoMismatch says so)
//  - eco:    the GPU's rated-power limit, GLOBAL client (nvmetrics-agent eco on|off); applies at once
// Reading the state needs no password. Fan speed, power limit, clock offsets and voltage are not offered: the driver
// has no controls for them.
import Foundation

@MainActor
final class Controls: ObservableObject {
    @Published private(set) var reposo: Bool?
    /// whether the loaded driver switches idle mode while apps run (nil: not known yet)
    @Published private(set) var reposoHot: Bool?
    @Published private(set) var eco: Bool?
    @Published private(set) var busy = false
    @Published var message: String?
    /// the notice refresh() last put in `message` (reposoMismatch), so that it takes down its own notice and no other
    private var mismatchNote: String?

    nonisolated private var setting: String { Agent.resources.appendingPathComponent("nvmetrics-setting.sh").path }

    func refresh() {
        Task.detached { [setting] in
            let r = Controls.run(setting, ["reposo", "status"])
            let e = Controls.run(Agent.agentPath, ["eco", "status"])
            let p = Controls.run(Agent.agentPath, ["policy", "status"])
            await MainActor.run {
                let policy = Controls.field(p, "policy")
                let conf = Controls.field(r, "reposo")
                self.reposo = Controls.reposoState(conf: conf, policy: policy)
                self.reposoHot = policy != nil
                self.eco = Controls.onOff(Controls.field(e, "eco"))
                // the notice goes up while they disagree and comes down, if it is still the one showing, when they agree
                let note = Controls.reposoMismatch(conf: conf, policy: policy)
                if let n = note { self.message = n }
                else if self.message != nil && self.message == self.mismatchNote { self.message = nil }
                self.mismatchNote = note
            }
        }
    }

    func setReposo(_ on: Bool) {
        // one prompt: the file first (it decides after a reboot), then the driver's policy for the apps already open;
        // an older driver refuses the second and the file alone still applies as apps restart
        admin([setting, "reposo", on ? "on" : "off"], then: [Agent.agentPath, "policy", on ? "idle" : "normal"]) { out in
            let policy = Controls.fieldAny(out, "policy")
            self.reposo = Controls.reposoState(conf: Controls.fieldAny(out, "reposo"), policy: policy)
            let hot = policy == (on ? "idle" : "normal")
            self.reposoHot = hot
            if hot {
                self.message = on ? "Modo reposo activo en todas las apps abiertas, sin cerrar sesión."
                                  : "Modo reposo quitado en todas las apps abiertas."
            } else {
                self.message = on ? "Modo reposo guardado. Cierra sesión y vuelve a entrar para que todas las apps lo usen."
                                  : "Modo reposo quitado. Se aplica al cerrar sesión y volver a entrar."
            }
        }
    }

    /// what the switch shows: the driver's policy when one was chosen since boot, else the file
    nonisolated static func reposoState(conf: String?, policy: String?) -> Bool? {
        if policy == "idle" { return true }
        if policy == "normal" { return false }
        return conf.map { $0 == "on" }
    }

    /// The file and the driver's policy disagree: the file changed after a policy was chosen in this boot and the policy
    /// did not (an NVMetrics whose agent has no "policy", the 0.14.7 fan candidate among them; an edit by hand; a set
    /// the driver refused), or the policy was set by hand. The driver follows the policy until a reboot, whatever the
    /// file says, so the switch here shows the policy and this says why. Nil when they agree or either is unknown.
    nonisolated static func reposoMismatch(conf: String?, policy: String?) -> String? {
        let what: String
        if policy == "idle" && conf == "off" {
            what = "El driver sigue en modo reposo hasta reiniciar aunque el archivo de ajustes ya no lo pide"
        } else if policy == "normal" && conf == "on" {
            what = "El driver tiene el modo reposo quitado hasta reiniciar aunque el archivo de ajustes lo pide"
        } else {
            return nil
        }
        return what + ": el archivo y la política del driver no coinciden (los cambió por separado otra versión de "
            + "NVMetrics, un cambio a mano o un cambio que el driver rechazó). Cambia el interruptor aquí para que "
            + "vuelvan a coincidir."
    }

    func setEco(_ on: Bool) {
        admin([Agent.agentPath, "eco", on ? "on" : "off"]) { out in
            let state = Controls.field(out, "eco")
            self.eco = Controls.onOff(state)
            if state == nil { self.message = "La gráfica no aceptó el modo eco." }
            else { self.message = on ? "Modo eco activo: la gráfica no pasa de su potencia nominal." : "Modo eco desactivado." }
        }
    }

    /// argv as root; `then`, when given, runs after it only if it succeeded, and its own failure does not fail the call
    private func admin(_ argv: [String], then: [String]? = nil, done: @escaping @MainActor @Sendable (String) -> Void) {
        busy = true
        Task.detached {
            var cmd = argv.map(Controls.shellQuote).joined(separator: " ")
            if let then { cmd += " && { " + then.map(Controls.shellQuote).joined(separator: " ") + " || true; }" }
            let script = "do shell script \(Controls.appleScriptString(cmd)) with administrator privileges"
            // osascript, out of process: NSAppleScript is not safe off the main thread, and this waits on a password
            let (status, out, err) = Controls.runFull("/usr/bin/osascript", ["-e", script])
            await MainActor.run {
                self.busy = false
                if status == 0 { done(out) }
                else if err.contains("-128") { self.message = nil }   // the user cancelled the password prompt
                else { self.message = "No se pudo aplicar: \(err.trimmingCharacters(in: .whitespacesAndNewlines))" }
            }
        }
    }

    // single-quote for /bin/sh: 'it'\''s'
    nonisolated static func shellQuote(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }
    // an AppleScript string literal
    nonisolated static func appleScriptString(_ s: String) -> String {
        "\"" + s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
    }

    nonisolated static func run(_ path: String, _ args: [String]) -> String { runFull(path, args).1 }

    nonisolated static func runFull(_ path: String, _ args: [String]) -> (Int32, String, String) {
        let p = Process(), out = Pipe(), err = Pipe()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = args
        p.standardOutput = out
        p.standardError = err
        guard (try? p.run()) != nil else { return (-1, "", "no se pudo ejecutar \(path)") }
        let o = out.fileHandleForReading.readDataToEndOfFile()
        let e = err.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return (p.terminationStatus, String(data: o, encoding: .utf8) ?? "", String(data: e, encoding: .utf8) ?? "")
    }

    /// "on" -> true, "off" -> false, anything else (null, "other", nothing) -> unknown
    nonisolated static func onOff(_ s: String?) -> Bool? {
        if s == "on" { return true }
        if s == "off" { return false }
        return nil
    }

    /// The lines of a helper's output. Any line ending separates them: "do shell script" hands the lines of what it ran
    /// back joined by carriage returns, not line feeds, so two helpers in one prompt arrive as {...}\r{...}.
    nonisolated static func lines(_ out: String) -> [Substring] {
        out.split(whereSeparator: { $0.isNewline })
    }

    /// a string field of the one-line JSON the helpers print, or nil
    nonisolated static func field(_ json: String, _ key: String) -> String? {
        guard let line = lines(json).last else { return nil }
        return lineField(line, key)
    }

    /// the same field from whichever line of several helpers' output has it (the last such line), or nil
    nonisolated static func fieldAny(_ json: String, _ key: String) -> String? {
        for line in lines(json).reversed() {
            if let v = lineField(line, key) { return v }
        }
        return nil
    }

    nonisolated private static func lineField(_ line: Substring, _ key: String) -> String? {
        guard let data = line.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return obj[key] as? String
    }
}
