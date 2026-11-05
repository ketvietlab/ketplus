import AppKit
import ApplicationServices
import ComputerProtocol

/// Connected displays and the agent's work display (ADR 0032).
enum Displays {
    static func all() -> [DisplayInfo] {
        let primaryHeight = NSScreen.screens.first?.frame.height ?? 0
        let mainID = CGMainDisplayID()
        return NSScreen.screens.compactMap { screen in
            guard let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else {
                return nil
            }
            let id = number.uint32Value
            let visible = screen.visibleFrame
            return DisplayInfo(id: id, name: screen.localizedName, bounds: CGDisplayBounds(id),
                               visibleFrame: CGRect(x: visible.minX, y: primaryHeight - visible.maxY,
                                                    width: visible.width, height: visible.height),
                               isMain: id == mainID)
        }
    }

    static func work(_ preference: DisplayPreference) -> DisplayInfo? {
        WorkDisplay.choose(all(), preference: preference)
    }

    static func screen(_ display: DisplayInfo) -> NSScreen? {
        NSScreen.screens.first {
            ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value == display.id
        }
    }

    /// Moves an app's unminimized windows, frameless ones included, onto the display.
    /// Returns how many moved; windows already there stay put.
    /// Windows of the app that count for placement and already lie on the display.
    static func windowsOn(_ display: DisplayInfo, of pid: pid_t) -> Int {
        movableFrames(of: pid).filter { WorkDisplay.isOn($0.frame, display) }.count
    }

    /// `startIndex` continues the cascade after windows already on the display, so a
    /// second app does not land exactly on the first.
    @discardableResult
    static func moveWindows(of pid: pid_t, to display: DisplayInfo, startIndex: Int = 0) -> Int {
        var moved = 0
        for (window, frame) in movableFrames(of: pid) {
            guard !WorkDisplay.isOn(frame, display) else { continue }
            let target = WorkDisplay.place(frame, on: display, index: startIndex + moved)
            var origin = target.origin
            var size = target.size
            if let position = AXValueCreate(.cgPoint, &origin),
               AXUIElementSetAttributeValue(window, kAXPositionAttribute as CFString, position) == .success {
                if target.size != frame.size, let extent = AXValueCreate(.cgSize, &size) {
                    AXUIElementSetAttributeValue(window, kAXSizeAttribute as CFString, extent)
                }
                moved += 1
            }
        }
        return moved
    }

    /// Raises the app's window with this frame above other apps' windows without
    /// activating the app, so that it repaints.
    static func raiseWindow(of pid: pid_t, frame: CGRect) -> Bool {
        guard let match = movableFrames(of: pid).first(where: {
            abs($0.frame.minX - frame.minX) < 2 && abs($0.frame.minY - frame.minY) < 2
                && abs($0.frame.width - frame.width) < 2 && abs($0.frame.height - frame.height) < 2
        }) else { return false }
        return AXUIElementPerformAction(match.window, kAXRaiseAction as CFString) == .success
    }

    private static func movableFrames(of pid: pid_t) -> [(window: AXUIElement, frame: CGRect)] {
        let app = AXUIElementCreateApplication(pid)
        AXUIElementSetMessagingTimeout(app, 1.5)
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(app, kAXWindowsAttribute as CFString, &value) == .success,
              let windows = value as? [AXUIElement] else { return [] }
        return windows.compactMap { window in
            guard let frame = frame(window), !bool(window, kAXMinimizedAttribute),
                  WorkDisplay.isMovable(role: string(window, kAXRoleAttribute),
                                        subrole: string(window, kAXSubroleAttribute), size: frame.size)
            else { return nil }
            return (window, frame)
        }
    }

    private static func string(_ element: AXUIElement, _ name: String) -> String? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success else { return nil }
        return value as? String
    }

    private static func bool(_ element: AXUIElement, _ name: String) -> Bool {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success else { return false }
        return value as? Bool ?? false
    }

    private static func frame(_ element: AXUIElement) -> CGRect? {
        var position: CFTypeRef?
        var size: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, kAXPositionAttribute as CFString, &position) == .success,
              AXUIElementCopyAttributeValue(element, kAXSizeAttribute as CFString, &size) == .success,
              let position, let size, CFGetTypeID(position) == AXValueGetTypeID(),
              CFGetTypeID(size) == AXValueGetTypeID() else { return nil }
        var origin = CGPoint.zero
        var extent = CGSize.zero
        guard AXValueGetValue(position as! AXValue, .cgPoint, &origin),
              AXValueGetValue(size as! AXValue, .cgSize, &extent) else { return nil }
        return CGRect(origin: origin, size: extent)
    }
}
