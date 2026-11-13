// The two switches NVMetrics offers, both through an administrator prompt (macOS asks for the password):
//  - reposo: NVRM_610_BOOST=0 in the user's block of nvrm610.conf (nvmetrics-setting.sh); applies as apps restart
//  - eco:    the GPU's rated-power limit, GLOBAL client (nvmetrics-agent eco on|off); applies at once
// Reading the state needs no password. Fan speed, power limit, clock offsets and voltage are not offered: the driver
// has no controls for them.
import Foundation

@MainActor
final class Controls: ObservableObject {
    @Published private(set) var reposo: Bool?
    @Published private(set) var eco: Bool?
    @Published private(set) var busy = false
    @Published var message: String?

    private var setting: String { Agent.resources.appendingPathComponent("nvmetrics-setting.sh").path }

    func refresh() {
        Task.detached { [setting] in
            let r = Controls.run(setting, ["reposo", "status"])
            let e = Controls.run(Agent.agentPath, ["eco", "status"])
            await MainActor.run {
                self.reposo = Controls.field(r, "reposo").map { $0 == "on" }
                self.eco = Controls.field(e, "eco").flatMap { $0 == "on" ? true : $0 == "off" ? false : nil }
            }
        }
    }

    func setReposo(_ on: Bool) {
        admin([setting, "reposo", on ? "on" : "off"]) { out in
            self.reposo = Controls.field(out, "reposo").map { $0 == "on" }
            self.message = on ? "Modo reposo guardado. Cierra sesión y vuelve a entrar para que todas las apps lo usen."
                              : "Modo reposo quitado. Se aplica al cerrar sesión y volver a entrar."
        }
    }

    func setEco(_ on: Bool) {
        admin([Agent.agentPath, "eco", on ? "on" : "off"]) { out in
            let state = Controls.field(out, "eco")
            self.eco = state.flatMap { $0 == "on" ? true : $0 == "off" ? false : nil }
            if state == nil { self.message = "La gráfica no aceptó el modo eco." }
            else { self.message = on ? "Modo eco activo: la gráfica no pasa de su potencia nominal." : "Modo eco desactivado." }
        }
    }

    private func admin(_ argv: [String], done: @escaping @MainActor (String) -> Void) {
        busy = true
        Task.detached {
            let cmd = argv.map(Controls.shellQuote).joined(separator: " ")
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

    /// a string field of the one-line JSON the helpers print, or nil
    nonisolated static func field(_ json: String, _ key: String) -> String? {
        guard let line = json.split(separator: "\n").last, let data = line.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return obj[key] as? String
    }
}
