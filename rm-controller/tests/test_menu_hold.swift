import AppKit
import Combine

// The real MenuHold runs; only "is the run loop tracking events" is the test's own answer.
final class TrackingAnswer {
    var value = false
}

@main struct MenuHoldTests {
    @MainActor static func until(_ condition: () -> Bool) async {
        for _ in 0..<3000 {
            if condition() { return }
            try? await Task.sleep(nanoseconds: 1_000_000)
        }
        fatalError("menu hold test timed out")
    }

    @MainActor static func main() async {
        let tracking = TrackingAnswer()
        let hold = MenuHold(1, tracking: { tracking.value })
        var published = 0
        let link = hold.objectWillChange.sink { _ in published += 1 }

        // no menu open: a new value shows at once, an equal one publishes nothing
        hold.offer(2)
        precondition(hold.shown == 2 && published == 1)
        hold.offer(2)
        precondition(hold.shown == 2 && published == 1)

        // the run loop tracks events (a menu is open): values wait and nothing is published, so the menu's items stay
        tracking.value = true
        hold.offer(3)
        hold.offer(4)
        precondition(hold.shown == 2 && published == 1)
        // tracking over: the next value shows; the ones that waited are not shown after it
        tracking.value = false
        hold.offer(5)
        precondition(hold.shown == 5 && published == 2)
        hold.menuClosed()
        precondition(hold.shown == 5 && published == 2)

        // AppKit says a menu opened: values wait whatever the run loop says, and the newest shows on closing
        hold.menuOpened()
        hold.offer(6)
        hold.offer(7)
        precondition(hold.shown == 5 && published == 2)
        hold.menuClosed()
        precondition(hold.shown == 7 && published == 3)
        hold.menuClosed()
        precondition(hold.shown == 7 && published == 3)

        // a value that went back to the one showing while the menu was open publishes nothing on closing
        hold.menuOpened()
        hold.offer(8)
        hold.offer(7)
        hold.menuClosed()
        precondition(hold.shown == 7 && published == 3)

        // AppKit's "ended" arrives while the run loop still tracks: the value that waited shows all the same
        hold.menuOpened()
        tracking.value = true
        hold.offer(9)
        hold.menuClosed()
        precondition(hold.shown == 9 && published == 4)
        tracking.value = false
        link.cancel()

        // follow(): the value is read at once and again after each change, on the main queue's next turn
        let changes = PassthroughSubject<Void, Never>()
        var source = 10
        let followed = MenuHold(0, tracking: { tracking.value })
        followed.follow(changes, value: { source })
        precondition(followed.shown == 10)
        source = 11
        changes.send()
        precondition(followed.shown == 10)
        await until { followed.shown == 11 }
        // a second follow() does nothing
        followed.follow(changes, value: { 99 })
        precondition(followed.shown == 11)

        // AppKit's own notifications open and close the hold
        NotificationCenter.default.post(name: NSMenu.didBeginTrackingNotification, object: nil)
        source = 12
        changes.send()
        try? await Task.sleep(nanoseconds: 100_000_000)
        precondition(followed.shown == 11)
        NotificationCenter.default.post(name: NSMenu.didEndTrackingNotification, object: nil)
        precondition(followed.shown == 12)

        print("PASS: menu values hold still while a menu is open and catch up when it closes")
    }
}
