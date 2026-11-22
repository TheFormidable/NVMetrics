import Foundation

@main struct GamingCaptureTests {
    static func main() throws {
        let d = JSONDecoder()
        func sample(_ t: Int, _ w: Double, _ flips: UInt64) throws -> Sample {
            try d.decode(Sample.self, from: Data("{\"t\":\(t),\"power_w\":\(w),\"display_flip_count\":\(flips),\"temp_gpu_c\":55}".utf8))
        }
        let a = try sample(1000, 10, 100), b = try sample(2000, 30, 160)
        var c = GamingCapture()
        c.append(a); c.append(b)
        precondition(c.coveredSeconds == 1 && c.averageWatts == 20)
        precondition(abs((c.energyWh ?? 0) - 20.0 / 3600) < 0.000001)
        precondition(GamingCapture.flipRate(previous: a, next: b) == 60)
        let reset = try sample(3000, 30, 10)
        precondition(GamingCapture.flipRate(previous: b, next: reset) == nil)
        c.breakContinuity()
        c.append(reset)
        precondition(c.coveredSeconds == 1)
        c.append(try sample(10000, 100, 200)); precondition(c.coveredSeconds == 1)
        precondition(c.csv().contains("timestamp_ms,gpu_c"))
        var cadence = AdaptiveCadence()
        precondition(cadence.next(utilization: 30, now: 1, current: 2000) == nil)
        precondition(cadence.next(utilization: 30, now: 3, current: 2000) == 500)
        precondition(cadence.next(utilization: 0, now: 4, current: 500) == nil)
        precondition(cadence.next(utilization: 0, now: 18, current: 500) == nil)
        precondition(cadence.next(utilization: 0, now: 19, current: 500) == 2000)
        precondition(cadence.next(utilization: nil, now: 20, current: 500) == nil)
        let resources = ResourceReading.parse("12 1.5 2048 /Applications/NVMetrics.app/Contents/MacOS/NVMetrics\n13 9 90 /usr/bin/unrelated")
        precondition(resources.count == 1 && resources[0].rssMiB == 2 && resources[0].cpu == 1.5)
        print("PASS: gaming energy coverage, counter reset, gaps and adaptive cadence")
    }
}
