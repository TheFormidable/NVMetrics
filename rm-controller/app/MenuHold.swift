// The menu-bar item's values, held still while a menu is open.
//
// A menu-style MenuBarExtra rebuilds the items of its NSMenu whenever a value its content reads changes. The telemetry
// lines change with every reading, once a second, and under an open menu each changed line is taken out and put back:
// the blue highlight moves by as many rows as lines changed, away from the pointer, and stays there until the mouse
// moves. So the menu and the label in the bar read their values from here, and this object leaves what it shows alone
// from the moment a menu opens until it closes. What arrives meanwhile is kept and shown when the menu closes. The
// window does not read from here and stays live.
//
// "A menu is open" is known in two ways, and either one is enough:
//  - AppKit's notifications that menu tracking began and ended;
//  - the main run loop's mode, which is the event-tracking mode for as long as a menu is tracked (the reason a timer
//    scheduled in the default mode stands still under an open menu). It is also the mode while a window is resized or
//    a slider is dragged, and then the bar's label waits for a moment too, which nobody sees.
// A value that waited with only the run loop's mode to go by is shown with the next reading after the menu closed.
import AppKit
import Combine

@MainActor
final class MenuHold<Value: Equatable>: ObservableObject {
    /// what the menu and the label in the bar show
    @Published private(set) var shown: Value
    /// the newest value offered while a menu was open, to show when it closes
    private var waiting: Value?
    /// between AppKit's "menu tracking began" and "ended"
    private var menuOpen = false
    private let tracking: () -> Bool
    private var links: Set<AnyCancellable> = []

    /// tracking: whether the main run loop is tracking events right now (a test passes its own answer)
    init(_ first: Value, tracking: @escaping () -> Bool = { RunLoop.main.currentMode == RunLoop.Mode.eventTracking }) {
        shown = first
        self.tracking = tracking
    }

    /// The newest value: shown at once, or, under an open menu, kept until the menu closes. An equal value publishes
    /// nothing, so the menu is not rebuilt for it.
    func offer(_ value: Value) {
        if menuOpen || tracking() {
            waiting = value
            return
        }
        waiting = nil
        if value != shown { shown = value }
    }

    func menuOpened() {
        menuOpen = true
    }

    /// A menu closed: show the newest value that arrived while it was open.
    func menuClosed() {
        menuOpen = false
        guard let value = waiting else { return }
        waiting = nil
        if value != shown { shown = value }
    }

    /// Offers `value()` now and again after each event of `changes`, and listens for menus opening and closing. Only
    /// the first call does anything. `changes` may fire before its change is in place (objectWillChange does), so the
    /// value is read on the main queue's next turn.
    func follow<P: Publisher>(_ changes: P, value: @escaping () -> Value) where P.Failure == Never {
        guard links.isEmpty else { return }
        changes
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.offer(value()) }
            .store(in: &links)
        NotificationCenter.default.publisher(for: NSMenu.didBeginTrackingNotification)
            .sink { [weak self] _ in self?.menuOpened() }
            .store(in: &links)
        NotificationCenter.default.publisher(for: NSMenu.didEndTrackingNotification)
            .sink { [weak self] _ in self?.menuClosed() }
            .store(in: &links)
        offer(value())
    }
}
