import Foundation
import Combine

// Substitute only the resource locator. The actual UI model and Controls run.
enum Agent {
    static let resources = URL(fileURLWithPath: "/fake")
    static let agentPath = "/fake/nvmetrics-agent"
}

@MainActor final class FakeIPC {
    var calls: [[String]] = []
    var percent = 0
    var end = "never_started"
    var refuseLower = false
    var autoFails = false
    var blockBegin = false
    var beginWaiter: CheckedContinuation<Void, Never>?
    var blockedLower: Int?
    var lowerWaiter: CheckedContinuation<Void, Never>?
    let token = String(repeating: "a", count: 32)
    func state() -> [String: Any] {
        ["active": percent > 0, "percent": percent, "physical_enabled": true,
         "safety_profile": "thermal_supervised", "manual_floor": 30,
         "one_shot_used": percent > 0, "seconds_left": 200.0,
         "readback_verified": percent > 0, "last_end_reason": end]
    }
    func query(_ args: [String]) async -> [String: Any] {
        calls.append(args)
        switch args[0] {
        case "inspect": return ["ok": true, "data": ["mode": "auto"]]
        case "manual_begin":
            if blockBegin { await withCheckedContinuation { beginWaiter = $0 } }
            percent = 50
            var data = state(); data["session"] = token
            return ["ok": true, "data": data]
        case "manual_lower", "manual_raise":
            let target = Int(args[2])!
            if refuseLower && args[0] == "manual_lower" {
                return ["ok":false,"error":"lower_thermal_margin_insufficient_no_write","details":["sample":["power_w":50.1,"gpu_c":46.1,"mem_c":68.0,"util_gpu_pct":20]]]
            }
            if blockedLower == target { await withCheckedContinuation { lowerWaiter = $0 } }
            percent = target
        case "manual_auto":
            if autoFails { return ["ok": false, "error": "transport_timeout"] }
            percent = 0; end = "auto_independently_verified_ready"
        default: break
        }
        return ["ok": true, "data": state()]
    }
    var writes: [[String]] { calls.filter { ["manual_begin", "manual_raise", "manual_lower", "manual_auto"].contains($0[0]) } }
}

final class ControlTransport: @unchecked Sendable {
    let refreshEntered = DispatchSemaphore(value: 0)
    let releaseRefresh = DispatchSemaphore(value: 0)
    let lock = NSLock()
    var stale = true
    var calls: [[String]] = []
    func waitForRefresh() { refreshEntered.wait() }
    func run(_ path: String, _ args: [String]) -> (Int32, String, String) {
        lock.lock(); calls.append([path] + args); let block = stale && args == ["reposo", "status"]; lock.unlock()
        if block { refreshEntered.signal(); releaseRefresh.wait() }
        if path == "/usr/bin/osascript" {
            return (0, args.last!.contains("reposo") ? "{\"reposo\":\"on\"}" : "{\"eco\":\"on\"}", "")
        }
        return (0, args[0] == "reposo" ? "{\"reposo\":\"off\"}" : "{\"eco\":\"off\"}", "")
    }
}

// The driver's boost policy as the agent's "policy status" reports it; a test changes it between two refreshes.
final class PolicyAnswer: @unchecked Sendable {
    private let lock = NSLock()
    private var policy = "idle"
    func set(_ value: String) { lock.lock(); policy = value; lock.unlock() }
    func line() -> String { lock.lock(); defer { lock.unlock() }; return "{\"policy\":\"" + policy + "\"}\n" }
}

