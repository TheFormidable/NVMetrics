// Colours of Isaac's mock-ups: dark with a green glow when macOS is in dark mode, white with green accents in light mode.
import SwiftUI

struct Theme {
    let scheme: ColorScheme
    var dark: Bool { scheme == .dark }

    static let green = Color(red: 0.46, green: 0.85, blue: 0.0)
    static let greenDim = Color(red: 0.30, green: 0.58, blue: 0.0)
    static let warn = Color(red: 1.0, green: 0.72, blue: 0.0)
    static let hot = Color(red: 1.0, green: 0.30, blue: 0.22)

    var background: LinearGradient {
        dark ? LinearGradient(colors: [Color(white: 0.11), Color(white: 0.05)], startPoint: .top, endPoint: .bottom)
             : LinearGradient(colors: [Color(white: 0.98), Color(white: 0.91)], startPoint: .top, endPoint: .bottom)
    }
    var panel: Color { dark ? Color(white: 0.14) : Color.white }
    var panelEdge: Color { dark ? Color(white: 0.24) : Color(white: 0.82) }
    var track: Color { dark ? Color(white: 0.20) : Color(white: 0.86) }
    var text: Color { dark ? Color(white: 0.93) : Color(white: 0.10) }
    var label: Color { dark ? Color(white: 0.62) : Color(white: 0.38) }
    var glow: Color { Theme.green.opacity(dark ? 0.55 : 0.30) }

    /// green below 70 °C, amber to 83 °C, red above
    static func tempColor(_ c: Double?) -> Color {
        guard let c else { return green }
        return c < 70 ? green : c < 83 ? warn : hot
    }
}
