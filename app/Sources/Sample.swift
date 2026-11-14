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
    var health: Health?            // null or missing: the driver has no health counters (before 3.0) or an older agent
    var boost: BoostState?         // only with "watch --control"
    var protect: ProtectState?     // only with "watch --control" (the app always runs it that way)

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

/// The 3.0 driver's own counters (debug.nvrm_* sysctls), all since the Mac started. null: that counter is missing, or
/// for xid_last / chan_last_kind / bugcheck_last: nothing happened yet.
struct Health: Decodable, Equatable {
    var xid_count: Int?
    var xid_last: Int?
    var chan_recovered: Int?
    var chan_gave_up: Int?
    var chan_dropped: Int?
    var chan_kicks: Int?
    var chan_last_kind: Int?
    var bugchecks: Int?
    var bugcheck_last: Int?
    var rm_asserts: Int?
    var switches_nondefault: Int?
    var switches: String?
    var fence_state: Int?
}

/// What the agent is doing with the GPU's "boost to max" request (watch --control).
struct BoostState: Decodable, Equatable {
    var mode: String?              // "off" | "auto" | "max"
    var held: Bool?
    var error: String?             // "0x<RM status>" when the driver refused the last request
}

/// The agent's heat protection (watch --control, agent/protect.h): while active it does not hold the boost request.
/// why: "hot" (core), "mem_hot" (video memory) or "fans_stopped" (fans stopped on a warm core); null while inactive.
struct ProtectState: Decodable, Equatable {
    var active: Bool?
    var why: String?
}

enum Fmt {
    static func num(_ v: Double?, _ digits: Int = 0) -> String {
        guard let v else { return "--" }
        return String(format: "%.\(digits)f", v)
    }
    static func int(_ v: Int?) -> String { v.map { String($0) } ?? "--" }
}
