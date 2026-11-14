import SwiftUI
import AppKit

struct MainView: View {
    @EnvironmentObject var agent: Agent
    @EnvironmentObject var controls: Controls
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        let th = Theme(scheme: scheme)
        let s = agent.latest
        let alerts = Alerts.list(agent.alertReadings, reposo: controls.reposo)
        VStack(spacing: 16) {
            header(th)
            if !alerts.isEmpty { alertBlock(alerts, th) }
            if let problem = agent.problem, s == nil {
                Text(problem).font(.system(size: 13)).foregroundStyle(Theme.warn)
                    .frame(maxWidth: .infinity).padding(10)
                    .background(th.panel).clipShape(RoundedRectangle(cornerRadius: 8))
            }
            meters(s, th)
            HStack(alignment: .top, spacing: 12) {
                Card(icon: "thermometer.medium", title: "Temperatura") {
                    InfoLine(name: "Núcleo", value: "\(Fmt.num(s?.temp_gpu_c, 1)) °C")
                    InfoLine(name: "Memoria", value: "\(Fmt.num(s?.temp_mem_c, 1)) °C")
                    InfoLine(name: "Placa", value: "\(Fmt.num(s?.temp_board_c, 1)) °C")
                    HistoryGraph(samples: agent.history).frame(height: 64).padding(.top, 4)
                    Text("Verde: temperatura · Gris: consumo (5 min)").font(.system(size: 10)).foregroundStyle(th.label)
                }
                Card(icon: "bolt.fill", title: "Energía", highlighted: true) {
                    InfoLine(name: "Consumo", value: "\(Fmt.num(s?.power_w, 1)) W")
                    InfoLine(name: "Promedio", value: "\(Fmt.num(s?.power_avg_w, 1)) W")
                    InfoLine(name: "Límite", value: "\(Fmt.num(s?.power_limit_w, 0)) W")
                    InfoLine(name: "Estado", value: s?.pstate.map { "P\($0)" } ?? "--")
                    InfoLine(name: "Uso GPU", value: "\(Fmt.int(s?.util_gpu_pct)) %")
                    InfoLine(name: "Vídeo (cod./decod.)", value: "\(Fmt.int(s?.util_enc_pct)) / \(Fmt.int(s?.util_dec_pct)) %")
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Frenando por").font(.system(size: 11, weight: .medium)).foregroundStyle(th.label)
                        Text(s?.throttleText ?? "--").font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(s?.isThrottlingHard == true ? Theme.hot : th.text)
                    }
                }
                FanCard()
            }
            .frame(height: 300)
            FanCurveCard().fixedSize(horizontal: false, vertical: true)
            modes(th)
            HStack(alignment: .top, spacing: 12) {
                BoostCard().frame(maxWidth: .infinity)
                HealthCard(health: s?.health, hasReadings: s != nil).frame(maxWidth: .infinity)
            }
            .fixedSize(horizontal: false, vertical: true)
            if let m = controls.message {
                Text(m).font(.system(size: 12)).foregroundStyle(th.text).frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(.horizontal, 22).padding(.top, 34).padding(.bottom, 20)
        .frame(width: 760)
        .background(th.background)
        .onAppear { controls.refresh() }
    }

    private func header(_ th: Theme) -> some View {
        HStack(spacing: 10) {
            Image(systemName: "gauge.with.needle.fill").font(.system(size: 26, weight: .bold)).foregroundStyle(Theme.green)
                .shadow(color: th.glow, radius: 8)
            VStack(alignment: .leading, spacing: 0) {
                (Text("NV").foregroundStyle(Theme.green) + Text("Metrics").foregroundStyle(th.text))
                    .font(.system(size: 24, weight: .heavy, design: .rounded)).italic()
                Text("MONITOR DE GRÁFICA").font(.system(size: 9, weight: .semibold)).kerning(2).foregroundStyle(th.label)
            }
            Spacer()
            Picker("", selection: $agent.intervalMs) {
                Text("0,5 s").tag(500); Text("1 s").tag(1000); Text("2 s").tag(2000)
            }
            .pickerStyle(.segmented).frame(width: 150)
            .help("Cada cuánto se lee la tarjeta")
        }
    }

    /// the alerts of Alerts.swift, most severe first; only called when there is at least one, so without alerts the
    /// window keeps its height
    private func alertBlock(_ alerts: [GPUAlert], _ th: Theme) -> some View {
        let top = alerts[0].level
        return VStack(alignment: .leading, spacing: 6) {
            ForEach(alerts, id: \.text) { a in
                HStack(alignment: .top, spacing: 8) {
                    Image(systemName: MainView.alertSymbol(a.level)).font(.system(size: 13, weight: .semibold)).frame(width: 18)
                    Text(a.text).font(.system(size: 12, weight: a.level == .info ? .medium : .semibold))
                        .fixedSize(horizontal: false, vertical: true)
                    Spacer(minLength: 0)
                }
                .foregroundStyle(MainView.alertColor(a.level, th))
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(th.panel)
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8)
            .stroke(top == .info ? th.panelEdge : MainView.alertColor(top, th).opacity(0.7), lineWidth: 1))
    }

