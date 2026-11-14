// NVMetrics: a menu-bar monitor for NVIDIA cards on the NullMoth driver. The menu bar shows the GPU temperature; the
// window shows clocks, power, temperatures, fan, VRAM, the two switches the driver really has (reposo, eco), when to
// ask the GPU for full clocks (boost, held by the agent only while the app runs) and the 3.0 driver's health counters.
// It warns when the card runs hot or its fans stand still on a warm GPU (Alerts.swift), with "⚠︎" in the menu bar.
// It follows macOS light / dark mode. All the driver work is in nvmetrics-agent and nvmetrics-setting.sh.
import SwiftUI

@main
struct NVMetricsApp: App {
    @StateObject private var agent = Agent()
    @StateObject private var controls = Controls()

    var body: some Scene {
        MenuBarExtra {
            MenuContent().environmentObject(agent).environmentObject(controls)
        } label: {
            let s = agent.latest
            let mark = Alerts.urgent(agent.alertReadings) ? "\u{26A0}\u{FE0E} " : ""    // ⚠︎ in its text form, for a danger or a warning
            let temp = s?.temp_gpu_c.map { String(format: "%.0f°", $0) } ?? "GPU --"
            Text(mark + temp)
                .monospacedDigit()
                // the menu's protection line depends on reposo, so it is read at launch too, not only with the window
                .onAppear { agent.start(); controls.refresh() }
        }
        Window("NVMetrics", id: "main") {
            MainView()
                .environmentObject(agent)
                .environmentObject(controls)
                .onAppear { agent.start() }
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentSize)
    }
}
