// What the menu-bar item shows, as one value: the reading behind the label in the bar and the lines of its menu, the
// first alert, the reason there are no readings and the boost mode its submenu ticks. MenuHold keeps this value still
// while a menu is open (MenuHold.swift says why); the fan submenu is not part of it and keeps reading its own model.
import Foundation
import Combine

struct MenuLines: Equatable {
    var sample: Sample?
    /// the first alert, in its one-line menu form
    var alert: String?
    /// a danger or a warning: "⚠︎" before the temperature in the bar
    var urgent = false
    /// why there are no readings (the menu shows it only while there is none)
    var problem: String?
    var boost = BoostMode.auto

    /// the values as they are now
    @MainActor static func now(_ agent: Agent, _ controls: Controls) -> MenuLines {
        let readings = agent.alertReadings
        var lines = MenuLines()
        lines.sample = agent.latest
        lines.alert = Alerts.list(readings, reposo: controls.reposo).first?.menuText
        lines.urgent = Alerts.urgent(readings)
        lines.problem = agent.problem
        lines.boost = agent.boostMode
        return lines
    }
}

extension MenuHold where Value == MenuLines {
    /// Starts following the agent's readings and the two switches (reposo changes the protection alert's text). Only
    /// the first call does anything.
    func start(agent: Agent, controls: Controls) {
        follow(agent.objectWillChange.merge(with: controls.objectWillChange)) { MenuLines.now(agent, controls) }
    }
}
