import SwiftUI
import AppKit
import UniformTypeIdentifiers

private struct FrameReport: Decodable {
    let schema: Int
    let source: String
    let frames: Int
    let duration_s: Double
    let fps: Double
    let one_percent_low: Double
    let p95_ms: Double
    let p99_ms: Double
    let median_ms: Double
}

struct GamingView: View {
    @EnvironmentObject private var agent: Agent
    @EnvironmentObject private var fanGuard: FanGuardUIModel
    @State private var capture = GamingCapture()
    @State private var recording = false
    @State private var frameReport: FrameReport?
    @State private var message: String?
    @State private var importing = false
    @State private var resources: [ResourceReading] = []
    @State private var measuring = false
    @State private var measured = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 15) {
                Text("Gaming Lab").font(.title2.bold())
                Text("Telemetría compartida, sesiones de energía y análisis real de fotogramas.")
                    .foregroundStyle(.secondary)
                GroupBox("Perfiles de monitor y boost") {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            profile("Escritorio", .auto, 1000)
                            profile("Gaming", .auto, 500)
                            profile("Ahorro", .off, 2000)
                            profile("Rendimiento", .max, 500)
                        }
                        Toggle("Cadencia adaptativa (0,5–2 s según carga)", isOn: $agent.adaptiveCadence)
                        Text("La cadencia aumenta con carga sostenida y se reduce después de 15 s de reposo.")
                            .font(.caption).foregroundStyle(.secondary)
                        Text("Gaming usa boost automático según carga. Rendimiento pide boost mientras NVMetrics está activo. No modifica Eco, Reposo, voltaje, ventiladores ni límites de potencia.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                GroupBox("Sesión de GPU") {
                    VStack(alignment: .leading, spacing: 8) {
                        LabeledContent("GPU / VRAM", value: "\(Fmt.num(agent.latest?.temp_gpu_c, 1)) / \(Fmt.num(agent.latest?.temp_mem_c, 1)) °C")
                        LabeledContent("Potencia", value: "\(Fmt.num(agent.latest?.power_w, 1)) W")
                        LabeledContent("Contador global de flips/s", value: Fmt.num(agent.displayFlipRate, 1))
                        Text("El contador NVAccel es global, no mide FPS de un juego. Si no existe, aparece --.")
                            .font(.caption).foregroundStyle(.secondary)
                        Divider()
                        LabeledContent("Muestras / duración", value: "\(capture.points.count) / \(Fmt.num(capture.duration, 1)) s")
                        LabeledContent("Energía estimada / cobertura", value: "\(Fmt.num(capture.energyWh, 3)) Wh / \(Fmt.num(capture.coveredSeconds, 1)) s")
                        LabeledContent("Potencia media cubierta", value: "\(Fmt.num(capture.averageWatts, 1)) W")
                        LabeledContent("Picos GPU / VRAM", value: "\(Fmt.num(capture.peakGPU, 1)) / \(Fmt.num(capture.peakVRAM, 1)) °C")
                        HStack {
                            Button(recording ? "Terminar sesión" : "Iniciar sesión") {
                                if recording { recording = false }
                                else { capture = GamingCapture(); recording = true; message = nil }
                            }.disabled(!recording && agent.latest == nil)
                            Button("Exportar CSV") { export() }.disabled(capture.points.isEmpty || recording)
                        }
                    }
                }
                GroupBox("FPS y frametime") {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Importa una captura real con una columna frametime_ms (milisegundos por fotograma). No se calculan 1% lows a partir de FPS globales.")
                            .font(.caption).foregroundStyle(.secondary)
                        Button("Importar captura CSV…") { importFrames() }.disabled(importing)
                        if importing { ProgressView() }
                        if let r = frameReport {
                            LabeledContent("FPS medio", value: Fmt.num(r.fps, 1))
                            LabeledContent("1% low", value: Fmt.num(r.one_percent_low, 1))
                            LabeledContent("Frametime mediano / P95 / P99", value: "\(Fmt.num(r.median_ms, 2)) / \(Fmt.num(r.p95_ms, 2)) / \(Fmt.num(r.p99_ms, 2)) ms")
                            LabeledContent("Fotogramas / duración", value: "\(r.frames) / \(Fmt.num(r.duration_s, 1)) s")
                        } else { Text("Sin captura de fotogramas.").foregroundStyle(.secondary) }
                    }
                }
                GroupBox("Recursos de NVMetrics") {
                    VStack(alignment: .leading, spacing: 8) {
                        Button("Medir procesos ahora") { measureResources() }.disabled(measuring)
                        Text("Instantánea de ps: CPU y RSS por proceso; no suma memoria compartida ni mide FPS.")
                            .font(.caption).foregroundStyle(.secondary)
                        if measured && resources.isEmpty { Text("No se identificaron procesos por ruta instalada.") }
                        ForEach(resources) { p in
                            LabeledContent("\(p.name) · PID \(p.id)", value: "\(Fmt.num(p.cpu, 1))% CPU · \(Fmt.num(p.rssMiB, 1)) MiB")
                        }
                    }
                }
                if let message { Text(message).font(.caption).foregroundStyle(.secondary) }
            }.padding(18)
        }
        .frame(minWidth: 510, minHeight: 460)
        .onReceive(agent.$latest) { sample in
            guard recording else { return }
            if let sample {
                if !capture.append(sample) { recording = false; message = "Sesión detenida por límite o discontinuidad; exporta el CSV." }
            } else { capture.breakContinuity() }
        }
        .onDisappear { recording = false }
    }

    private func profile(_ title: String, _ boost: BoostMode, _ interval: Int) -> some View {
        Button(title) {
            agent.adaptiveCadence = false
            agent.intervalMs = interval
            agent.boostMode = boost
        }
        .disabled(fanGuard.active || fanGuard.busy)
        .tint(agent.boostMode == boost && agent.intervalMs == interval ? Color.green : Color.accentColor)
    }

    private func export() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "NVMetrics-gaming-session.csv"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do { try capture.csv().write(to: url, atomically: true, encoding: .utf8) }
        catch { message = error.localizedDescription }
    }

    private func measureResources() {
        measuring = true
        Task { @MainActor in
            let r = await Task.detached(priority: .utility) {
                Controls.runFull("/bin/ps", ["-axo", "pid,%cpu,rss,comm"])
            }.value
            resources = r.0 == 0 ? ResourceReading.parse(r.1) : []
            measured = true; measuring = false
        }
    }

    private func importFrames() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.commaSeparatedText]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        importing = true; message = nil; frameReport = nil
        let path = Agent.resources.appendingPathComponent("GuardTools/Runtime/nvmetrics-runtime").path
        let input = url.path
        Task { @MainActor in
            let r = await Task.detached(priority: .utility) { Controls.runFull(path, ["benchmark", input]) }.value
            if r.0 == 0, let report = try? JSONDecoder().decode(FrameReport.self, from: Data(r.1.utf8)),
               report.schema == 1 && report.source == "imported_per_frame_ms" {
                frameReport = report
            } else { message = "No se pudo analizar la captura: " + r.2 }
            importing = false
        }
    }
}
