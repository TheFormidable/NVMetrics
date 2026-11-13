// One reading from nvmetrics-agent (metrics_json.h). Every field is optional: null in the JSON means the driver did not
// give that number, and the app shows "--" instead of a zero that looks like a reading.
import Foundation

struct Sample: Decodable, Equatable {
    var t: Double
    var temp_gpu_c: Double?
    var temp_mem_c: Double?
    var temp_board_c: Double?
    var power_w: Double?
    var power_avg_w: Double?
    var power_limit_w: Double?
    var clock_gpu_mhz: Int?
    var clock_mem_mhz: Int?
    var clock_video_mhz: Int?
    var clock_sm_mhz: Int?
    var pstate: Int?
    var util_gpu_pct: Int?
    var util_mem_pct: Int?
    var util_enc_pct: Int?
    var util_dec_pct: Int?
    var throttle_mask: Int?
    var throttle: [String]?
    var fan_rpm: [Int]?
    var vram_total_mib: Int?
    var vram_used_mib: Int?

    var date: Date { Date(timeIntervalSince1970: t / 1000) }

    /// the throttle reasons in plain Spanish; "idle" is the normal state of a resting card, not a problem
    var throttleText: String {
        guard let list = throttle else { return "--" }
        let names: [String: String] = [
            "idle": "en reposo", "app_clocks": "relojes fijados por una app", "sw_power_cap": "límite de potencia",
            "hw_slowdown": "freno de hardware", "sync_boost": "boost sincronizado", "sw_thermal": "temperatura (software)",
            "hw_thermal": "temperatura (hardware)", "power_brake": "freno de alimentación", "display_clock": "reloj de pantalla",
            "board_limit": "límite de la placa", "reliability": "fiabilidad del voltaje",
        ]
        if list.isEmpty { return "ninguno" }
        return list.map { names[$0] ?? $0 }.joined(separator: ", ")
    }

    /// true when the card is slowing itself down for heat or power (not just resting)
    var isThrottlingHard: Bool {
        guard let list = throttle else { return false }
        return list.contains { ["sw_thermal", "hw_thermal", "hw_slowdown", "power_brake"].contains($0) }
    }
}

enum Fmt {
    static func num(_ v: Double?, _ digits: Int = 0) -> String {
        guard let v else { return "--" }
        return String(format: "%.\(digits)f", v)
    }
    static func int(_ v: Int?) -> String { v.map { String($0) } ?? "--" }
}
