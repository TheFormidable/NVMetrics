import SwiftUI

/// A labelled bar like the mock-ups' GPU / MEM / TEMP rows: segmented fill, big number, unit.
struct MeterRow: View {
    let icon: String, name: String, value: String, unit: String
    let fraction: Double?            // 0...1, nil = no reading
    var color: Color = Theme.green
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        let th = Theme(scheme: scheme)
        HStack(spacing: 10) {
            Image(systemName: icon).frame(width: 18).foregroundStyle(th.label)
            Text(name).font(.system(size: 12, weight: .semibold)).foregroundStyle(th.label).frame(width: 64, alignment: .leading)
            GeometryReader { g in
                let n = 28
                let lit = Int(((fraction ?? 0).clamped(to: 0...1) * Double(n)).rounded())
                HStack(spacing: 2) {
                    ForEach(0..<n, id: \.self) { i in
                        RoundedRectangle(cornerRadius: 1.5)
                            .fill(i < lit ? color : th.track)
                            .shadow(color: i < lit && th.dark ? color.opacity(0.6) : .clear, radius: 3)
                            .frame(width: max(1, (g.size.width - CGFloat(n - 1) * 2) / CGFloat(n)))
                    }
                }
            }
            .frame(height: 8)
            Text(value).font(.system(size: 22, weight: .semibold, design: .rounded)).monospacedDigit()
                .foregroundStyle(th.text).frame(width: 78, alignment: .trailing)
            Text(unit).font(.system(size: 11)).foregroundStyle(th.label).frame(width: 30, alignment: .leading)
        }
        .frame(height: 28)
    }
}

/// A rounded panel with a title, like the VOLTAGE / CLOCK / FAN cards.
struct Card<Content: View>: View {
    let icon: String, title: String
    var highlighted = false
    @ViewBuilder let content: Content
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        let th = Theme(scheme: scheme)
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                Image(systemName: icon)
                Text(title.uppercased()).font(.system(size: 12, weight: .bold))
                Spacer()
            }
            .padding(.horizontal, 12).padding(.vertical, 8)
            .foregroundStyle(highlighted ? Color.black : th.text)
            .background(highlighted ? AnyShapeStyle(LinearGradient(colors: [Theme.green, Theme.greenDim], startPoint: .top, endPoint: .bottom))
                                    : AnyShapeStyle(th.track.opacity(0.6)))
            VStack(alignment: .leading, spacing: 8) { content }
                .padding(.horizontal, 12).padding(.bottom, 12)
            Spacer(minLength: 0)
        }
        .background(th.panel)
        .clipShape(RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(highlighted ? Theme.green.opacity(0.7) : th.panelEdge, lineWidth: 1))
        .shadow(color: highlighted ? th.glow : .clear, radius: 10)
    }
}

struct InfoLine: View {
    let name: String, value: String
    var tint: Color? = nil           // colour of the value; nil = normal text
    @Environment(\.colorScheme) private var scheme
    var body: some View {
        let th = Theme(scheme: scheme)
        HStack {
            Text(name).font(.system(size: 11, weight: .medium)).foregroundStyle(th.label)
            Spacer()
            Text(value).font(.system(size: 13, weight: .semibold, design: .rounded)).monospacedDigit().foregroundStyle(tint ?? th.text)
        }
    }
}

/// Temperature (green) and power (dashed) over the last minutes.
struct HistoryGraph: View {
    let samples: [Sample]
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        let th = Theme(scheme: scheme)
        GeometryReader { g in
            let w = g.size.width, h = g.size.height
            let t0 = samples.first?.t ?? 0, t1 = max((samples.last?.t ?? 1), t0 + 1)
            let maxW = max(samples.compactMap { $0.power_w }.max() ?? 1, 10)
            ZStack {
                ForEach([25.0, 50, 75], id: \.self) { v in
                    Path { p in let y = h - h * v / 100; p.move(to: CGPoint(x: 0, y: y)); p.addLine(to: CGPoint(x: w, y: y)) }
                        .stroke(th.track, lineWidth: 1)
                }
                line(w, h, t0, t1) { s in s.temp_gpu_c.map { $0 / 100 } }
                    .stroke(Theme.green, style: StrokeStyle(lineWidth: 2, lineJoin: .round))
                    .shadow(color: th.dark ? Theme.green.opacity(0.6) : .clear, radius: 4)
                line(w, h, t0, t1) { s in s.power_w.map { $0 / maxW } }
                    .stroke(th.label, style: StrokeStyle(lineWidth: 1.2, dash: [4, 3]))
            }
        }
    }

    private func line(_ w: CGFloat, _ h: CGFloat, _ t0: Double, _ t1: Double, _ y: @escaping (Sample) -> Double?) -> Path {
        Path { p in
            var started = false
            for s in samples {
                guard let v = y(s) else { started = false; continue }
                let pt = CGPoint(x: w * (s.t - t0) / (t1 - t0), y: h - h * v.clamped(to: 0...1))
                if started { p.addLine(to: pt) } else { p.move(to: pt); started = true }
            }
        }
    }
}

extension Double {
    func clamped(to r: ClosedRange<Double>) -> Double { Swift.min(Swift.max(self, r.lowerBound), r.upperBound) }
}
