import AppKit
import CoreGraphics

/// Resolves which application receives input, so the agent can check the grant
/// and tier of the real target rather than what the model claims it is clicking.
enum WindowTargets {
    static let finder = "com.apple.finder"

    /// Bundle ID of the app whose window is topmost at a global point. The menu bar
    /// belongs to the frontmost app; the Dock and the bare desktop belong to Finder.
    /// Returns "" for windows of processes without a bundle (always blocked).
    static func application(at point: CGPoint) -> String {
        let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
        let windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] ?? []
        let ownPID = getpid()
        let menuBarLevel = Int(CGWindowLevelForKey(.mainMenuWindow))
        for window in windows {
            guard let pid = window[kCGWindowOwnerPID as String] as? pid_t, pid != ownPID,
                  let boundsInfo = window[kCGWindowBounds as String] as? [String: Any],
                  let bounds = CGRect(dictionaryRepresentation: boundsInfo as CFDictionary),
                  bounds.contains(point),
                  (window[kCGWindowAlpha as String] as? Double ?? 1) > 0
            else { continue }
            let layer = window[kCGWindowLayer as String] as? Int ?? 0
            let bundle = NSRunningApplication(processIdentifier: pid)?.bundleIdentifier ?? ""
            if bundle.isEmpty && layer == menuBarLevel { return frontmostApplication }
            if bundle == "com.apple.dock" {
                // The Dock keeps a transparent full-display window above apps; it takes no
                // clicks. Only its real tiles count, and they belong to Finder's grant.
                if coversDisplay(bounds) { continue }
                return finder
            }
            return bundle
        }
        return finder
    }

    private static func coversDisplay(_ bounds: CGRect) -> Bool {
        var count: UInt32 = 0
        var displays = [CGDirectDisplayID](repeating: 0, count: 16)
        guard CGGetActiveDisplayList(16, &displays, &count) == .success else { return false }
        return displays.prefix(Int(count)).contains { CGDisplayBounds($0).equalTo(bounds) }
    }

    static var frontmostApplication: String {
        NSWorkspace.shared.frontmostApplication?.bundleIdentifier ?? ""
    }
}
