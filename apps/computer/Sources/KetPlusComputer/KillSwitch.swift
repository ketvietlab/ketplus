import CoreGraphics
import Foundation

/// Global Esc while a conversation holds the control lease. The user's Esc is
/// consumed, so on-screen content cannot use it to dismiss a dialog. Esc posted
/// by this agent (the `key` tool) carries its PID and passes through.
final class KillSwitch {
    private var tap: CFMachPort?
    private var source: CFRunLoopSource?
    private let onEscape: () -> Void
    private let isArmed: () -> Bool

    init(isArmed: @escaping () -> Bool, onEscape: @escaping () -> Void) {
        self.isArmed = isArmed
        self.onEscape = onEscape
    }

    /// Installs the tap on the current (main) run loop. Needs Accessibility.
    @discardableResult
    func install() -> Bool {
        if tap != nil { return true }
        let mask = CGEventMask(1 << CGEventType.keyDown.rawValue)
        let context = Unmanaged.passUnretained(self).toOpaque()
        guard let tap = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap,
                                          options: .defaultTap, eventsOfInterest: mask,
                                          callback: killSwitchCallback, userInfo: context)
        else { return false }
        self.tap = tap
        source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: tap, enable: true)
        return true
    }

    fileprivate func handle(_ type: CGEventType, _ event: CGEvent) -> Unmanaged<CGEvent>? {
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let tap { CGEvent.tapEnable(tap: tap, enable: true) }
            return Unmanaged.passUnretained(event)
        }
        let escape: Int64 = 53
        guard type == .keyDown, event.getIntegerValueField(.keyboardEventKeycode) == escape,
              event.getIntegerValueField(.eventSourceUnixProcessID) != Int64(getpid()),
              isArmed()
        else { return Unmanaged.passUnretained(event) }
        onEscape()
        return nil
    }
}

private func killSwitchCallback(proxy: CGEventTapProxy, type: CGEventType, event: CGEvent,
                                userInfo: UnsafeMutableRawPointer?) -> Unmanaged<CGEvent>? {
    guard let userInfo else { return Unmanaged.passUnretained(event) }
    return Unmanaged<KillSwitch>.fromOpaque(userInfo).takeUnretainedValue().handle(type, event)
}
