// The two switches NVMetrics offers, both through an administrator prompt (macOS asks for the password):
//  - reposo: NVRM_610_BOOST=0 in the user's block of nvrm610.conf (nvmetrics-setting.sh); applies as apps restart
//  - eco:    the GPU's rated-power limit, GLOBAL client (nvmetrics-agent eco on|off); applies at once
// Reading the state needs no password. Fan speed, power limit, clock offsets and voltage are not offered: the driver
// has no controls for them.
import Foundation
import Combine

private final class CapturedProcessOutput: @unchecked Sendable {
    private let lock = NSLock()
    private var values = [Data(), Data()]
    func put(_ data: Data, at index: Int) {
        lock.lock(); defer { lock.unlock() }
        values[index] = data
    }
    func get(_ index: Int) -> Data {
        lock.lock(); defer { lock.unlock() }
        return values[index]
    }
}

@MainActor
final class Controls: ObservableObject {
    @Published private(set) var reposo: Bool?
    @Published private(set) var eco: Bool?
    @Published private(set) var busy = false
    @Published var message: String?

    private var revision = 0
    private var refreshing = false
    private let execute: @Sendable (String, [String]) -> (Int32, String, String)
    init(execute: @escaping @Sendable (String, [String]) -> (Int32, String, String) = { Controls.runFull($0, $1) }) {
        self.execute = execute
    }

    nonisolated private var setting: String { Agent.resources.appendingPathComponent("nvmetrics-setting.sh").path }

    func refresh() {
        guard !busy && !refreshing else { return }
        refreshing = true
        let requestedRevision = revision
        Task.detached { [setting, execute] in
            let r = execute(setting, ["reposo", "status"])
            let e = execute(Agent.agentPath, ["eco", "status"])
            await MainActor.run {
                self.refreshing = false
                guard self.revision == requestedRevision && !self.busy else { return }
                self.reposo = r.0 == 0 ? Controls.onOff(Controls.field(r.1, "reposo")) : nil
                self.eco = e.0 == 0 ? Controls.onOff(Controls.field(e.1, "eco")) : nil
            }
        }
    }

    func setReposo(_ on: Bool) {
        admin([setting, "reposo", on ? "on" : "off"]) { out in
            self.reposo = Controls.onOff(Controls.field(out, "reposo"))
            guard self.reposo == on else {
                self.message = "El modo reposo solicitado no quedó confirmado."; return
            }
            self.message = on ? "Modo reposo guardado. Cierra sesión y vuelve a entrar para que todas las apps lo usen."
                              : "Modo reposo quitado. Se aplica al cerrar sesión y volver a entrar."
        }
    }

    func setEco(_ on: Bool) {
        admin([Agent.agentPath, "eco", on ? "on" : "off"]) { out in
            let state = Controls.field(out, "eco")
            self.eco = Controls.onOff(state)
            if self.eco != on { self.message = "La gráfica no confirmó el modo eco solicitado." }
            else { self.message = on ? "Modo eco activo: la gráfica no pasa de su potencia nominal." : "Modo eco desactivado." }
        }
    }

    private func admin(_ argv: [String], done: @escaping @MainActor @Sendable (String) -> Void) {
        guard !busy else { return }
        revision += 1
        busy = true
        Task.detached { [execute] in
            let cmd = argv.map(Controls.shellQuote).joined(separator: " ")
            let script = "do shell script \(Controls.appleScriptString(cmd)) with administrator privileges"
            // osascript, out of process: NSAppleScript is not safe off the main thread, and this waits on a password
            let (status, out, err) = execute("/usr/bin/osascript", ["-e", script])
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
        // Drain both pipes together. A full stderr pipe otherwise blocks the
        // child while this thread waits for stdout EOF (including fan IPC).
        let captured = CapturedProcessOutput(), group = DispatchGroup()
        for (index, pipe) in [out, err].enumerated() {
            group.enter()
            DispatchQueue.global().async {
                captured.put(pipe.fileHandleForReading.readDataToEndOfFile(), at: index)
                group.leave()
            }
        }
        p.waitUntilExit()
        group.wait()
        return (p.terminationStatus, String(data: captured.get(0), encoding: .utf8) ?? "", String(data: captured.get(1), encoding: .utf8) ?? "")
    }

    /// "on" -> true, "off" -> false, anything else (null, "other", nothing) -> unknown
    nonisolated static func onOff(_ s: String?) -> Bool? {
        if s == "on" { return true }
        if s == "off" { return false }
        return nil
    }

    /// a string field of the one-line JSON the helpers print, or nil
    nonisolated static func field(_ json: String, _ key: String) -> String? {
        guard let line = json.split(separator: "\n").last, let data = line.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return obj[key] as? String
    }
}
