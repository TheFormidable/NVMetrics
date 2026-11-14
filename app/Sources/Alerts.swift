// The warnings NVMetrics shows, worked out from the recent readings (Agent.alertReadings): a block at the top of the
// window, the first one as the first line of the menu (in a short form, since menu items do not wrap), and "⚠︎" next to
// the temperature in the menu bar for a danger or a warning. Pure (no UI, no state of its own), so every threshold and
// every text is here. Each temperature is judged in whole degrees, the number the app shows, and a temperature alert,
// once on, stays until the reading is more than 2 °C below its threshold (`band`), so a reading that hovers at a
// threshold does not add and remove the block (and resize the window) every second. These are the app's thresholds for
// telling the user; the agent's protection (agent/protect.h) has its own and arrives here only as "protect".
import Foundation

struct GPUAlert: Equatable {
    /// declared most severe first; list() keeps this order
    enum Level: CaseIterable {
        case danger, warning, info
    }

    let level: Level
    let text: String
    /// one menu line (menu items do not wrap); the window shows `text`
    let menuText: String

    init(level: Level, text: String, menuText: String? = nil) {
        self.level = level
        self.text = text
        self.menuText = menuText ?? text
    }
}

enum Alerts {
    /// a temperature alert starts at its threshold and ends only once the reading is below threshold - band
    static let band = 2.0

    /// every alert for the newest reading, most severe first (rule order within a level); empty with no reading.
    /// readings: the recent readings, oldest first, the newest last (empty when there is no current reading); the older
    /// ones only decide whether a temperature alert stays on inside its band. reposo: the "Modo reposo" switch, nil
    /// while unknown.
    static func list(_ readings: [Sample], reposo: Bool?) -> [GPUAlert] {
        guard let s = readings.last else { return [] }
        var out: [GPUAlert] = []
        if let c = s.temp_gpu_c.map({ whole($0) }) {
            if hot(readings, \.temp_gpu_c, 88) {
                out.append(GPUAlert(level: .danger, text: "PELIGRO: la GPU está a \(deg(c)) °C. Cierra el juego o la carga y revisa la refrigeración."))
            } else if hot(readings, \.temp_gpu_c, 80) {
                out.append(GPUAlert(level: .warning, text: "Temperatura alta: \(deg(c)) °C."))
            }
        }
        if let m = s.temp_mem_c.map({ whole($0) }), hot(readings, \.temp_mem_c, 95) {
            out.append(GPUAlert(level: .warning, text: "La memoria de vídeo está a \(deg(m)) °C."))
        }
        // stopped fans arrive as an empty list (the agent lists the fans up to the last one that turns); nil is "no
        // reading", which says nothing about the fans. A fan that turns ends it at once; the core has the band.
        if stopped(s) == true, let c = s.temp_gpu_c.map({ whole($0) }), fansStoppedWarm(readings) {
            out.append(GPUAlert(level: .danger, text: "La GPU está a \(deg(c)) °C y los ventiladores no giran. Comprueba que nada los bloquee."))
        }
        // not in Ahorro (boost "off"): NVMetrics asks for no clocks there, so protection changes nothing. A line
        // without "boost" (an older agent) still shows it.
        if s.protect?.active == true && s.boost?.mode != "off" {
            var text = "Protección activa: NVMetrics no pide relojes máximos hasta que la tarjeta se enfríe."
            if reposo != true {
                text += " Con el modo reposo apagado el driver los sigue pidiendo; actívalo para que esto baje el consumo."
            }
            let menuText = reposo == true ? "Protección activa (detalles en la ventana)"
                                          : "Protección activa, sin efecto con reposo apagado"
            out.append(GPUAlert(level: .info, text: text, menuText: menuText))
        }
        return GPUAlert.Level.allCases.flatMap { level in out.filter { $0.level == level } }
    }

    /// true when the newest reading has a danger or a warning (the menu bar's "⚠︎"); reposo only changes an info text
    static func urgent(_ readings: [Sample]) -> Bool {
        list(readings, reposo: nil).contains { $0.level != .info }
    }

    /// whole degrees, rounded the way the rest of the app shows them
    private static func deg(_ c: Double) -> String { Fmt.num(c, 0) }

    /// the whole degrees the app shows (String(format: "%.0f") rounds halves to even), so each threshold judges the
    /// number on screen
    private static func whole(_ c: Double) -> Double { c.rounded(.toNearestOrEven) }

    /// true when v, in whole degrees, is at least limit; false without a value
    private static func reaches(_ v: Double?, _ limit: Double) -> Bool { v.map { whole($0) >= limit } ?? false }

    /// true when v, in whole degrees, is below limit; false without a value
    private static func under(_ v: Double?, _ limit: Double) -> Bool { v.map { whole($0) < limit } ?? false }

    /// on from `limit`, off only below `limit - band`
    private static func hot(_ readings: [Sample], _ key: KeyPath<Sample, Double?>, _ limit: Double) -> Bool {
        latched(readings, on: { reaches($0[keyPath: key], limit) }, off: { under($0[keyPath: key], limit - band) })
    }

    /// every fan stopped with the core at 70 °C or more; off when a fan turns or the core is below 70 - band
    private static func fansStoppedWarm(_ readings: [Sample]) -> Bool {
        latched(readings, on: { stopped($0) == true && reaches($0.temp_gpu_c, 70) },
                off: { stopped($0) == false || under($0.temp_gpu_c, 70 - band) })
    }

    /// on when the newest reading that decides (meets on or off) meets on; readings that meet neither (inside the
    /// band, or without the value) keep what the earlier ones decided; off when none decides
    private static func latched(_ readings: [Sample], on: (Sample) -> Bool, off: (Sample) -> Bool) -> Bool {
        for s in readings.reversed() {
            if on(s) { return true }
            if off(s) { return false }
        }
        return false
    }

    /// true: a valid fan reading with every fan stopped (an empty list); false: a fan turns; nil: no fan reading
    private static func stopped(_ s: Sample) -> Bool? {
        s.fan_rpm.map { rpm in !rpm.contains(where: { $0 > 0 }) }
    }
}
