import SwiftUI
import AppKit

struct MainView: View {
    @EnvironmentObject var agent: Agent
    @EnvironmentObject var controls: Controls
    @EnvironmentObject var fanGuard: FanGuardUIModel
    @Environment(\.colorScheme) private var scheme
    @AppStorage("nvmetrics.dashboard.skin") private var skin = "dark"
    @State private var page = "overview"
    @Environment(\.openWindow) private var openWindow

    private let pages = [("overview", "gauge.with.needle.fill", "Resumen"),
                         ("thermal", "chart.xyaxis.line", "Telemetría"),
                         ("fan", "fanblades.fill", "Ventiladores"),
                         ("settings", "gearshape.fill", "Ajustes"),
                         ("health", "shield.checkered", "Diagnóstico")]

    var body: some View {
        let th = Theme(scheme: skin == "light" ? .light : .dark)
        let s = agent.latest
        let alerts = Alerts.list(agent.alertReadings, reposo: controls.reposo)
        VStack(spacing: 8) {
            header(th)
                .padding(.horizontal, 18).padding(.top, 28)
            HStack(alignment: .top, spacing: 8) {
            VStack(spacing: 7) {
                ForEach(pages.indices, id: \.self) { index in
                    let item = pages[index]
                    Button { page = item.0 } label: {
                        Image(systemName: item.1)
                            .font(.system(size: 16, weight: .semibold))
                            .frame(width: 36, height: 34)
                            .foregroundStyle(page == item.0 ? Color.black : th.text)
                            .background(page == item.0 ? Theme.green : th.track,
                                        in: RoundedRectangle(cornerRadius: 7))
                    }.buttonStyle(.plain).help(item.2).accessibilityLabel(item.2)
                }
                Spacer(minLength: 0)
            }.padding(5).background(th.panel, in: RoundedRectangle(cornerRadius: 9))
            ScrollView {
            VStack(spacing: 12) {
            if !alerts.isEmpty { alertBlock(alerts, th) }
            if let problem = agent.problem, s == nil {
                Text(problem).font(.system(size: 13)).foregroundStyle(Theme.warn)
                    .frame(maxWidth: .infinity).padding(10)
                    .background(th.panel).clipShape(RoundedRectangle(cornerRadius: 8))
            }
            if page == "overview" || page == "thermal" { meters(s, th) }
            if page == "overview" || page == "thermal" {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 185), spacing: 12, alignment: .top)], spacing: 12) {
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
                    InfoLine(name: "Uso memoria", value: "\(Fmt.int(s?.util_mem_pct)) %")
                    InfoLine(name: "Reloj SM", value: "\(Fmt.int(s?.clock_sm_mhz)) MHz")
                    InfoLine(name: "Reloj vídeo", value: "\(Fmt.int(s?.clock_video_mhz)) MHz")
                    InfoLine(name: "Vídeo (cod./decod.)", value: "\(Fmt.int(s?.util_enc_pct)) / \(Fmt.int(s?.util_dec_pct)) %")
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Frenando por").font(.system(size: 11, weight: .medium)).foregroundStyle(th.label)
                        Text(s?.throttleText ?? "--").font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(s?.isThrottlingHard == true ? Theme.hot : th.text)
                    }
                }
                FanCard()
            }
            if page == "overview" {
                Button("Control de ventiladores") { page = "fan" }
                    .buttonStyle(.borderedProminent).tint(Theme.greenDim)
            }
            }
            if page == "fan" {
            FanCard()
            FanGuardCard().fixedSize(horizontal: false, vertical: true)
            FanCurveCard().fixedSize(horizontal: false, vertical: true)
            Button("Comportamiento AUTO") { openWindow(id: "firmware-curve") }
            }
            if page == "settings" {
            modes(th)
            BoostCard().frame(maxWidth: .infinity)
            Picker("Intervalo de lectura", selection: $agent.intervalMs) {
                Text("0,5 s").tag(500); Text("1 s").tag(1000); Text("2 s").tag(2000)
            }
            .pickerStyle(.segmented)
            }
            if page == "health" {
                HealthCard(health: s?.health, hasReadings: s != nil)
                if let profile = s?.gpu_profile {
                    Card(icon: "cpu", title: "Canales observados") {
                        InfoLine(name: "GPU ID", value: String(profile.gpu_id))
                        InfoLine(name: "Térmicos", value: profile.thermalNames.joined(separator: ", "))
                        InfoLine(name: "Potencia", value: profile.powerNames.joined(separator: ", "))
                        InfoLine(name: "Tacómetros", value: String(profile.tachSlots.count))
                    }
                }
            }
            if let m = controls.message {
                Text(m).font(.system(size: 12)).foregroundStyle(th.text).frame(maxWidth: .infinity, alignment: .leading)
            }
        }
            .padding(.horizontal, 4).padding(.vertical, 8)
            .frame(maxWidth: .infinity)
            }
            }.padding(.horizontal, 12)
            HStack(spacing: 8) {
                Image(systemName: "shield.lefthalf.filled").foregroundStyle(Theme.green)
                Text(fanGuard.rmStatus).font(.system(size: 10)).lineLimit(2)
                Spacer(minLength: 4)
                if fanGuard.active || fanGuard.busy {
                    Button("AUTO") { fanGuard.restoreAuto() }.buttonStyle(.borderedProminent).tint(Theme.greenDim)
                }
            }.padding(9).background(th.panel, in: RoundedRectangle(cornerRadius: 9))
                .padding(.horizontal, 12).padding(.bottom, 8)
        }
        .frame(minWidth: 480, minHeight: 340)
        .background {
            if skin == "glass" {
                Rectangle().fill(.regularMaterial)
                    .overlay(LinearGradient(colors: [Color.green.opacity(0.22), .clear, Color.cyan.opacity(0.17)], startPoint: .topLeading, endPoint: .bottomTrailing))
            } else if skin == "graphite" {
                LinearGradient(colors: [Color(white: 0.24), Color(white: 0.10)], startPoint: .topLeading, endPoint: .bottomTrailing)
            } else { th.background }
        }
        .environment(\.colorScheme, skin == "light" ? .light : .dark)
        .preferredColorScheme(skin == "light" ? .light : .dark)
        .onAppear { controls.refresh() }
    }

    private func header(_ th: Theme) -> some View {
        HStack(spacing: 10) {
            if let icon = NSImage(named: "AppIcon") {
                Image(nsImage: icon).resizable().scaledToFit().frame(width: 40, height: 40)
            } else {
                Image(systemName: "gauge.with.needle.fill").font(.system(size: 26, weight: .bold)).foregroundStyle(Theme.green)
            }
            VStack(alignment: .leading, spacing: 0) {
                (Text("NV").foregroundStyle(Theme.green) + Text("Metrics").foregroundStyle(th.text))
                    .font(.system(size: 24, weight: .heavy, design: .rounded)).italic()
                Text("MONITOR DE GRÁFICA").font(.system(size: 9, weight: .semibold)).kerning(2).foregroundStyle(th.label)
            }
            .padding(.horizontal, 20).padding(.vertical, 5)
            .background {
                DashboardBadge().fill(LinearGradient(colors: [th.track, th.panel], startPoint: .top, endPoint: .bottom))
            }
            .overlay { DashboardBadge().stroke(Theme.green.opacity(0.55), lineWidth: 1) }
            Spacer()
            Picker("Estilo", selection: $skin) {
                Text("Oscuro").tag("dark")
                Text("Claro").tag("light")
                Text("Grafito").tag("graphite")
                Text("Cristal").tag("glass")
            }
            .pickerStyle(.menu).frame(width: 125)
            .help("Apariencia de NVMetrics")
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
        let maxGpu = Double(max(1, max(agent.history.compactMap { $0.clock_gpu_mhz }.max() ?? 0, s?.clock_gpu_mhz ?? 0)))
        let maxMem = Double(max(1, max(agent.history.compactMap { $0.clock_mem_mhz }.max() ?? 0, s?.clock_mem_mhz ?? 0)))
        return VStack(spacing: 4) {
            MeterRow(icon: "cpu", name: "GPU", value: Fmt.int(s?.clock_gpu_mhz), unit: "MHz",
                     fraction: s?.clock_gpu_mhz.map { Double($0) / maxGpu })
            MeterRow(icon: "memorychip", name: "MEM", value: Fmt.int(s?.clock_mem_mhz), unit: "MHz",
                     fraction: s?.clock_mem_mhz.map { Double($0) / maxMem })
            MeterRow(icon: "bolt.fill", name: "POTENCIA", value: Fmt.num(s?.power_w, 0), unit: "W",
                     fraction: s?.power_w.flatMap { w in s?.power_limit_w.flatMap { $0 > 0 ? w / $0 : nil } })
            MeterRow(icon: "thermometer.medium", name: "TEMP", value: Fmt.num(s?.temp_gpu_c, 0), unit: "°C",
                     fraction: s?.temp_gpu_c.map { $0 / 100 }, color: Theme.tempColor(s?.temp_gpu_c))
            Text("Relojes: máximo observado · potencia: límite leído · temperatura: escala 100 °C")
                .font(.system(size: 10)).foregroundStyle(th.label)
        }
        .padding(12)
        .background(th.panel.opacity(th.dark ? 0.6 : 1))
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(th.panelEdge, lineWidth: 1))
    }

    private func modes(_ th: Theme) -> some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 230), spacing: 8, alignment: .top)], spacing: 8) {
            modeCard(th, title: "Modo reposo", icon: "moon.fill", value: controls.reposo,
                     detail: "Deja que la gráfica baje a su estado de bajo consumo cuando no haces nada. "
                         + (controls.reposoHot == true ? "Se aplica al momento, también a las apps abiertas."
                                                       : "Se aplica al cerrar sesión y volver a entrar.")) {
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

private struct DashboardBadge: Shape {
    func path(in rect: CGRect) -> Path {
        let bevel = min(14, rect.width / 5)
        var path = Path()
        path.move(to: CGPoint(x: rect.minX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
        path.addLine(to: CGPoint(x: rect.maxX - bevel, y: rect.maxY))
        path.addLine(to: CGPoint(x: rect.minX + bevel, y: rect.maxY))
        path.closeSubpath()
        return path
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
