import SwiftUI
import AppKit

// Carbon dashboard. The existing agent and guarded controls remain the only source of truth.
struct MainView: View {
    @EnvironmentObject var agent: Agent
    @EnvironmentObject var controls: Controls
    @EnvironmentObject var fanGuard: FanGuardUIModel
    @Environment(\.openWindow) private var openWindow
    @AppStorage("nvmetrics.dashboard.skin") private var skin = "dark"
    @State private var page = "Resumen"
    private let pages = ["Resumen", "Ventiladores", "Diagnóstico", "Ajustes"]
    private var th: Theme { Theme(scheme: skin == "light" ? .light : .dark) }
    private var s: Sample? { agent.latest }

    var body: some View {
        VStack(spacing: 10) {
            header
            navigation
            ScrollView {
                VStack(spacing: 10) {
                    warnings
                    switch page {
                    case "Ventiladores":
                        fanPanel
                        FanGuardCard().fixedSize(horizontal: false, vertical: true)
                        FanCurveCard().fixedSize(horizontal: false, vertical: true)
                    case "Diagnóstico":
                        HealthCard(health: s?.health, hasReadings: s != nil)
                        details
                    case "Ajustes":
                        BoostCard()
                        details
                        Picker("Intervalo de lectura", selection: $agent.intervalMs) {
                            Text("0,5 s").tag(500); Text("1 s").tag(1000); Text("2 s").tag(2000)
                        }.pickerStyle(.segmented)
                    default:
                        afterburnerConsole
                        FanGuardCard().fixedSize(horizontal: false, vertical: true)
                    }
                }.frame(maxWidth: .infinity, alignment: .topLeading).padding(.bottom, 2)
            }
            quickControls
            HStack(spacing: 6) {
                Image(systemName: "shield.lefthalf.filled").foregroundStyle(fanGuard.autoVerified ? Theme.green : Theme.warn)
                Text(fanGuard.rmStatus).lineLimit(2)
                Spacer(minLength: 4)
                if fanGuard.active || fanGuard.busy {
                    Button("Restaurar AUTO") { fanGuard.restoreAuto() }.buttonStyle(.borderedProminent).tint(Theme.greenDim)
                }
                Text("\(Double(agent.intervalMs) / 1000, specifier: "%g") s").monospacedDigit()
            }.font(.system(size: 10)).foregroundStyle(th.label)
        }
        .padding(14)
        .frame(minWidth: 490, minHeight: 380)
        .background {
            if skin == "glass" {
                Rectangle().fill(.regularMaterial).overlay(LinearGradient(colors: [.green.opacity(0.13), .clear, .cyan.opacity(0.12)], startPoint: .topLeading, endPoint: .bottomTrailing))
            } else if skin == "graphite" {
                LinearGradient(colors: [Color(white: 0.24), Color(white: 0.12)], startPoint: .topLeading, endPoint: .bottomTrailing)
            } else { th.background }
        }
        .environment(\.colorScheme, skin == "light" ? .light : .dark)
        .preferredColorScheme(skin == "light" ? .light : .dark)
        .onAppear { controls.refresh() }
        .task {
            while !Task.isCancelled {
                fanGuard.refresh()
                do { try await Task.sleep(nanoseconds: 5_000_000_000) } catch { break }
            }
        }
    }

    private var header: some View {
        HStack(spacing: 10) {
            if let icon = NSImage(named: "AppIcon") {
                Image(nsImage: icon).resizable().scaledToFit().frame(width: 40, height: 40)
            }
            Text("NVMetrics").font(.system(size: 25, weight: .heavy)).italic().foregroundStyle(th.text)
            Divider().frame(height: 30)
            VStack(alignment: .leading, spacing: 3) {
                Text("NVIDIA GPU").font(.system(size: 13, weight: .semibold))
                HStack(spacing: 5) {
                    Circle().fill(s == nil ? Theme.warn : Theme.green).frame(width: 7, height: 7)
                    Text(s == nil ? "Sin lecturas" : "Telemetría disponible").font(.system(size: 10))
                }.foregroundStyle(th.label)
            }
            Spacer(minLength: 4)
            Picker("Estilo", selection: $skin) {
                Text("Oscuro").tag("dark"); Text("Claro").tag("light")
                Text("Grafito").tag("graphite"); Text("Cristal").tag("glass")
            }.labelsHidden().frame(width: 92)
        }
    }

