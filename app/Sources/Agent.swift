// Runs nvmetrics-agent (bundled in Contents/Resources) and turns its JSON lines into Samples. Reading needs no rights.
// If the agent stops (driver not loaded, sleep), it is started again after a pause that grows to 30 s.
// The agent runs as "watch <ms> --control": the app writes "boost off|auto|max" lines to its stdin. The boost request
// needs no password and lasts only while the agent runs (it lets go when NVMetrics quits or stops it). In this mode the
// agent also runs its heat protection, which drops the boost request while the card is too hot ("protect" in each line).
import Foundation
import Combine

/// When the agent asks the GPU for full clocks (NV2080_CTRL_CMD_PERF_BOOST). Raw values are the agent's words.
enum BoostMode: String, CaseIterable {
    case off, auto, max

    var label: String {
        switch self {
        case .off: return "Ahorro"
        case .auto: return "Automático"
        case .max: return "Máximo"
        }
    }

    var detail: String {
        switch self {
        case .off: return "Nunca pide relojes máximos."
        case .auto: return "Pide relojes máximos mientras la GPU trabaja (un juego, por ejemplo) y deja de pedirlos 15 s después de que se calme."
        case .max: return "Pide relojes máximos todo el rato mientras NVMetrics esté en marcha, aunque cierres la ventana."
        }
    }
}

@MainActor
final class Agent: ObservableObject {
    @Published private(set) var latest: Sample?
    @Published private(set) var history: [Sample] = []        // last 5 minutes, for the graph
    @Published private(set) var problem: String?               // shown when there are no readings
    /// what Alerts judges: the recent readings while there is a current one, nothing once the agent ended (ended()
    /// drops latest but keeps the graph's history), so no warning outlives the agent
    var alertReadings: [Sample] { latest == nil ? [] : history }
    @Published var intervalMs: Int = 1000 { didSet { restart() } }
    @Published var boostMode: BoostMode = Agent.savedBoostMode() {
        didSet {
            UserDefaults.standard.set(boostMode.rawValue, forKey: "nvmetrics.boostMode")
            sendBoost()
        }
    }

    private var process: Process?
    private var input: FileHandle?                              // the agent's stdin (write end)
    private var buffer = Data()
    private var retryDelay: TimeInterval = 2
    private var stopping = false
    private let decoder = JSONDecoder()

    nonisolated static var resources: URL { Bundle.main.resourceURL ?? URL(fileURLWithPath: ".") }
    nonisolated static var agentPath: String { resources.appendingPathComponent("nvmetrics-agent").path }

    nonisolated static func savedBoostMode() -> BoostMode {
        BoostMode(rawValue: UserDefaults.standard.string(forKey: "nvmetrics.boostMode") ?? "") ?? .auto
    }

    init() {
        // writing to the stdin of an agent that already died must fail with an error, not kill the app with SIGPIPE
        _ = signal(SIGPIPE, SIG_IGN)
    }

    func start() {
        stopping = false
        guard process == nil else { return }
        guard FileManager.default.isExecutableFile(atPath: Agent.agentPath) else {
            problem = "Falta nvmetrics-agent dentro de la app."
            return
        }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: Agent.agentPath)
        p.arguments = ["watch", String(intervalMs), "--control"]
        let out = Pipe(), err = Pipe(), inp = Pipe()
        p.standardOutput = out
        p.standardError = err
        p.standardInput = inp
        // the handlers run on Foundation's own threads; they hop to the main actor with a constant reference (the
        // Agent lives as long as the app)
        let me = self
        let id = ObjectIdentifier(p)
        out.fileHandleForReading.readabilityHandler = { h in
            let chunk = h.availableData
            if chunk.isEmpty { h.readabilityHandler = nil; return }
            Task { @MainActor in me.take(chunk) }
        }
        err.fileHandleForReading.readabilityHandler = { h in
            let chunk = h.availableData
            if chunk.isEmpty { h.readabilityHandler = nil; return }
            guard let text = String(data: chunk, encoding: .utf8) else { return }
            Task { @MainActor in me.problem = Agent.explain(text) }
        }
        p.terminationHandler = { _ in
            Task { @MainActor in me.ended(id) }
        }
        do {
            try p.run()
            process = p
            input = inp.fileHandleForWriting
            sendBoost()
        } catch {
            problem = "No se pudo iniciar nvmetrics-agent: \(error.localizedDescription)"
        }
    }

    func stop() {
        stopping = true
        process?.terminate()            // the agent lets go of the boost on SIGTERM
        process = nil
        closeInput()
    }

    private func restart() { stop(); start() }

    /// tells the running agent the boost mode; a no-op while none runs (start() sends it)
    private func sendBoost() {
        guard let input else { return }
        // throwing write: a dead reader gives EPIPE here (SIGPIPE is ignored), which needs no handling: the
        // termination handler restarts the agent and start() sends the mode again
        try? input.write(contentsOf: Data("boost \(boostMode.rawValue)\n".utf8))
    }

    private func closeInput() {
        try? input?.close()
        input = nil
    }

    /// a process ended; only the current one matters (an old one stopped by restart() ends after the new one started)
    private func ended(_ id: ObjectIdentifier) {
        guard let p = process, ObjectIdentifier(p) == id else { return }
        process = nil
        closeInput()
        latest = nil          // no agent, no current reading: no stale alerts or "⚠︎", and the driver notice can show
        buffer.removeAll()    // a half line from the dead agent must not prefix the next agent's first line
        guard !stopping else { return }
        let delay = retryDelay
        retryDelay = min(retryDelay * 2, 30)
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            if !self.stopping { self.start() }
        }
    }

    private func take(_ chunk: Data) {
        guard !chunk.isEmpty else { return }
        buffer.append(chunk)
        while let nl = buffer.firstIndex(of: 0x0a) {
            let line = buffer.subdata(in: buffer.startIndex..<nl)
            buffer.removeSubrange(buffer.startIndex...nl)
            guard let s = try? decoder.decode(Sample.self, from: line) else { continue }
            latest = s
            problem = nil
            retryDelay = 2
            history.append(s)
            let cutoff = s.t - 5 * 60 * 1000
            if let first = history.firstIndex(where: { $0.t >= cutoff }), first > 0 { history.removeFirst(first) }
        }
    }

    nonisolated static func explain(_ stderr: String) -> String {
        if stderr.contains("cannot open") { return "El driver NullMoth no está cargado: no hay lecturas." }
        if stderr.contains("no GPU") { return "El driver no ve ninguna gráfica." }
        if stderr.contains("shared-data") { return "El driver no entregó el bloque de métricas." }
        return stderr.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
