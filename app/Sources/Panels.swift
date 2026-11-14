// The two lower cards of the window: when the agent asks the GPU for full clocks (BoostMode), and the 3.0 driver's
// health counters. Neither needs a password.
import SwiftUI

/// "Rendimiento de la GPU": Ahorro / Automático / Máximo, and what the agent is doing right now.
struct BoostCard: View {
    @EnvironmentObject var agent: Agent
    @EnvironmentObject var controls: Controls
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        let th = Theme(scheme: scheme)
        Card(icon: "speedometer", title: "Rendimiento de la GPU") {
            Picker("", selection: $agent.boostMode) {
                ForEach(BoostMode.allCases, id: \.self) { m in Text(m.label).tag(m) }
            }
            .pickerStyle(.segmented).labelsHidden()
            Text(agent.boostMode.detail).font(.system(size: 11)).foregroundStyle(th.label)
                .fixedSize(horizontal: false, vertical: true)
            status(th)
            if controls.reposo != true {
                Text("Solo cambia algo con el modo reposo activado: sin él, el propio driver ya tiene la tarjeta a tope.")
                    .font(.system(size: 10)).foregroundStyle(th.text).fixedSize(horizontal: false, vertical: true)
            }
            Text("Se aplica al momento, sin contraseña, y sigue aunque cierres esta ventana: deja de pedirse al salir de NVMetrics (menú de la barra > Salir).")
                .font(.system(size: 10)).foregroundStyle(th.label).fixedSize(horizontal: false, vertical: true)
        }
    }

    /// from the latest sample's "boost" (nil: no reading yet)
    @ViewBuilder private func status(_ th: Theme) -> some View {
        let b = agent.latest?.boost
        if let e = b?.error {
            Text("El driver rechazó la petición (\(e))").font(.system(size: 12, weight: .semibold)).foregroundStyle(Theme.hot)
        } else if let held = b?.held {
            Text(held ? "Ahora: a tope" : "Ahora: la tarjeta decide (reposo si no hay carga)")
                .font(.system(size: 12, weight: .semibold)).foregroundStyle(held ? Theme.green : th.text)
        } else {
            Text("Ahora: --").font(.system(size: 12, weight: .semibold)).foregroundStyle(th.label)
        }
    }
}

/// "Salud del driver": counters the 3.0 driver keeps since the Mac started. Zero reads as fine, above zero in amber.
struct HealthCard: View {
    let health: Health?
    let hasReadings: Bool
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        let th = Theme(scheme: scheme)
        Card(icon: "stethoscope", title: "Salud del driver") {
            if let h = health {
                count(th, "Errores de la GPU (Xid)", h.xid_count, extra: h.xid_last.map { " · último: \($0)" } ?? "")
                count(th, "Apps rescatadas tras un fallo", h.chan_recovered)
                count(th, "Apps que perdieron la GPU", h.chan_gave_up)
                count(th, "Fallos del firmware", h.bugchecks)
                count(th, "Avisos internos del driver", h.rm_asserts)
                count(th, "Ajustes de arranque fuera de lo normal", h.switches_nondefault)
                if (h.switches_nondefault ?? 0) > 0, let sw = h.switches {
                    Text(sw).font(.system(size: 10, design: .monospaced)).foregroundStyle(th.label)
                        .fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
                }
                Text("Desde el último arranque").font(.system(size: 10)).foregroundStyle(th.label)
            } else {
                Text(hasReadings ? "Disponible con el driver 3.0." : "Sin lecturas.")
                    .font(.system(size: 12)).foregroundStyle(th.label)
            }
        }
    }

    private func count(_ th: Theme, _ name: String, _ v: Int?, extra: String = "") -> InfoLine {
        InfoLine(name: name, value: v.map { "\($0)\(extra)" } ?? "--", tint: (v ?? 0) > 0 ? Theme.warn : th.label)
    }
}
