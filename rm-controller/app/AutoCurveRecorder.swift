import Foundation
import SwiftUI
import AppKit
import Charts

// Observations are not firmware setpoints, and never feed the manual guard.
struct AutoCurvePoint: Codable, Identifiable, Sendable {
    var id: UUID = UUID()
    let observed_at: Double
    let gpu_c: Double
    let mem_c: Double
    let power_w: Double
    let util_gpu_pct: Int
    let rpm: [Int]
    let pstate: Int?
    let clock_gpu_mhz: Int?
    let auto_bracketed: Bool
    var reposoConfigured: Bool? = nil
    var ecoReported: Bool? = nil
    var boostSelected: String? = nil
    var profile: Int? {
        guard let r = reposoConfigured, let e = ecoReported else { return nil }
        return (r ? 2 : 0) + (e ? 1 : 0)
    }
    // The native agent serializes [] for a valid all-stopped reading.
    var fans: [Int] { rpm.isEmpty ? [0, 0] : rpm }
}

@MainActor
final class AutoCurveRecorder: ObservableObject {
    @Published private(set) var points: [AutoCurvePoint] = []
    @Published private(set) var status = "Esperando primera lectura AUTO"
    @Published var enabled = true {
        didSet { UserDefaults.standard.set(enabled, forKey: "nvmetrics.autoObservation.enabled") }
    }
    private var loop: Task<Void, Never>?
    private static let capacity = 3000
    private let url = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("NVMetrics", isDirectory: true).appendingPathComponent("auto-observations.json")

    init() {
        if UserDefaults.standard.object(forKey: "nvmetrics.autoObservation.enabled") != nil {
            enabled = UserDefaults.standard.bool(forKey: "nvmetrics.autoObservation.enabled")
        }
        if let data = try? Data(contentsOf: url), let old = try? Self.decoder().decode([AutoCurvePoint].self, from: data) {
            points = Array(old.suffix(Self.capacity))
        }
    }

    nonisolated private static func decoder() -> JSONDecoder { JSONDecoder() }

    nonisolated private static func settings() -> (Bool?, Bool?) {
        let path = Agent.resources.appendingPathComponent("nvmetrics-setting.sh").path
        let r = Controls.runFull(path, ["reposo", "status"])
        let e = Controls.runFull(Agent.agentPath, ["eco", "status"])
        // Reposo as the driver applies it now: its boost policy when one was chosen since boot (driver 3.0.1 on),
        // else the saved file, exactly as the switch shows it (Controls.reposoState).
        let p = Controls.runFull(Agent.agentPath, ["policy", "status"])
        let conf = r.0 == 0 ? Controls.field(r.1, "reposo") : nil
        let policy = p.0 == 0 ? Controls.field(p.1, "policy") : nil
        return (Controls.reposoState(conf: conf, policy: policy),
                e.0 == 0 ? Controls.onOff(Controls.field(e.1, "eco")) : nil)
    }

    nonisolated private static func collect() -> (AutoCurvePoint?, String) {
        let first = settings()
        let runtime = Agent.resources.appendingPathComponent("GuardTools/Runtime/nvmetrics-runtime").path
        let (code, output, _) = Controls.runFull(runtime, ["client", "auto_observe"])
        // The decoded observation has no id on the wire; use a separate wire type.
        guard let line = output.split(separator: "\n").last, let bytes = line.data(using: .utf8) else { return (nil, "Sin respuesta del servicio AUTO") }
        guard let reply = try? JSONDecoder().decode(WireResponse.self, from: bytes) else { return (nil, "Lectura AUTO inválida") }
        guard code == 0, reply.ok, let data = reply.data, data.auto_bracketed else {
            return (nil, reply.error ?? "AUTO no confirmado")
        }
        let last = settings()
        guard first.0 == last.0, first.1 == last.1 else { return (nil, "Configuración cambió durante la lectura") }
        return (AutoCurvePoint(observed_at: data.observed_at, gpu_c: data.gpu_c, mem_c: data.mem_c,
            power_w: data.power_w, util_gpu_pct: data.util_gpu_pct, rpm: data.rpm, pstate: data.pstate,
            clock_gpu_mhz: data.clock_gpu_mhz, auto_bracketed: true, reposoConfigured: first.0, ecoReported: first.1), "AUTO observado")
    }

    private struct WireResponse: Decodable { let ok: Bool; let data: WirePoint?; let error: String? }
    private struct WirePoint: Decodable {
        let observed_at: Double, gpu_c: Double, mem_c: Double, power_w: Double
        let util_gpu_pct: Int, rpm: [Int], pstate: Int?, clock_gpu_mhz: Int?, auto_bracketed: Bool
    }

    func start(agent: Agent, controls: Controls, fanGuard: FanGuardUIModel) {
        guard loop == nil else { return }
        loop = Task { [weak self] in
            while !Task.isCancelled {
                if let self = self {
                    if !self.enabled { self.status = "Registro pausado" }
                    else if fanGuard.active || fanGuard.busy || controls.busy {
                        self.status = "Pausado durante control manual o cambio de modo"
                    } else {
                        let boost = agent.boostMode.rawValue
                        let (result, reason) = await Task.detached(priority: .utility) { Self.collect() }.value
                        if self.enabled && !fanGuard.active && !fanGuard.busy && !controls.busy && agent.boostMode.rawValue == boost {
                            self.status = reason
                            if var point = result {
                                point.boostSelected = boost
                                self.points.append(point)
                                self.points = Array(self.points.suffix(Self.capacity))
                                self.persist()
                            }
                        }
                    }
                } else { return }
                do { try await Task.sleep(nanoseconds: 30_000_000_000) } catch { return }
            }
        }
    }

