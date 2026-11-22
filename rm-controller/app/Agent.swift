// Runs nvmetrics-agent (bundled in Contents/Resources) and turns its JSON lines into Samples. Reading needs no rights.
// If the agent stops (driver not loaded, sleep), it is started again after a pause that grows to 30 s.
// The agent runs as "watch <ms> --control": the app writes "boost off|auto|max" lines to its stdin. The boost request
// needs no password and lasts only while the agent runs (it lets go when NVMetrics quits or stops it). In this mode the
// agent also runs its heat protection, which drops the boost request while the card is too hot ("protect" in each line).
import Foundation
import Combine
import AppKit

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
    // Maximum non-zero RPM index observed, possibly in an earlier launch.
    // Not the board's authoritative physical fan count.
    @Published private(set) var previouslyObservedFanCount: Int =
        min(8, max(0, UserDefaults.standard.integer(forKey: "nvmetrics.fan.observedSlots")))
    @Published private(set) var history: [Sample] = []        // last 5 minutes, for the graph
    @Published private(set) var problem: String?               // shown when there are no readings
    /// what Alerts judges: the recent readings while there is a current one, nothing once the agent ended (ended()
    /// drops latest but keeps the graph's history), so no warning outlives the agent
    var alertReadings: [Sample] {
        guard latest != nil, let start = sessionStartedAt else { return [] }
        return history.filter { $0.t >= start }
    }
    @Published var intervalMs: Int = 500 { didSet { if oldValue != intervalMs { restart() } } }
    @Published var boostMode: BoostMode = Agent.savedBoostMode() {
        didSet {
            UserDefaults.standard.set(boostMode.rawValue, forKey: "nvmetrics.boostMode")
            sendBoost()
        }
    }

    private var process: Process?
    private var generation: UUID?
    private var input: FileHandle?                              // the agent's stdin (write end)
    private var buffer = Data()
    private var discardingFrame = false
    private var retryDelay: TimeInterval = 2
    private var stopping = false
    private var lastSampleAt: TimeInterval = 0
    private var sessionStartedAt: Double?
    private var wakeObserver: AnyCancellable?
    private var watchdog: Task<Void, Never>?
    private var retry: Task<Void, Never>?
    private var restartRequested = false
    private let decoder = JSONDecoder()

    nonisolated static var resources: URL { Bundle.main.resourceURL ?? URL(fileURLWithPath: ".") }
    nonisolated static var agentPath: String { resources.appendingPathComponent("nvmetrics-agent").path }

    nonisolated static func savedBoostMode() -> BoostMode {
        BoostMode(rawValue: UserDefaults.standard.string(forKey: "nvmetrics.boostMode") ?? "") ?? .auto
    }

    init() {
        // writing to the stdin of an agent that already died must fail with an error, not kill the app with SIGPIPE
        _ = signal(SIGPIPE, SIG_IGN)
        wakeObserver = NSWorkspace.shared.notificationCenter.publisher(for: NSWorkspace.didWakeNotification)
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in
                // Reset only the monitor/boost client, never the Manual guard or its lease.
                self?.restart()
            }
    }

    func start() {
        guard process == nil else { return }
        stopping = false
        restartRequested = false
        retry?.cancel(); retry = nil
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
        let id = UUID()
        out.fileHandleForReading.readabilityHandler = { h in
            let chunk = h.availableData
            if chunk.isEmpty { h.readabilityHandler = nil; return }
            DispatchQueue.main.async { me.take(chunk, from: id) }
        }
        err.fileHandleForReading.readabilityHandler = { h in
            let chunk = h.availableData
            if chunk.isEmpty { h.readabilityHandler = nil; return }
            guard let text = String(data: chunk, encoding: .utf8) else { return }
            DispatchQueue.main.async {
                guard me.generation == id, !me.stopping else { return }
                me.problem = Agent.explain(text)
            }
        }
        p.terminationHandler = { _ in
            Task { @MainActor in me.ended(id) }
        }
        do {
            try p.run()
            process = p
            generation = id
            input = inp.fileHandleForWriting
            lastSampleAt = ProcessInfo.processInfo.systemUptime
            watchdog?.cancel()
            watchdog = Task { @MainActor [weak self] in
                while !Task.isCancelled {
                    do { try await Task.sleep(nanoseconds: 1_000_000_000) } catch { return }
                    guard let self, self.generation == id, !self.stopping else { return }
                    if ProcessInfo.processInfo.systemUptime - self.lastSampleAt > max(8, Double(self.intervalMs) / 1000 * 4) {
                        self.problem = "Telemetría interrumpida; reconectando con el driver."
                        self.restart()
                        return
                    }
                }
            }
            sendBoost()
        } catch {
            out.fileHandleForReading.readabilityHandler = nil
            err.fileHandleForReading.readabilityHandler = nil
            problem = "No se pudo iniciar nvmetrics-agent: \(error.localizedDescription)"
            scheduleRetry()
        }
    }

    func stop() {
        restartRequested = false
        retry?.cancel(); retry = nil
        watchdog?.cancel(); watchdog = nil
        stopping = true
        latest = nil
        sessionStartedAt = nil
        buffer.removeAll()
        discardingFrame = false
        closeInput()
        if let process, process.isRunning { process.terminate() }
    }

    private func restart() {
        stop()
        // Wait for the old boost-owning RM client to exit before opening a new one.
        if process != nil { restartRequested = true } else { start() }
    }

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

    /// A process ended; only callbacks carrying the current generation matter.
    private func ended(_ id: UUID) {
        guard let p = process, generation == id else { return }
        (p.standardOutput as? Pipe)?.fileHandleForReading.readabilityHandler = nil
        (p.standardError as? Pipe)?.fileHandleForReading.readabilityHandler = nil
        process = nil
        generation = nil
        watchdog?.cancel(); watchdog = nil
        closeInput()
        latest = nil          // no agent, no current reading: no stale alerts or "⚠︎", and the driver notice can show
        sessionStartedAt = nil
        buffer.removeAll()    // a half line from the dead agent must not prefix the next agent's first line
        discardingFrame = false
        if restartRequested { start(); return }
        guard !stopping else { return }
        scheduleRetry()
    }

    private func scheduleRetry() {
        retry?.cancel()
        let delay = retryDelay
        retryDelay = min(retryDelay * 2, 30)
        retry = Task { @MainActor in
            do { try await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000)) } catch { return }
            if !self.stopping { self.start() }
        }
    }

    private func take(_ chunk: Data, from id: UUID) {
        guard !chunk.isEmpty, generation == id, !stopping else { return }
        buffer.append(chunk)
        while let nl = buffer.firstIndex(of: 0x0a) {
            if discardingFrame { buffer.removeSubrange(buffer.startIndex...nl); discardingFrame = false; continue }
            if nl - buffer.startIndex > 65_536 { buffer.removeSubrange(buffer.startIndex...nl); continue }
            let line = buffer.subdata(in: buffer.startIndex..<nl)
            buffer.removeSubrange(buffer.startIndex...nl)
            guard let s = try? decoder.decode(Sample.self, from: line), s.t.isFinite,
                  s.t > 0, latest.map({ s.t > $0.t }) ?? true else { continue }
            lastSampleAt = ProcessInfo.processInfo.systemUptime
            if sessionStartedAt == nil {
                // Wall-clock correction/sleep can create a new time origin.
                if let last = history.last, last.t >= s.t { history.removeAll() }
                sessionStartedAt = s.t
            }
            // Remember observed coolers even when RUSD later reports [] (all stopped).
            let observed = s.fan_live?.fan_count_observed ??
                s.fan_rpm?.lastIndex(where: { $0 > 0 }).map { $0 + 1 }
            if let observed, observed > previouslyObservedFanCount && observed <= 8 {
                previouslyObservedFanCount = observed
                UserDefaults.standard.set(observed, forKey: "nvmetrics.fan.observedSlots")
            }
            latest = s
            problem = nil
            retryDelay = 2
            history.append(s)
            let cutoff = s.t - 5 * 60 * 1000
            if let first = history.firstIndex(where: { $0.t >= cutoff }), first > 0 { history.removeFirst(first) }
            if history.count > 1_200 { history.removeFirst(history.count - 1_200) }
        }
        if buffer.count > 65_536 { buffer.removeAll(); discardingFrame = true; problem = "Trama de telemetría demasiado grande." }
    }

    nonisolated static func explain(_ stderr: String) -> String {
        if stderr.contains("cannot open") { return "El driver NullMoth no está cargado: no hay lecturas." }
        if stderr.contains("no GPU") { return "El driver no ve ninguna gráfica." }
        if stderr.contains("shared-data") { return "El driver no entregó el bloque de métricas." }
        return stderr.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
