import Foundation

// Read-only session data from the single existing telemetry agent.
struct GamingCapture {
    static let capacity = 7200
    private(set) var points: [Sample] = []
    private var previous: Sample?
    private(set) var wattSeconds = 0.0
    private(set) var coveredSeconds = 0.0
    private(set) var peakGPU: Double?
    private(set) var peakVRAM: Double?
    var duration: Double {
        guard let first = points.first, let last = points.last else { return 0 }
        return max(0, (last.t - first.t) / 1000)
    }
    var energyWh: Double? { coveredSeconds > 0 ? wattSeconds / 3600 : nil }
    var averageWatts: Double? { coveredSeconds > 0 ? wattSeconds / coveredSeconds : nil }

    static func flipRate(previous: Sample?, next: Sample) -> Double? {
        guard let old = previous, let a = old.display_flip_count, let b = next.display_flip_count,
              b >= a else { return nil }
        let dt = (next.t - old.t) / 1000
        guard dt.isFinite, dt >= 0.1, dt <= 5 else { return nil }
        return Double(b - a) / dt
    }

    mutating func breakContinuity() { previous = nil }

    @discardableResult mutating func append(_ sample: Sample) -> Bool {
        guard points.count < Self.capacity, sample.t.isFinite, sample.t > 0 else { return false }
        if let last = points.last {
            if sample.t == last.t { return true }
            if sample.t < last.t { return false }
        }
        if let old = previous, let a = old.power_w, let b = sample.power_w,
           a.isFinite, b.isFinite, a >= 0, b >= 0, a <= 1000, b <= 1000 {
            let dt = (sample.t - old.t) / 1000
            if dt > 0, dt <= 5 {
                wattSeconds += (a + b) * 0.5 * dt
                coveredSeconds += dt
            }
        }
        if let t = sample.temp_gpu_c, t.isFinite, t > 0, t <= 150 { peakGPU = max(peakGPU ?? t, t) }
        if let t = sample.temp_mem_c, t.isFinite, t > 0, t <= 150 { peakVRAM = max(peakVRAM ?? t, t) }
        previous = sample
        points.append(sample)
        return points.count < Self.capacity
    }

    func csv() -> String {
        var lines = ["timestamp_ms,gpu_c,vram_c,power_w,gpu_util_pct,gpu_mhz,mem_mhz,display_flip_count"]
        for s in points {
            let fields: [String] = [String(s.t), s.temp_gpu_c.map { String($0) } ?? "",
                s.temp_mem_c.map { String($0) } ?? "", s.power_w.map { String($0) } ?? "",
                s.util_gpu_pct.map { String($0) } ?? "", s.clock_gpu_mhz.map { String($0) } ?? "",
                s.clock_mem_mhz.map { String($0) } ?? "", s.display_flip_count.map { String($0) } ?? ""]
            lines.append(fields.joined(separator: ","))
        }
        return lines.joined(separator: "\n") + "\n"
    }
}

struct ResourceReading: Identifiable {
    let id: Int
    let name: String
    let cpu: Double
    let rssMiB: Double

    static func parse(_ text: String) -> [ResourceReading] {
        let app = "/Applications/NVMetrics.app/Contents/"
        let root = "/Library/PrivilegedHelperTools/org.nvmetrics.rmguard/"
        return text.split(separator: "\n").compactMap { line in
            let f = line.split(maxSplits: 3, whereSeparator: { $0.isWhitespace })
            guard f.count == 4, let pid = Int(f[0]), let cpu = Double(f[1]), let rss = Double(f[2]),
                  cpu.isFinite, rss.isFinite, cpu >= 0, rss >= 0 else { return nil }
            let command = String(f[3])
            guard command.hasPrefix(app) || command.hasPrefix(root) else { return nil }
            return ResourceReading(id: pid, name: URL(fileURLWithPath: command).lastPathComponent,
                                   cpu: cpu, rssMiB: rss / 1024)
        }
    }
}
