import Foundation
import IOKit.pwr_mgt

/// Keeps the display awake while live video is on screen.
///
/// Playing with a game controller generates no Mac input events, so without
/// this the display would dim and sleep in the middle of a game.
@MainActor
final class DisplaySleepBlocker {
    private var assertionID: IOPMAssertionID = 0
    private var isActive = false

    func setActive(_ active: Bool) {
        guard active != isActive else { return }
        if active {
            let result = IOPMAssertionCreateWithName(
                kIOPMAssertionTypePreventUserIdleDisplaySleep as CFString,
                IOPMAssertionLevel(kIOPMAssertionLevelOn),
                "CapView is showing live capture video" as CFString,
                &assertionID
            )
            isActive = result == kIOReturnSuccess
        } else {
            IOPMAssertionRelease(assertionID)
            assertionID = 0
            isActive = false
        }
    }
}
