// NVMetrics: a menu-bar monitor for NVIDIA cards on the NullMoth driver. The menu bar shows the GPU temperature; the
// window shows clocks, power, temperatures, fan, VRAM, the two switches the driver really has (reposo, eco), when to
// ask the GPU for full clocks (boost, held by the agent only while the app runs) and the 3.0 driver's health counters.
// It warns when the card runs hot or its fans stand still on a warm GPU (Alerts.swift), with "⚠︎" in the menu bar.
// It follows macOS light / dark mode. All the driver work is in nvmetrics-agent and nvmetrics-setting.sh.
import SwiftUI
import AppKit

@main
struct NVMetricsApp: App {
    @StateObject private var agent = Agent()
    @StateObject private var controls = Controls()
    @StateObject private var fanGuard = FanGuardUIModel()
    @StateObject private var recorder = AutoCurveRecorder()
    /// what the label in the bar and its menu show: the agent's readings, held still while a menu is open
    @StateObject private var menu = MenuHold(MenuLines())

    var body: some Scene {
        MenuBarExtra {
            MenuContent(agent: agent).environmentObject(menu).environmentObject(fanGuard)
                .environmentObject(agent).environmentObject(controls)
        } label: {
            let lines = menu.shown
            let mark = lines.urgent ? "\u{26A0}\u{FE0E} " : ""    // ⚠︎ in its text form, for a danger or a warning
            let temp = lines.sample?.temp_gpu_c.map { String(format: "%.0f°", $0) } ?? "GPU --"
            Text(mark + temp)
                .monospacedDigit()
                // the menu's protection line depends on reposo, so it is read at launch too, not only with the window
                .onAppear {
                    agent.start(); controls.refresh()
                    recorder.start(agent: agent, controls: controls, fanGuard: fanGuard)
                    menu.start(agent: agent, controls: controls)
                }
        }
        Window("NVMetrics", id: "main") {
            MainView()
                .environmentObject(agent)
                .environmentObject(controls).environmentObject(fanGuard)
                .onAppear {
                    agent.start(); recorder.start(agent: agent, controls: controls, fanGuard: fanGuard)
                    menu.start(agent: agent, controls: controls)
                }
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: min(780, NSScreen.main?.visibleFrame.width ?? 780),
                     height: min(680, (NSScreen.main?.visibleFrame.height ?? 720) - 40))
        .windowResizability(.contentMinSize)
        Window("Comportamiento AUTO", id: "firmware-curve") {
            FirmwareCurveView().environmentObject(recorder)
        }.windowResizability(.contentSize)
    }
}