    private func persist() {
        do {
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try JSONEncoder().encode(points).write(to: url, options: .atomic)
        } catch { status = "No se pudo guardar el historial: \(error.localizedDescription)" }
    }

    func exportCSV() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "NVMetrics-AUTO-observations.csv"
        guard panel.runModal() == .OK, let dest = panel.url else { return }
        let date = ISO8601DateFormatter()
        func flag(_ b: Bool?) -> String { b.map { $0 ? "on" : "off" } ?? "unknown" }
        var lines = ["utc,auto_bracketed,reposo_configured,eco_reported,boost_selected,gpu_c,mem_c,power_w,util_gpu_pct,fan1_rpm,fan2_rpm,pstate,clock_gpu_mhz"]
        for p in points {
            guard p.fans.count == 2 else { continue }
            let fields: [String] = [date.string(from: Date(timeIntervalSince1970: p.observed_at)), "true", flag(p.reposoConfigured), flag(p.ecoReported), p.boostSelected ?? "unknown", String(p.gpu_c), String(p.mem_c), String(p.power_w), String(p.util_gpu_pct), String(p.fans[0]), String(p.fans[1]), p.pstate.map { String($0) } ?? "", p.clock_gpu_mhz.map { String($0) } ?? ""]
            lines.append(fields.joined(separator: ","))
        }
        do { try (lines.joined(separator: "\n") + "\n").write(to: dest, atomically: true, encoding: .utf8) }
        catch { status = "Error de exportación: \(error.localizedDescription)" }
    }
}

struct FirmwareCurveView: View {
    @EnvironmentObject private var recorder: AutoCurveRecorder
    @State private var profile = -1
    private let labels = ["Reposo OFF · Eco OFF", "Reposo OFF · Eco ON", "Reposo ON · Eco OFF", "Reposo ON · Eco ON"]
    private var filtered: [AutoCurvePoint] { recorder.points.filter { profile == -1 || ($0.profile ?? 4) == profile } }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Comportamiento AUTO del firmware").font(.title2.bold())
            Text("Observaciones reales, no una curva programable. No modifica los ventiladores ni ejecuta cargas.").font(.subheadline).foregroundStyle(.secondary)
            Toggle("Registrar automáticamente (aprox. cada 30 s)", isOn: $recorder.enabled)
            Text(recorder.status).font(.caption).foregroundStyle(.secondary)
            Picker("Configuración", selection: $profile) {
                Text("Todas").tag(-1)
                ForEach(0..<labels.count, id: \.self) { i in Text(labels[i]).tag(i) }
                Text("Estado desconocido").tag(4)
            }
            let data = filtered
            Text("\(data.count) muestras · \(data.filter { $0.fans == [0, 0] }.count) con ambos ventiladores a 0 RPM").font(.caption)
            if profile == -1 {
                ForEach(0..<labels.count, id: \.self) { i in
                    let samples = recorder.points.filter { $0.profile == i }
                    HStack {
                        Text(labels[i]); Spacer()
                        Text("\(samples.count) muestras · \(samples.filter { $0.fans == [0, 0] }.count) a 0 RPM").monospacedDigit()
                    }.font(.caption)
                }
            }
            if data.isEmpty {
                Text("Aún no hay mediciones para esta configuración.").frame(maxWidth: .infinity, minHeight: 180).foregroundStyle(.secondary)
            } else {
                Chart {
                    ForEach(data) { point in
                        ForEach(Array(point.fans.enumerated()), id: \.offset) { item in
                            PointMark(x: .value("GPU °C", point.gpu_c), y: .value("RPM", item.element))
                                .foregroundStyle(by: .value("Ventilador", "Fan \(item.offset + 1)"))
                                .symbolSize(15).opacity(0.65)
                        }
                    }
                }.chartYAxisLabel("RPM").chartXAxisLabel("GPU °C").frame(height: 240)
                if let p = data.last {
                    Text(String(format: "Última: %.1f °C GPU · %.1f °C VRAM · %.1f W · %d %% uso", p.gpu_c, p.mem_c, p.power_w, p.util_gpu_pct)).font(.caption)
                    Text("Estado: \(p.pstate.map { "P\($0)" } ?? "—") · reloj: \(p.clock_gpu_mhz.map { "\($0) MHz" } ?? "—") · boost elegido: \(p.boostSelected ?? "—")").font(.caption)
                }
            }
            Text("Reposo refleja lo que el driver aplica: con el driver 3.0.1 o posterior el cambio llega al momento; con uno anterior es la configuración guardada, y hay que cerrar sesión y volver a entrar tras cambiarla. En el CSV, reposo_configured es este mismo estado; las muestras anteriores a la 0.14.8 guardan la configuración del archivo. P-state y relojes indican el comportamiento observado; la histéresis, VRAM, carga y potencia pueden producir RPM distintas a igual temperatura.").font(.caption).foregroundStyle(.secondary)
            HStack { Text("Historial local: \(recorder.points.count)/3000").font(.caption); Spacer(); Button("Exportar CSV…") { recorder.exportCSV() }.disabled(recorder.points.isEmpty) }
        }.padding(24).frame(width: 620)
    }
}
