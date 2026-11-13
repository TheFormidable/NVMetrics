// NVMetrics: a menu-bar monitor for NVIDIA cards on the NullMoth driver. The menu bar shows the GPU temperature; the
// window shows clocks, power, temperatures, fan, VRAM and the two switches the driver really has (reposo, eco).
// It follows macOS light / dark mode. All the driver work is in nvmetrics-agent and nvmetrics-setting.sh.
import SwiftUI

@main
struct NVMetricsApp: App {
    @StateObject private var agent = Agent()
    @StateObject private var controls = Controls()

    var body: some Scene {
        MenuBarExtra {
            MenuContent().environmentObject(agent)
        } label: {
            let t = agent.latest?.temp_gpu_c
            Text(t.map { String(format: "%.0f°", $0) } ?? "GPU --")
                .monospacedDigit()
                .onAppear { agent.start() }
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
