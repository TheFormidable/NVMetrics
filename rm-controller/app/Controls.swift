// The two switches NVMetrics offers, both through an administrator prompt (macOS asks for the password):
//  - reposo: NVRM_610_BOOST=0 in the user's block of nvrm610.conf (nvmetrics-setting.sh), which every app reads when
//            it starts and which lasts across reboots, and in the same prompt the driver's boost policy
//            (nvmetrics-agent policy idle|normal, driver 3.0.1 on), which every running app follows the next time it
//            works (within a second of its work; one that sits idle with NVRM_610_BOOST=hold keeps its hold until
//            then); with an older driver only the file is written and it applies as apps restart. A tool that writes
//            only the file after a policy was chosen in this boot changes nothing until a reboot (reposoMismatch)
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
    /// whether the loaded driver switches idle mode while apps run (nil: not known yet)
    @Published private(set) var reposoHot: Bool?
    @Published private(set) var eco: Bool?
    @Published private(set) var busy = false
    @Published var message: String?
    /// the notice refresh() last put in `message` (reposoMismatch), so that it takes down its own notice and no other
    private var mismatchNote: String?

    private var revision = 0
    private var refreshing = false
    private var refreshedAt: TimeInterval = -100
    private let execute: @Sendable (String, [String]) -> (Int32, String, String)
    init(execute: @escaping @Sendable (String, [String]) -> (Int32, String, String) = { Controls.runFull($0, $1) }) {
        self.execute = execute
    }

    nonisolated private var setting: String { Agent.resources.appendingPathComponent("nvmetrics-setting.sh").path }
    nonisolated private static let privilegedRoot = "/Library/PrivilegedHelperTools/org.nvmetrics.rmguard/"

    func refresh() {
        guard !busy && !refreshing &&
              ProcessInfo.processInfo.systemUptime - refreshedAt >= 2 else { return }
        refreshing = true
        let requestedRevision = revision
        Task.detached { [setting, execute] in
            let r = execute(setting, ["reposo", "status"])
            let e = execute(Agent.agentPath, ["eco", "status"])
            let p = execute(Agent.agentPath, ["policy", "status"])
            await MainActor.run {
                self.refreshing = false
                guard self.revision == requestedRevision && !self.busy else { return }
                self.refreshedAt = ProcessInfo.processInfo.systemUptime
                let conf = r.0 == 0 ? Controls.field(r.1, "reposo") : nil
                let policy = p.0 == 0 ? Controls.field(p.1, "policy") : nil
                self.reposo = Controls.reposoState(conf: conf, policy: policy)
                self.reposoHot = policy != nil
                self.eco = e.0 == 0 ? Controls.onOff(Controls.field(e.1, "eco")) : nil
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
        // an older driver has no policy, and then the file alone still applies as apps restart
        admin([Self.privilegedRoot + "nvmetrics-setting.sh", "reposo", on ? "on" : "off"],
              followedBy: [Self.privilegedRoot + "nvmetrics-agent", "policy", on ? "idle" : "normal"]) { out in
            let conf = Controls.fieldAny(out, "reposo")
            let policy = Controls.fieldAny(out, "policy")
            if policy == nil, let why = Controls.fieldAny(out, "why"), why == "refused" || why == "failed" {
                // a driver that has the policy kept the one it had: the file changed alone, so read both again and
                // show what the driver follows (the notice says so when they now disagree)
                self.message = "El archivo de ajustes quedó guardado, pero el driver no aceptó el cambio de política."
                self.refresh()
                return
            }
            self.reposo = Controls.reposoState(conf: conf, policy: policy)
            self.mismatchNote = Controls.reposoMismatch(conf: conf, policy: policy)
            let hot = policy == (on ? "idle" : "normal")
            self.reposoHot = hot
            guard self.reposo == on else {
                self.message = "El modo reposo solicitado no quedó confirmado."; return
            }
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
        return onOff(conf)
    }

    /// The file and the driver's policy disagree: the file changed after a policy was chosen in this boot and the policy
    /// did not (an NVMetrics whose agent has no "policy", an edit by hand, a set the driver refused), or the policy was
    /// set by hand. The driver follows the policy until a reboot, whatever the file says, so the switch here shows the
    /// policy and this says why. Nil when they agree or either is unknown.
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
        admin([Self.privilegedRoot + "nvmetrics-agent", "eco", on ? "on" : "off"]) { out in
            let state = Controls.field(out, "eco")
            self.eco = Controls.onOff(state)
            if self.eco != on { self.message = "La gráfica no confirmó el modo eco solicitado." }
            else { self.message = on ? "Modo eco activo: la gráfica no pasa de su potencia nominal." : "Modo eco desactivado." }
        }
    }

    /// argv as root; `followedBy`, when given, runs after it only if it succeeded, and its own failure does not fail
    /// the call
    private func admin(_ argv: [String], followedBy second: [String]? = nil,
                       done: @escaping @MainActor @Sendable (String) -> Void) {
        guard !busy else { return }
        revision += 1
        refreshedAt = -100
        busy = true
        Task.detached { [execute] in
            let clean = ["/usr/bin/env", "-i", "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "HOME=/var/root", "LANG=C"]
            var cmd = (clean + argv).map(Controls.shellQuote).joined(separator: " ")
            if let more = second { cmd += " && { " + (clean + more).map(Controls.shellQuote).joined(separator: " ") + " || true; }" }
            let script = "do shell script \(Controls.appleScriptString(cmd)) with administrator privileges"
            // osascript, out of process: NSAppleScript is not safe off the main thread, and this waits on a password
            let (status, out, err) = execute("/usr/bin/osascript", ["-e", script])
            await MainActor.run {
                self.busy = false
                if status == 0 { done(out) }
                // the user cancelled the password prompt: nothing changed, so only a mismatch notice stays up
                else if err.contains("-128") { self.message = self.mismatchNote }
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

    nonisolated static func runFull(_ path: String, _ args: [String], stdin: Data? = nil) -> (Int32, String, String) {
        let p = Process(), out = Pipe(), err = Pipe()
        let input = stdin.map { _ in Pipe() }
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = args
        p.standardOutput = out
        p.standardError = err
        if let input { p.standardInput = input }
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
        if let input, let stdin {
            try? input.fileHandleForWriting.write(contentsOf: stdin)
            try? input.fileHandleForWriting.close()
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
