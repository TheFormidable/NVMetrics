import Foundation
// Pure selection/transition policy; never performs an RM write.
struct FanSelectionPolicy {
    static func lowerRefusal(_ details: [String: Any]?) -> String {
        guard let sample = details?["sample"] as? [String: Any] else {
            return "Sin margen para bajar: GPU <58 °C, VRAM <82 °C, consumo ≤50 W y uso ≤20 %."
        }
        var reasons: [String] = []
        if let gpu = sample["gpu_c"] as? Double, gpu >= 58 { reasons.append(String(format: "GPU %.1f °C (debe ser <58 °C)", gpu)) }
        if let mem = sample["mem_c"] as? Double, mem >= 82 { reasons.append(String(format: "VRAM %.1f °C (debe ser <82 °C)", mem)) }
        if let power = sample["power_w"] as? Double, power > 50 { reasons.append(String(format: "consumo %.2f W (límite 50 W)", power)) }
        if let util = sample["util_gpu_pct"] as? Int, util > 20 { reasons.append("uso \(util) % (límite 20 %)") }
        return reasons.isEmpty ? "El guard rechazó el escalón antes de escribir; las condiciones cambiaron durante la comprobación." :
            "Bajada aplazada: " + reasons.joined(separator: "; ") + "."
    }

    static func canKeepVerifiedStep(current: Int, floor: Int) -> Bool {
        current >= floor && current <= 100 && current % 5 == 0
    }
    static func clamp(_ value: Int, floor: Int) -> Int {
        min(100, max(floor, (value / 5) * 5))
    }

    static func minimum(floor: Int, load: Bool, thermal: Bool, current: Int) -> Int {
        // Existing non-thermal policies do not permit reductions above 50%.
        (load || (!thermal && current > 50)) ? max(floor, current) : floor
    }

    static func nextStep(current: Int, target: Int, floor: Int, load: Bool, thermal: Bool) -> Int? {
        let lower = minimum(floor: floor, load: load, thermal: thermal, current: current)
        guard current >= floor, current <= 100, current % 5 == 0,
              target >= lower, target <= 100, target % 5 == 0, target != current else { return nil }
        // Existing RM raise supports a direct verified target; reductions must
        // be exactly one five-point step, each with independent readback/tach checks.
        return target > current ? target : current - 5
    }
}
