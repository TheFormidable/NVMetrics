import Foundation

// A UI monitoring preference, never the manual fan guard's sampling policy.
struct AdaptiveCadence {
    private var target: Int?
    private var since: Double?

    mutating func reset() { target = nil; since = nil }

    mutating func next(utilization: Int?, now: Double, current: Int) -> Int? {
        guard let u = utilization, (0...100).contains(u), now.isFinite else {
            reset(); return nil
        }
        let wanted = u >= 25 ? 500 : u <= 5 ? 2000 : 1000
        guard wanted != current else { reset(); return nil }
        if target != wanted || since == nil || now < (since ?? now) {
            target = wanted; since = now; return nil
        }
        let delay: Double = wanted == 500 ? 2 : wanted == 1000 ? 5 : 15
        guard now - (since ?? now) >= delay else { return nil }
        reset()
        return wanted
    }
}