    private var navigation: some View {
        HStack(spacing: 4) {
            ForEach(pages, id: \.self) { name in
                Button { page = name } label: {
                    Text(name).font(.system(size: 12, weight: .semibold))
                        .frame(maxWidth: .infinity).padding(.vertical, 8)
                        .foregroundStyle(page == name ? (th.dark ? Color.white : Color.black) : th.label)
                        .background(page == name ? Theme.greenDim.opacity(th.dark ? 0.42 : 0.25) : .clear, in: RoundedRectangle(cornerRadius: 7))
                }.buttonStyle(.plain)
            }
        }.padding(4).carbonPanel(th)
    }

    // Afterburner-style instrument panel: real telemetry and guarded controls only.
    // The reference Swift file's simulated GPUBackend is deliberately NOT used.
    private var afterburnerConsole: some View {
        VStack(spacing: 10) {
            HStack(spacing: 8) {
                Label("GPU MONITOR", systemImage: "gauge.with.needle")
                    .font(.system(size: 11, weight: .bold)).tracking(1.2)
                Spacer()
                Text("LECTURA DIRECTA · \(Double(agent.intervalMs) / 1000, specifier: "%g") s")
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(th.label)
            }
            .foregroundStyle(th.text)
            HStack(alignment: .top, spacing: 8) {
                afterburnerColumn(
                    "RELOJ GPU", value: Fmt.int(s?.clock_gpu_mhz), unit: "MHz",
                    caption: "Memoria \(Fmt.int(s?.clock_mem_mhz)) MHz",
                    symbol: "cpu"
                )
                afterburnerCenter
                afterburnerColumn(
                    "CONSUMO", value: Fmt.num(s?.power_w, 1), unit: "W",
                    caption: "Límite \(Fmt.num(s?.power_limit_w, 0)) W",
                    symbol: "bolt.fill"
                )
            }
            telemetry
            fanPanel
        }
    }

    private func afterburnerColumn(_ title: String, value: String, unit: String,
                                   caption: String, symbol: String) -> some View {
        VStack(spacing: 7) {
            Label(title, systemImage: symbol)
                .font(.system(size: 10, weight: .bold))
                .foregroundStyle(th.label)
                .lineLimit(1)
                .minimumScaleFactor(0.75)
            Text(value)
                .font(.system(size: 28, weight: .heavy, design: .rounded))
                .monospacedDigit()
                .lineLimit(1)
                .minimumScaleFactor(0.65)
                .foregroundStyle(th.text)
            Text(unit)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(Theme.green)
            Text(caption)
                .font(.system(size: 10))
                .lineLimit(1)
                .minimumScaleFactor(0.7)
                .foregroundStyle(th.label)
        }
        .padding(12)
        .frame(maxWidth: .infinity, minHeight: 150)
        .background(th.track.opacity(0.45), in: RoundedRectangle(cornerRadius: 10))
    }

