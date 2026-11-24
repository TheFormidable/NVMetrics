import SwiftUI
import AppKit

// Separate utility window. The existing dashboard, theme and controls are untouched.
private struct DoctorReport: Decodable {
    struct Check: Decodable, Identifiable {
        let name: String
        let status: String
        let detail: String
        var id: String { name }
    }
    let schema: Int
    let macos: String
    let checks: [Check]
}

struct DoctorView: View {
    @EnvironmentObject private var agent: Agent
    @State private var report: DoctorReport?
    @State private var busy = false
    @State private var error: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("NVMetrics Doctor").font(.title2.bold())
            Text("Diagnóstico de solo lectura. No modifica ventiladores, potencia ni perfiles.")
                .foregroundStyle(.secondary)
            if let report {
                Text("macOS \(report.macos)").font(.caption)
                List(report.checks) { check in
                    VStack(alignment: .leading, spacing: 4) {
                        Label(check.name, systemImage: symbol(check.status)).font(.headline)
                        Text(check.detail).font(.caption).foregroundStyle(.secondary)
                    }
                }
            } else if let error {
                Text(error).foregroundStyle(.secondary)
            }
            if busy { ProgressView("Comprobando…") }
            HStack {
                Button("Comprobar") { run() }.disabled(busy)
                Button("Copiar informe") {
                    guard let report else { return }
                    let lines = report.checks.map { "[\($0.status)] \($0.name): \($0.detail)" }
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString((["NVMetrics Doctor – macOS \(report.macos)"] + lines).joined(separator: "\n"), forType: .string)
                }.disabled(report == nil || busy)
            }
        }
        .padding(20).frame(minWidth: 440, minHeight: 390)
        .onAppear { if report == nil { run() } }
    }

    private func symbol(_ status: String) -> String {
        switch status {
        case "ok": return "checkmark.circle"
        case "warn", "error": return "exclamationmark.triangle"
        default: return "questionmark.circle"
        }
    }

    private func run() {
        guard !busy else { return }
        busy = true; error = nil; report = nil
        let path = Agent.resources.appendingPathComponent("GuardTools/Runtime/nvmetrics-runtime").path
        let snapshot = agent.latestRaw
        Task { @MainActor in
            let result = await Task.detached(priority: .utility) {
                Controls.runFull(path, snapshot == nil ? ["doctor"] : ["doctor", "--snapshot"], stdin: snapshot)
            }.value
            if result.0 == 0, let decoded = try? JSONDecoder().decode(DoctorReport.self, from: Data(result.1.utf8)), decoded.schema == 1 {
                report = decoded
            } else { error = "No se pudo completar el diagnóstico. Comprueba la instalación de NVMetrics." }
            busy = false
        }
    }
}
