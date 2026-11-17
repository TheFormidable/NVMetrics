// NVMetrics fan v0.2: two-fan live telemetrics + read-only curve advice.
// The fan count is inferred from positive RPM observations and cached across launches.
// It is NOT a firmware-discovered physical count; no controls here affect RPM.
import SwiftUI

struct FanCard: View {
    @EnvironmentObject var agent: Agent
    @EnvironmentObject var fanGuard: FanGuardUIModel
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        let th = Theme(scheme: scheme)
        let sample = agent.latest
        let measured = sample?.fan_rpm
        let profile = sample?.gpu_profile
        let slots = profile?.tachSlots ?? (measured ?? []).indices.filter { (measured?[$0] ?? 0) > 0 && $0 < 16 }
        return Card(icon: "fanblades.fill", title: "Ventiladores") {
            if !slots.isEmpty {
                ForEach(slots, id: \.self) { i in
                    let rpm: Int? = measured.map { i < $0.count ? $0[i] : 0 }
                    InfoLine(name: "Ventilador \(i + 1)",
                             value: rpm.map { $0 == 0 ? "0 RPM · parado" : "\($0) RPM" } ?? "--")
                }
                InfoLine(name: "Giran ahora", value: spinningText(sample))
            } else if measured == nil {
                InfoLine(name: "RPM", value: "Sin lectura")
            } else {
                InfoLine(name: "RPM", value: "0 · sin recuento conocido")
            }
            InfoLine(name: "VRAM", value: vram(sample))
            if let profile {
                InfoLine(name: "Sensores", value: profile.thermalNames.isEmpty ? "--" : profile.thermalNames.joined(separator: ", "))
                InfoLine(name: "Potencia", value: profile.powerNames.isEmpty ? "--" : profile.powerNames.joined(separator: ", "))
            }
            Text("\(slots.count) canales de tacómetro observados en esta sesión. La cantidad física no está confirmada por el firmware.")
                .font(.system(size: 10)).foregroundStyle(th.label)
                .fixedSize(horizontal: false, vertical: true)
            Text(fanGuard.active ? "Control manual supervisado · consulta el estado RM." : fanGuard.autoVerified ? "Control automático del firmware verificado." : "Control RM: consulta la confirmación de AUTO.")
                .font(.system(size: 10, weight: .medium)).foregroundStyle(th.label)
        }
    }

    private func spinningText(_ sample: Sample?) -> String {
        guard let sample else { return "--" }
        if let n = sample.fan_live?.spinning_count { return String(n) }
        guard let rpm = sample.fan_rpm else { return "--" }
        return String(rpm.filter { $0 > 0 }.count)
    }

    private func vram(_ sample: Sample?) -> String {
        guard let used = sample?.vram_used_mib, let total = sample?.vram_total_mib else { return "--" }
        return String(format: "%.1f / %.1f GB", Double(used)/1024, Double(total)/1024)
    }
}

struct FanCurveCard: View {
    @EnvironmentObject var agent: Agent
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        let th = Theme(scheme: scheme)
        let sample = agent.latest
        let live = sample?.fan_live
        let pct = live?.recommendation_pct
        return Card(icon: "chart.xyaxis.line", title: "Curva recomendada · en tiempo real") {
            HStack(alignment: .firstTextBaseline) {
                Text("GPU: \(Fmt.num(sample?.temp_gpu_c, 1)) °C")
                    .font(.system(size: 13, weight: .semibold)).foregroundStyle(th.text)
                Spacer()
                Text(pct.map { "Objetivo: \($0) %" } ?? "Objetivo: --")
                    .font(.system(size: 15, weight: .bold, design: .rounded))
                    .foregroundStyle(live?.emergency_advisory == true ? Theme.hot : Theme.green)
                    .monospacedDigit()
            }
            FanCurveGraph(temperature: sample?.temp_gpu_c, target: pct)
                .frame(height: 92)
            HStack(spacing: 6) {
                Image(systemName: "lock.shield.fill")
                Text("Simulación de solo lectura · nunca aplicada a la GPU")
            }
            .font(.system(size: 11, weight: .medium)).foregroundStyle(th.label)
            if sample != nil && live?.temperature_fresh == false {
                Text("La temperatura no se ha actualizado recientemente: recomendación suspendida.")
                    .font(.system(size: 11)).foregroundStyle(Theme.warn)
            } else if live?.emergency_advisory == true {
                Text("Aviso térmico: el cálculo recomienda 100 %, pero no controla los ventiladores.")
                    .font(.system(size: 11)).foregroundStyle(Theme.hot)
            }
        }
    }
}

private struct FanCurveGraph: View {
    let temperature: Double?
    let target: Int?
    @Environment(\.colorScheme) private var scheme

    // Must match agent/fan_preview.h v0.1 points. The actual live target comes from C.
    private let points: [(Double, Double)] = [(40,30),(55,40),(65,55),(75,75),(82,90),(88,100)]

    var body: some View {
        let th = Theme(scheme: scheme)
        return GeometryReader { geo in
            let w = max(1, geo.size.width)
            let h = max(1, geo.size.height)
            ZStack {
                Path { path in
                    for n in [0.25, 0.5, 0.75] {
                        let y = h * CGFloat(1-n)
                        path.move(to: CGPoint(x: 0, y: y))
                        path.addLine(to: CGPoint(x: w, y: y))
                    }
                }.stroke(th.track, lineWidth: 0.8)
                Path { path in
                    for (i, point) in points.enumerated() {
                        let pt = CGPoint(x: x(point.0, w), y: y(point.1, h))
                        if i == 0 { path.move(to: pt) } else { path.addLine(to: pt) }
                    }
                }
                .stroke(Theme.green, style: StrokeStyle(lineWidth: 2.0, lineCap: .round, lineJoin: .round))
                if let temperature, let target, temperature.isFinite {
                    Circle().fill(Theme.green).frame(width: 9, height: 9)
                        .overlay(Circle().stroke(th.background, lineWidth: 1))
                        .position(x: x(temperature, w), y: y(Double(target), h))
                }
            }
        }
        .accessibilityLabel("Curva teórica: 40 grados 30 por ciento hasta 88 grados 100 por ciento; sin aplicación física")
    }

    private func x(_ c: Double, _ width: CGFloat) -> CGFloat {
        CGFloat(min(1, max(0, (c - 35) / 60))) * width
    }
    private func y(_ percent: Double, _ height: CGFloat) -> CGFloat {
        (1 - CGFloat(min(100, max(0, percent))) / 100) * height
    }
}
