import AppKit
import ApplicationServices
import ComputerProtocol

/// Connected displays and the agent's work display (ADR 0028).
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

    /// Moves an app's standard, unminimized windows onto the display. Returns how
    /// many moved; windows already there stay put.
    @discardableResult
    static func moveWindows(of pid: pid_t, to display: DisplayInfo) -> Int {
        let app = AXUIElementCreateApplication(pid)
        AXUIElementSetMessagingTimeout(app, 1.5)
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(app, kAXWindowsAttribute as CFString, &value) == .success,
              let windows = value as? [AXUIElement] else { return 0 }
        var moved = 0
        for window in windows {
            guard string(window, kAXSubroleAttribute) == kAXStandardWindowSubrole,
                  !bool(window, kAXMinimizedAttribute), let frame = frame(window),
                  !WorkDisplay.isOn(frame, display) else { continue }
            let target = WorkDisplay.place(frame, on: display, index: moved)
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