@main struct UIStateTests {
    @MainActor static func until(_ condition: () -> Bool) async {
        for _ in 0..<3000 {
            if condition() { return }
            try? await Task.sleep(nanoseconds: 1_000_000)
        }
        fatalError("UI test timed out")
    }
    @MainActor static func make(_ ipc: FakeIPC) async -> FanGuardUIModel {
        let m = FanGuardUIModel(query: ipc.query, pause: { ns in
            // Keep normal heartbeat/rescue sleeps; accelerate only step pacing.
            try? await Task.sleep(nanoseconds: ns == 700_000_000 ? 1_000_000 : ns)
        })
        m.refresh(); await until { m.canBegin }
        return m
    }
    @MainActor static func main() async {
        do {
            let ipc = FakeIPC()
            let model = await make(ipc)
            model.begin(); await until { model.active && !model.busy }
            precondition(model.currentPercent == 30)
            precondition(ipc.writes.map { $0[0] } == ["manual_begin"] + Array(repeating: "manual_lower", count: 4))
            let writes = ipc.writes.count
            model.selectPercent(100)
            precondition(model.currentPercent == 30 && model.targetPercent == 100 && ipc.writes.count == writes)
            model.applySelection(); await until { !model.busy }
            precondition(model.currentPercent == 100)
            model.selectPercent(30); model.applySelection(); await until { !model.busy }
            precondition(model.currentPercent == 30)
            precondition(ipc.writes.suffix(14).map { Int($0[2])! } == Array(stride(from: 95, through: 30, by: -5)))
            model.restoreAuto(); await until { !model.busy && !model.active }
        }
        do {
            let ipc = FakeIPC(); ipc.blockBegin = true
            let model = await make(ipc)
            model.begin(); await until { ipc.beginWaiter != nil }
            model.restoreAuto(); ipc.beginWaiter?.resume(); ipc.beginWaiter = nil
            await until { !model.busy }
            precondition(ipc.writes.map { $0[0] } == ["manual_begin", "manual_auto"])
            precondition(!model.active)
        }
        do {
            let ipc = FakeIPC()
            let m = await make(ipc)
            m.begin(); await until { m.active && !m.busy }
            m.selectPercent(100);m.applySelection();await until { !m.busy }
            ipc.blockedLower = 95
            m.selectPercent(30);m.applySelection();await until { ipc.lowerWaiter != nil }
            m.restoreAuto();ipc.lowerWaiter?.resume();ipc.lowerWaiter = nil
            await until { !m.busy && !m.active }
            precondition(ipc.writes.suffix(2).map { $0[0] } == ["manual_lower", "manual_auto"])
            precondition(ipc.writes.dropLast().last?[2] == "95")
        }
        do {
            let ipc = FakeIPC(); let m = await make(ipc)
            m.begin(); await until { m.active && !m.busy }
            ipc.autoFails = true
            m.restoreAuto(); await until { !m.busy }
            precondition(!m.active && !m.canApply && !m.canBegin && !m.autoVerified)
            let heartbeats = ipc.calls.filter { $0[0] == "manual_heartbeat" }.count
            try? await Task.sleep(nanoseconds: 1_100_000_000)
            precondition(ipc.calls.filter { $0[0] == "manual_heartbeat" }.count == heartbeats)
            // Simulate the native watchdog and independently verified rescue.
            ipc.percent = 0; ipc.end = "auto_independently_verified_ready"
            await until { m.canBegin }
        }
        do {
            let transport = ControlTransport()
            let controls = Controls(execute: { transport.run($0, $1) })
            controls.refresh()
            await Task.detached { transport.waitForRefresh() }.value
            controls.setReposo(true); await until { !controls.busy }
            precondition(controls.reposo == true && controls.eco == nil)
            transport.releaseRefresh.signal()
            try? await Task.sleep(nanoseconds: 50_000_000)
            precondition(controls.reposo == true) // old OFF response must be discarded
            controls.setEco(true); await until { !controls.busy }
            precondition(controls.eco == true && controls.reposo == true)
        }
        do {
            let controls = Controls(execute: { _, _ in (0, "{\"eco\":\"off\",\"reposo\":\"unknown\"}", "") })
            controls.setEco(true); await until { !controls.busy }
            precondition(controls.eco == false && controls.message?.contains("no confirmó") == true)
            controls.setReposo(true); await until { !controls.busy }
            precondition(controls.reposo == nil && controls.message?.contains("no quedó confirmado") == true)
        }
        do {
            let ipc = FakeIPC(); let model = await make(ipc)
            model.begin(); await until { model.active && !model.busy }
            model.selectPercent(50);model.applySelection();await until { !model.busy }
            ipc.refuseLower = true
            model.selectPercent(30);model.applySelection();await until { !model.busy }
            precondition(model.currentPercent == 50 && model.targetPercent == 30 && model.canApply)
            precondition(model.message?.contains("50.10 W") == true)
            model.restoreAuto();await until { !model.busy }
        }
        do {
            // Driver with the boost policy: one prompt writes the file and sets the policy, and "do shell script"
            // hands the two answers back joined by a carriage return.
            let controls = Controls(execute: { path, args in
                guard path == "/usr/bin/osascript" else { return (0, "{}", "") }
                return args.last!.contains("'policy' 'idle'")
                    ? (0, "{\"reposo\":\"on\"}\r{\"policy\":\"idle\"}\n", "")
                    : (0, "{\"reposo\":\"off\"}\r{\"policy\":\"normal\"}\n", "")
            })
            controls.setReposo(true); await until { !controls.busy }
            precondition(controls.reposo == true && controls.reposoHot == true)
            precondition(controls.message?.contains("sin cerrar sesión") == true)
            controls.setReposo(false); await until { !controls.busy }
            precondition(controls.reposo == false && controls.reposoHot == true)
            precondition(controls.message?.contains("quitado en todas las apps abiertas") == true)
        }
        do {
            // Driver without the policy (3.0): the second helper answers null, the file alone decides and the
            // message still says to log out and in.
            let controls = Controls(execute: { _, _ in (0, "{\"reposo\":\"on\"}\r{\"policy\":null,\"why\":\"driver\"}\n", "") })
            controls.setReposo(true); await until { !controls.busy }
            precondition(controls.reposo == true && controls.reposoHot == false)
            precondition(controls.message?.contains("Cierra sesión") == true)
        }
        do {
            // The file says off while the policy chosen earlier in this boot says idle: the switch shows what the
            // driver does and a notice says the two disagree. The notice comes down when they agree again.
            let answer = PolicyAnswer()
            let controls = Controls(execute: { _, args in
                if args == ["reposo", "status"] { return (0, "{\"reposo\":\"off\"}\n", "") }
                if args == ["policy", "status"] { return (0, answer.line(), "") }
                return (0, "{\"eco\":\"off\"}\n", "")
            })
            controls.refresh(); await until { controls.reposoHot != nil }
            precondition(controls.reposo == true && controls.message?.contains("no coinciden") == true)
            answer.set("normal")
            controls.refresh(); await until { controls.reposo == false }
            precondition(controls.reposoHot == true && controls.message == nil)
        }
        let captured = await Task.detached {
            Controls.runFull("/bin/sh", ["-c", "/usr/bin/head -c 200000 /dev/zero >&2; /usr/bin/printf done"])
        }.value
        precondition(captured.0 == 0 && captured.1 == "done" && captured.2.count == 200000)
        print("PASS: real UI model full 30–100 transitions, staged selection, AUTO priority, failed AUTO, independent Eco/Reposo, stale refresh, readback rejection and concurrent subprocess pipes")
    }
}
