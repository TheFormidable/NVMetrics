@main struct FanSelectionTests {
    static func sequence(_ start: Int, _ target: Int) -> [Int] {
        var current = start
        var result: [Int] = []
        for _ in 0..<15 {
            guard let next = FanSelectionPolicy.nextStep(current: current, target: target, floor: 30, load: false, thermal: true) else { break }
            result.append(next)
            current = next
        }
        return result
    }

    static func main() {
        precondition(FanSelectionPolicy.canKeepVerifiedStep(current: 40, floor: 30))
        precondition(FanSelectionPolicy.canKeepVerifiedStep(current: 30, floor: 30))
        precondition(!FanSelectionPolicy.canKeepVerifiedStep(current: 25, floor: 30))
        precondition(!FanSelectionPolicy.canKeepVerifiedStep(current: 41, floor: 30))
        precondition(FanSelectionPolicy.clamp(25, floor: 30) == 30)
        precondition(FanSelectionPolicy.clamp(105, floor: 30) == 100)
        precondition(FanSelectionPolicy.clamp(35 - 5, floor: 30) == 30)
        precondition(FanSelectionPolicy.clamp(30 + 5, floor: 30) == 35)
        precondition(sequence(50, 30) == [45, 40, 35, 30])
        precondition(sequence(100, 30) == Array(stride(from: 95, through: 30, by: -5)))
        precondition(sequence(30, 100) == [100])
        precondition(sequence(30, 30).isEmpty)
        precondition(sequence(30, 25).isEmpty)
        precondition(FanSelectionPolicy.nextStep(current: 100, target: 95, floor: 50, load: true, thermal: false) == nil)
        precondition(FanSelectionPolicy.nextStep(current: 100, target: 95, floor: 30, load: false, thermal: false) == nil)
        precondition(FanSelectionPolicy.nextStep(current: 50, target: 45, floor: 30, load: false, thermal: false) == 45)
        let reason = FanSelectionPolicy.lowerRefusal(["sample": ["gpu_c":46.1,"mem_c":68.0,"power_w":50.10,"util_gpu_pct":20]])
        precondition(reason.contains("50.10 W") && !reason.contains("GPU"))
        print("PASS: fan selection floor, staged increments and bounded RM transitions")
    }
}