    /// danger red, warning amber, info in the normal text colour
    private static func alertColor(_ level: GPUAlert.Level, _ th: Theme) -> Color {
        switch level {
        case .danger: return Theme.hot
        case .warning: return Theme.warn
        case .info: return th.text
        }
    }

    private static func alertSymbol(_ level: GPUAlert.Level) -> String {
        switch level {
        case .danger: return "exclamationmark.triangle.fill"
        case .warning: return "thermometer.high"
        case .info: return "shield.lefthalf.filled"
        }
    }

    private func meters(_ s: Sample?, _ th: Theme) -> some View {
        let maxGpu = Double(max(agent.history.compactMap { $0.clock_gpu_mhz }.max() ?? 0, 3000))
        let maxMem = Double(max(agent.history.compactMap { $0.clock_mem_mhz }.max() ?? 0, 1))
        return VStack(spacing: 4) {
            MeterRow(icon: "cpu", name: "GPU", value: Fmt.int(s?.clock_gpu_mhz), unit: "MHz",
                     fraction: s?.clock_gpu_mhz.map { Double($0) / maxGpu })
            MeterRow(icon: "memorychip", name: "MEM", value: Fmt.int(s?.clock_mem_mhz), unit: "MHz",
                     fraction: s?.clock_mem_mhz.map { Double($0) / maxMem })
            MeterRow(icon: "bolt.fill", name: "POTENCIA", value: Fmt.num(s?.power_w, 0), unit: "W",
                     fraction: s?.power_w.flatMap { w in s?.power_limit_w.map { w / $0 } })
            MeterRow(icon: "thermometer.medium", name: "TEMP", value: Fmt.num(s?.temp_gpu_c, 0), unit: "°C",
                     fraction: s?.temp_gpu_c.map { $0 / 100 }, color: Theme.tempColor(s?.temp_gpu_c))
        }
        .padding(12)
        .background(th.panel.opacity(th.dark ? 0.6 : 1))
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(th.panelEdge, lineWidth: 1))
    }

    private func modes(_ th: Theme) -> some View {
        HStack(alignment: .top, spacing: 12) {
            modeCard(th, title: "Modo reposo", icon: "moon.fill", value: controls.reposo,
                     detail: "Deja que la gráfica baje a su estado de bajo consumo cuando no haces nada. Se aplica al cerrar sesión y volver a entrar.") {
                controls.setReposo($0)
            }
            modeCard(th, title: "Modo eco", icon: "leaf.fill", value: controls.eco,
                     detail: "Limita la gráfica a su potencia nominal, sin turbo: menos calor con carga, algo menos de rendimiento. Se aplica al momento.") {
                controls.setEco($0)
            }
        }
    }

    private func modeCard(_ th: Theme, title: String, icon: String, value: Bool?, detail: String, set: @escaping (Bool) -> Void) -> some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: icon).font(.system(size: 18)).foregroundStyle(value == true ? Theme.green : th.label).frame(width: 22)
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.system(size: 14, weight: .bold)).foregroundStyle(th.text)
                Text(value == nil ? "Estado desconocido. \(detail)" : detail).font(.system(size: 11)).foregroundStyle(th.label)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer()
            Toggle("", isOn: Binding(get: { value ?? false }, set: set))
                .toggleStyle(.switch).tint(Theme.green).labelsHidden()
                .disabled(controls.busy)
        }
        .padding(12)
        .background(th.panel)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(value == true ? Theme.green.opacity(0.7) : th.panelEdge, lineWidth: 1))
    }

    private func vram(_ s: Sample?) -> String {
        guard let used = s?.vram_used_mib, let total = s?.vram_total_mib else { return "--" }
        return String(format: "%.1f / %.1f GB", Double(used) / 1024, Double(total) / 1024)
    }
}

struct MenuContent: View {
    @EnvironmentObject var agent: Agent
    @EnvironmentObject var controls: Controls
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        let s = agent.latest
        if let first = Alerts.list(agent.alertReadings, reposo: controls.reposo).first { Text(first.menuText) }
        Text("Temperatura: \(Fmt.num(s?.temp_gpu_c, 0)) °C")
        Text("Consumo: \(Fmt.num(s?.power_w, 0)) W · \(s?.pstate.map { "P\($0)" } ?? "--")")
        Text("Reloj: \(Fmt.int(s?.clock_gpu_mhz)) MHz · Uso: \(Fmt.int(s?.util_gpu_pct)) %")
        Text("Ventilador: \(fanText(s?.fan_rpm))")
        if let p = agent.problem, s == nil { Text(p) }
        Divider()
        // a Picker in a menu-style MenuBarExtra is a submenu with a checkmark on the current mode
        Picker("Rendimiento de la GPU", selection: $agent.boostMode) {
            ForEach(BoostMode.allCases, id: \.self) { m in Text(m.label).tag(m) }
        }
        Button("Abrir NVMetrics") { openWindow(id: "main"); NSApp.activate(ignoringOtherApps: true) }
            .keyboardShortcut("o")
        Button("Salir") { NSApp.terminate(nil) }.keyboardShortcut("q")
    }

    /// nil: no reading; empty: every fan stopped (the shared block lists coolers only up to the last that turns)
    private func fanText(_ rpm: [Int]?) -> String {
        guard let rpm else { return "--" }
        guard let top = rpm.max(), top > 0 else { return "parados" }
        return "\(top) RPM"
    }
}