    private var afterburnerCenter: some View {
        VStack(spacing: 4) {
            Text("GEFORCE RTX")
                .font(.system(size: 10, weight: .heavy))
                .tracking(1.3)
                .foregroundStyle(th.label)
            ZStack {
                Circle()
                    .stroke(th.track, lineWidth: 9)
                Circle()
                    .trim(from: 0, to: min(1, max(0, (s?.temp_gpu_c ?? 0) / 100)))
                    .stroke(Theme.tempColor(s?.temp_gpu_c),
                            style: StrokeStyle(lineWidth: 9, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                VStack(spacing: 0) {
                    Text(Fmt.num(s?.temp_gpu_c, 1))
                        .font(.system(size: 30, weight: .heavy, design: .rounded))
                        .monospacedDigit()
                        .minimumScaleFactor(0.6)
                    Text("°C · GPU")
                        .font(.system(size: 10, weight: .semibold))
                        .foregroundStyle(th.label)
                }
            }
            .frame(width: 108, height: 108)
            Text("VRAM \(Fmt.num(s?.temp_mem_c, 1)) °C")
                .font(.system(size: 10, weight: .medium))
                .monospacedDigit()
                .foregroundStyle(th.label)
        }
        .frame(maxWidth: .infinity, minHeight: 150)
        .padding(8)
        .background(th.track.opacity(0.35), in: RoundedRectangle(cornerRadius: 10))
    }

    private var metrics: some View {
        let gpuMax = Double(max(1, agent.history.compactMap { $0.clock_gpu_mhz }.max() ?? 1))
        let memMax = Double(max(1, agent.history.compactMap { $0.clock_mem_mhz }.max() ?? 1))
        let power = s?.power_w.flatMap { w in s?.power_limit_w.flatMap { $0 > 0 ? w / $0 : nil } }
        return ViewThatFits(in: .horizontal) {
            HStack(spacing: 10) { metricTiles(gpuMax, memMax, power) }
                .frame(minWidth: 620)
            LazyVGrid(columns: [GridItem(.flexible(), spacing: 10), GridItem(.flexible(), spacing: 10)], spacing: 10) {
                metricTiles(gpuMax, memMax, power)
            }
        }
    }

    @ViewBuilder private func metricTiles(_ gpuMax: Double, _ memMax: Double, _ power: Double?) -> some View {
            metric("GPU", "cpu", Fmt.int(s?.clock_gpu_mhz), "MHz", s?.clock_gpu_mhz.map { Double($0) / gpuMax }, Theme.green, "Máximo observado")
            metric("Memoria", "memorychip", Fmt.int(s?.clock_mem_mhz), "MHz", s?.clock_mem_mhz.map { Double($0) / memMax }, Theme.green, "Máximo observado")
            metric("Temperatura", "thermometer.medium", Fmt.num(s?.temp_gpu_c), "°C", s?.temp_gpu_c.map { $0 / 100 }, Theme.tempColor(s?.temp_gpu_c), "Escala 100 °C")
            metric("Consumo", "bolt.fill", Fmt.num(s?.power_w), "W", power, Theme.green, "Límite leído")
    }

    private func metric(_ title: String, _ icon: String, _ value: String, _ unit: String, _ fraction: Double?, _ color: Color, _ scale: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Label(title, systemImage: icon).font(.system(size: 12, weight: .medium)).foregroundStyle(th.label)
            HStack(alignment: .firstTextBaseline, spacing: 4) {
                Text(value).font(.system(size: 24, weight: .bold, design: .rounded)).monospacedDigit()
                Text(unit).font(.system(size: 11, weight: .medium)).foregroundStyle(th.label)
            }.lineLimit(1).minimumScaleFactor(0.65)
            HStack(spacing: 2) {
                ForEach(0..<20, id: \.self) { i in
                    RoundedRectangle(cornerRadius: 1).fill(fraction.map { $0.isFinite && Double(i + 1) / 20 <= $0 } == true ? color : th.track)
                }
            }.frame(height: 6).accessibilityHidden(true)
        }.padding(12).frame(maxWidth: .infinity, alignment: .leading).carbonPanel(th).help(scale)
    }

    private var telemetry: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("Telemetría", systemImage: "chart.xyaxis.line").font(.system(size: 17, weight: .semibold)).foregroundStyle(th.text)
            Text("Temperatura y consumo · últimos 5 minutos").font(.system(size: 11)).foregroundStyle(th.label)
            CarbonHistory(samples: agent.history, th: th)
            HStack(spacing: 10) {
                tiny("VRAM", "\(Fmt.num(s?.temp_mem_c, 1)) °C")
                Divider().frame(height: 30)
                tiny("Uso GPU", "\(Fmt.int(s?.util_gpu_pct)) %")
                Divider().frame(height: 30)
                tiny("VRAM usada", vram)
            }.padding(9).background(th.track.opacity(0.4), in: RoundedRectangle(cornerRadius: 8))
        }.padding(13).frame(maxWidth: .infinity, alignment: .leading).carbonPanel(th)
    }

    private func tiny(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.system(size: 10)).foregroundStyle(th.label)
            Text(value).font(.system(size: 15, weight: .semibold)).monospacedDigit().lineLimit(1).minimumScaleFactor(0.65)
        }.frame(maxWidth: .infinity, alignment: .leading)
    }

    private var vram: String {
        guard let used = s?.vram_used_mib, let total = s?.vram_total_mib else { return "--" }
        return String(format: "%.1f / %.1f GB", Double(used)/1024, Double(total)/1024)
    }

    private var fanPanel: some View {
        let rpm = s?.fan_rpm
        let slots = s?.gpu_profile?.tachSlots ?? (rpm ?? []).indices.filter { (rpm?[$0] ?? 0) > 0 && $0 < 16 }
        return VStack(alignment: .leading, spacing: 9) {
            Label("Ventiladores", systemImage: "fanblades.fill").font(.system(size: 17, weight: .semibold))
            if slots.isEmpty { Text("Sin canales de RPM observados").font(.system(size: 11)).foregroundStyle(th.label) }
            ForEach(slots, id: \.self) { i in
                HStack(spacing: 8) {
                    Image(systemName: "fanblades.fill").font(.system(size: 18)).foregroundStyle(Theme.greenDim)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Ventilador \(i + 1)").font(.system(size: 12, weight: .semibold))
                        Text("Tacómetro observado").font(.system(size: 9)).foregroundStyle(th.label)
                    }
                    Spacer(minLength: 3)
                    Text(rpm.flatMap { i < $0.count ? $0[i] : nil }.map { "\($0) RPM" } ?? "--").font(.system(size: 17, weight: .semibold, design: .rounded)).monospacedDigit()
                }.padding(9).background(th.track.opacity(0.45), in: RoundedRectangle(cornerRadius: 8))
            }
            HStack(spacing: 4) {
                Button { if !fanGuard.autoVerified || fanGuard.active || fanGuard.busy { fanGuard.restoreAuto() } } label: {
                    Text("AUTO").frame(maxWidth: .infinity).padding(.vertical, 7)
                        .background(fanGuard.autoVerified && !fanGuard.active ? Theme.greenDim.opacity(0.4) : th.track, in: RoundedRectangle(cornerRadius: 6))
                }.buttonStyle(.plain)
                Button { page = "Ventiladores" } label: {
                    Text("Manual").frame(maxWidth: .infinity).padding(.vertical, 7)
                        .background(fanGuard.active ? Theme.greenDim.opacity(0.4) : th.track, in: RoundedRectangle(cornerRadius: 6))
                }.buttonStyle(.plain)
            }.font(.system(size: 12, weight: .semibold))
            Text(fanGuard.rmStatus).font(.system(size: 11)).foregroundStyle(th.label).fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 6) {
                Button("Curva recomendada") { page = "Ventiladores" }
                Button("Comportamiento AUTO") { openWindow(id: "firmware-curve"); NSApp.activate(ignoringOtherApps: true) }
            }.font(.system(size: 10)).buttonStyle(.bordered)
        }.padding(13).frame(maxWidth: .infinity, alignment: .leading).carbonPanel(th)
    }

    private var quickControls: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: 12) {
                quickSwitch("Eco", "leaf.fill", controls.eco, controls.setEco)
                Divider().frame(height: 22)
                quickSwitch("Reposo", "moon.fill", controls.reposo, controls.setReposo)
                Divider().frame(height: 22)
                boostPicker
                Spacer(minLength: 0)
            }
            VStack(alignment: .leading, spacing: 6) {
                HStack { quickSwitch("Eco", "leaf.fill", controls.eco, controls.setEco); quickSwitch("Reposo", "moon.fill", controls.reposo, controls.setReposo) }
                boostPicker
            }
        }.padding(10).carbonPanel(th)
    }

    private func quickSwitch(_ title: String, _ icon: String, _ value: Bool?, _ action: @escaping (Bool) -> Void) -> some View {
        HStack(spacing: 5) {
            Label(title, systemImage: icon).foregroundStyle(th.text)
            if let value {
                Toggle(title, isOn: Binding(get: { value }, set: action)).labelsHidden().toggleStyle(.switch).controlSize(.small).tint(Theme.greenDim).disabled(controls.busy)
            } else { Text("--").foregroundStyle(th.label) }
        }.font(.system(size: 12)).fixedSize(horizontal: true, vertical: false)
    }

    private var boostPicker: some View {
        HStack(spacing: 5) {
            Label("Boost", systemImage: "bolt.fill").font(.system(size: 12))
            Picker("Boost", selection: $agent.boostMode) {
                ForEach(BoostMode.allCases, id: \.self) { m in Text(m.label).tag(m) }
            }.labelsHidden().frame(width: 105)
        }.fixedSize(horizontal: true, vertical: false)
    }

    private var details: some View {
        Card(icon: "cpu", title: "Lecturas adicionales") {
            InfoLine(name: "Placa", value: "\(Fmt.num(s?.temp_board_c, 1)) °C")
            InfoLine(name: "Consumo medio", value: "\(Fmt.num(s?.power_avg_w, 1)) W")
            InfoLine(name: "Límite leído", value: "\(Fmt.num(s?.power_limit_w)) W")
            InfoLine(name: "Estado", value: s?.pstate.map { "P\($0)" } ?? "--")
            InfoLine(name: "Uso memoria", value: "\(Fmt.int(s?.util_mem_pct)) %")
            InfoLine(name: "Reloj SM", value: "\(Fmt.int(s?.clock_sm_mhz)) MHz")
            InfoLine(name: "Reloj vídeo", value: "\(Fmt.int(s?.clock_video_mhz)) MHz")
            InfoLine(name: "Codificación / decodificación", value: "\(Fmt.int(s?.util_enc_pct)) / \(Fmt.int(s?.util_dec_pct)) %")
            InfoLine(name: "Motivo de limitación", value: s?.throttleText ?? "--")
            if let profile = s?.gpu_profile {
                InfoLine(name: "GPU ID", value: String(profile.gpu_id))
                InfoLine(name: "Sensores", value: profile.thermalNames.joined(separator: ", "))
                InfoLine(name: "Potencia", value: profile.powerNames.joined(separator: ", "))
            }
        }
    }

    @ViewBuilder private var warnings: some View {
        ForEach(Alerts.list(agent.alertReadings, reposo: controls.reposo), id: \.text) { alert in
            Text(alert.text).font(.system(size: 12, weight: .medium)).foregroundStyle(alert.level == .danger ? Theme.hot : alert.level == .warning ? Theme.warn : th.text)
                .frame(maxWidth: .infinity, alignment: .leading).padding(9).carbonPanel(th)
        }
        if let problem = agent.problem, s == nil {
            Text(problem).foregroundStyle(Theme.warn).frame(maxWidth: .infinity, alignment: .leading)
        }
        if let message = controls.message {
            Text(message).font(.system(size: 11)).frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

private extension View {
    func carbonPanel(_ th: Theme) -> some View {
        self.background {
            RoundedRectangle(cornerRadius: 12).fill(LinearGradient(colors: [th.panel, th.panel.opacity(0.65)], startPoint: .topLeading, endPoint: .bottomTrailing))
        }.overlay { RoundedRectangle(cornerRadius: 12).stroke(th.panelEdge, lineWidth: 1) }
    }
}

private struct CarbonHistory: View {
    let samples: [Sample]
    let th: Theme
    private var end: Double { samples.last?.t ?? 0 }
    private var maxPower: Double { max(10, ceil((samples.compactMap { $0.power_w }.max() ?? 0) / 25) * 25) }
    var body: some View {
        VStack(spacing: 5) {
            HStack {
                Label("°C", systemImage: "minus").foregroundStyle(Theme.green)
                Spacer()
                Label("W", systemImage: "minus").foregroundStyle(th.label)
            }.font(.system(size: 10))
            HStack(spacing: 6) {
                axis([100, 75, 50, 25, 0])
                GeometryReader { g in
                    ZStack {
                        Path { p in
                            for i in 0...4 {
                                let y = g.size.height * CGFloat(i) / 4
                                p.move(to: CGPoint(x: 0, y: y)); p.addLine(to: CGPoint(x: g.size.width, y: y))
                            }
                            for i in 0...5 {
                                let x = g.size.width * CGFloat(i) / 5
                                p.move(to: CGPoint(x: x, y: 0)); p.addLine(to: CGPoint(x: x, y: g.size.height))
                            }
                        }.stroke(th.track, lineWidth: 1)
                        line(g.size, scale: 100) { $0.temp_gpu_c }.stroke(Theme.green, lineWidth: 2)
                        line(g.size, scale: maxPower) { $0.power_w }.stroke(th.label, style: StrokeStyle(lineWidth: 1.5, dash: [4, 3]))
                    }.clipped()
                }
                axis([maxPower, maxPower * 0.75, maxPower * 0.5, maxPower * 0.25, 0])
            }.frame(height: 130)
            HStack { Text("−5 min"); Spacer(); Text("Ahora") }.font(.system(size: 10)).foregroundStyle(th.label)
        }
    }
    private func axis(_ values: [Double]) -> some View {
        VStack {
            ForEach(values.indices, id: \.self) { i in
                if i > 0 { Spacer(minLength: 0) }
                Text(String(format: "%.0f", values[i]))
            }
        }.font(.system(size: 9)).monospacedDigit().foregroundStyle(th.label)
    }
    private func line(_ size: CGSize, scale: Double, value: (Sample) -> Double?) -> Path {
        Path { p in
            var started = false
            for sample in samples where sample.t >= end - 300_000 {
                guard let v = value(sample), v.isFinite else { started = false; continue }
                let x = size.width * CGFloat((sample.t - end + 300_000) / 300_000)
                let y = size.height * CGFloat(1 - min(1, max(0, v / scale)))
                let pt = CGPoint(x: x, y: y)
                if started { p.addLine(to: pt) } else { p.move(to: pt); started = true }
            }
        }
    }
}

struct MenuContent: View {
    /// Not observed here, only told the boost mode the user picks. Every value the menu shows, except the fan submenu
    /// (FanGuardMenu, which reads its own model), comes from `menu`, which stands still while the menu is open, so
    /// that a new reading does not rebuild the items under the pointer and move the highlight (MenuHold.swift).
    let agent: Agent
    @EnvironmentObject var menu: MenuHold<MenuLines>
    @Environment(\.openWindow) private var openWindow

    init(agent: Agent) {
        self.agent = agent
    }

    var body: some View {
        let lines = menu.shown
        let s = lines.sample
        if let alert = lines.alert { Text(alert) }
        Text("Temperatura: \(Fmt.num(s?.temp_gpu_c, 0)) °C")
        Text("Consumo: \(Fmt.num(s?.power_w, 0)) W · \(s?.pstate.map { "P\($0)" } ?? "--")")
        Text("Reloj: \(Fmt.int(s?.clock_gpu_mhz)) MHz · Uso: \(Fmt.int(s?.util_gpu_pct)) %")
        Text("Ventilador: \(fanText(s?.fan_rpm))")
        if let p = lines.problem, s == nil { Text(p) }
        Divider()
        FanGuardMenu()
        Divider()
        // a Picker in a menu-style MenuBarExtra is a submenu with a checkmark on the current mode
        Picker("Rendimiento de la GPU", selection: Binding(get: { lines.boost }, set: { agent.boostMode = $0 })) {
            ForEach(BoostMode.allCases, id: \.self) { m in Text(m.label).tag(m) }
        }
        Button("Abrir NVMetrics") { openWindow(id: "main-carbon"); NSApp.activate(ignoringOtherApps: true) }
            .keyboardShortcut("o")
        Button("Diagnóstico…") { openWindow(id: "doctor"); NSApp.activate(ignoringOtherApps: true) }
        Button("Salir") { NSApp.terminate(nil) }.keyboardShortcut("q")
    }

    /// nil: no reading; empty: every fan stopped (the shared block lists coolers only up to the last that turns)
    private func fanText(_ rpm: [Int]?) -> String {
        guard let rpm else { return "--" }
        guard let top = rpm.max(), top > 0 else { return "parados" }
        return "\(top) RPM"
    }
}
