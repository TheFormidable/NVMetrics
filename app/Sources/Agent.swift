// Runs nvmetrics-agent (bundled in Contents/Resources) and turns its JSON lines into Samples. Reading needs no rights.
// If the agent stops (driver not loaded, sleep), it is started again after a pause that grows to 30 s.
import Foundation
import Combine

@MainActor
final class Agent: ObservableObject {
    @Published private(set) var latest: Sample?
    @Published private(set) var history: [Sample] = []        // last 5 minutes, for the graph
    @Published private(set) var problem: String?               // shown when there are no readings
    @Published var intervalMs: Int = 1000 { didSet { restart() } }

    private var process: Process?
    private var buffer = Data()
    private var retryDelay: TimeInterval = 2
    private var stopping = false
    private let decoder = JSONDecoder()

    static var resources: URL { Bundle.main.resourceURL ?? URL(fileURLWithPath: ".") }
    static var agentPath: String { resources.appendingPathComponent("nvmetrics-agent").path }

    func start() {
        stopping = false
        guard process == nil else { return }
        guard FileManager.default.isExecutableFile(atPath: Agent.agentPath) else {
            problem = "Falta nvmetrics-agent dentro de la app."
            return
        }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: Agent.agentPath)
        p.arguments = ["watch", String(intervalMs)]
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let chunk = h.availableData
            Task { @MainActor in self?.take(chunk) }
        }
        err.fileHandleForReading.readabilityHandler = { [weak self] h in
            let chunk = h.availableData
            guard !chunk.isEmpty, let text = String(data: chunk, encoding: .utf8) else { return }
            Task { @MainActor in self?.problem = Agent.explain(text) }
        }
        p.terminationHandler = { [weak self] _ in
            Task { @MainActor in self?.ended() }
        }
        do { try p.run(); process = p } catch { problem = "No se pudo iniciar nvmetrics-agent: \(error.localizedDescription)" }
    }

    func stop() {
        stopping = true
        process?.terminate()
        process = nil
    }

    private func restart() { stop(); start() }

    private func ended() {
        process = nil
        guard !stopping else { return }
        let delay = retryDelay
        retryDelay = min(retryDelay * 2, 30)
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in self?.start() }
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

    static func explain(_ stderr: String) -> String {
        if stderr.contains("cannot open") { return "El driver NullMoth no está cargado: no hay lecturas." }
        if stderr.contains("no GPU") { return "El driver no ve ninguna gráfica." }
        if stderr.contains("shared-data") { return "El driver no entregó el bloque de métricas." }
        return stderr.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}
