import ComputerProtocol
import CoreGraphics
import Foundation

enum InputError: Error, CustomStringConvertible {
    case eventCreationFailed

    var description: String { "The input event could not be created." }
}

enum MouseButton {
    case left, right, middle

    var cgButton: CGMouseButton {
        switch self {
        case .left: return .left
        case .right: return .right
        case .middle: return .center
        }
    }

    var downType: CGEventType {
        switch self {
        case .left: return .leftMouseDown
        case .right: return .rightMouseDown
        case .middle: return .otherMouseDown
        }
    }

    var upType: CGEventType {
        switch self {
        case .left: return .leftMouseUp
        case .right: return .rightMouseUp
        case .middle: return .otherMouseUp
        }
    }
}

enum ScrollDirection: String {
    case up, down, left, right
}

/// Posts synthetic HID events. Every event carries this process's PID, which the
/// kill switch uses to tell agent input from the user's own Esc.
enum InputControl {
    private static let pause: useconds_t = 30_000

    static func click(_ button: MouseButton, count: Int, at point: CGPoint, modifiers: KeyCombo.Modifiers) throws {
        try move(to: point)
        let flags = cgFlags(modifiers)
        for index in 1...count {
            let down = try mouseEvent(button.downType, at: point, button: button.cgButton)
            let up = try mouseEvent(button.upType, at: point, button: button.cgButton)
            for event in [down, up] {
                event.setIntegerValueField(.mouseEventClickState, value: Int64(index))
                event.flags = flags
            }
            down.post(tap: .cghidEventTap)
            usleep(pause)
            up.post(tap: .cghidEventTap)
            usleep(pause)
        }
    }

    static func move(to point: CGPoint) throws {
        try mouseEvent(.mouseMoved, at: point, button: .left).post(tap: .cghidEventTap)
        usleep(pause)
    }

    static func drag(from start: CGPoint, to end: CGPoint, modifiers: KeyCombo.Modifiers) throws {
        let flags = cgFlags(modifiers)
        try move(to: start)
        let down = try mouseEvent(.leftMouseDown, at: start, button: .left)
        down.flags = flags
        down.post(tap: .cghidEventTap)
        usleep(pause)
        let steps = 12
        for step in 1...steps {
            let t = Double(step) / Double(steps)
            let point = CGPoint(x: start.x + (end.x - start.x) * t, y: start.y + (end.y - start.y) * t)
            let dragged = try mouseEvent(.leftMouseDragged, at: point, button: .left)
            dragged.flags = flags
            dragged.post(tap: .cghidEventTap)
            usleep(15_000)
        }
        let up = try mouseEvent(.leftMouseUp, at: end, button: .left)
        up.flags = flags
        up.post(tap: .cghidEventTap)
    }

    static func scroll(_ direction: ScrollDirection, amount: Int, at point: CGPoint?, modifiers: KeyCombo.Modifiers) throws {
        if let point { try move(to: point) }
        let vertical: Int32 = direction == .up ? 1 : direction == .down ? -1 : 0
        let horizontal: Int32 = direction == .left ? 1 : direction == .right ? -1 : 0
        for _ in 0..<amount {
            guard let event = CGEvent(scrollWheelEvent2Source: source(), units: .line, wheelCount: 2,
                                      wheel1: vertical * 3, wheel2: horizontal * 3, wheel3: 0)
            else { throw InputError.eventCreationFailed }
            event.flags = cgFlags(modifiers)
            event.post(tap: .cghidEventTap)
            usleep(pause)
        }
    }

    /// Types literal Unicode text, independent of the keyboard layout.
    static func type(_ text: String) throws {
        let units = Array(text.utf16)
        var index = 0
        while index < units.count {
            let chunk = Array(units[index..<min(index + 16, units.count)])
            guard let down = CGEvent(keyboardEventSource: source(), virtualKey: 0, keyDown: true),
                  let up = CGEvent(keyboardEventSource: source(), virtualKey: 0, keyDown: false)
            else { throw InputError.eventCreationFailed }
            chunk.withUnsafeBufferPointer { buffer in
                down.keyboardSetUnicodeString(stringLength: buffer.count, unicodeString: buffer.baseAddress)
                up.keyboardSetUnicodeString(stringLength: buffer.count, unicodeString: buffer.baseAddress)
            }
            down.post(tap: .cghidEventTap)
            up.post(tap: .cghidEventTap)
            usleep(pause)
            index += chunk.count
        }
    }

    static func press(_ combo: KeyCombo, repeat count: Int) throws {
        guard let code = combo.keyCode else { return }
        for _ in 0..<count {
            guard let down = CGEvent(keyboardEventSource: source(), virtualKey: code, keyDown: true),
                  let up = CGEvent(keyboardEventSource: source(), virtualKey: code, keyDown: false)
            else { throw InputError.eventCreationFailed }
            down.flags = cgFlags(combo.modifiers)
            up.flags = cgFlags(combo.modifiers)
            down.post(tap: .cghidEventTap)
            usleep(pause)
            up.post(tap: .cghidEventTap)
            usleep(pause)
        }
    }

    static var cursorLocation: CGPoint { CGEvent(source: nil)?.location ?? .zero }

    private static func source() -> CGEventSource? { CGEventSource(stateID: .hidSystemState) }

    private static func mouseEvent(_ type: CGEventType, at point: CGPoint, button: CGMouseButton) throws -> CGEvent {
        guard let event = CGEvent(mouseEventSource: source(), mouseType: type,
                                  mouseCursorPosition: point, mouseButton: button)
        else { throw InputError.eventCreationFailed }
        return event
    }

    private static func cgFlags(_ modifiers: KeyCombo.Modifiers) -> CGEventFlags {
        var flags: CGEventFlags = []
        if modifiers.contains(.shift) { flags.insert(.maskShift) }
        if modifiers.contains(.control) { flags.insert(.maskControl) }
        if modifiers.contains(.option) { flags.insert(.maskAlternate) }
        if modifiers.contains(.command) { flags.insert(.maskCommand) }
        if modifiers.contains(.function) { flags.insert(.maskSecondaryFn) }
        return flags
    }
}
