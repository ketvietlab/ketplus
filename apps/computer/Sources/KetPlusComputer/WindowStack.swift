import AppKit
import ComputerProtocol
import CoreGraphics

/// The on-screen window order, front to back (ADR 0032).
enum WindowStack {
    struct Hidden {
        let pid: pid_t
        let bundle: String
        let frame: CGRect
    }

    /// Granted windows on the display that other apps' windows hide, front to back.
    static func hiddenGranted(_ granted: Set<String>, on display: DisplayInfo) -> [Hidden] {
        guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements],
                                                    kCGNullWindowID) as? [[String: Any]] else { return [] }
        let ownPid = ProcessInfo.processInfo.processIdentifier
        var above: [CGRect] = []
        var hidden: [Hidden] = []
        for window in list {
            guard window[kCGWindowLayer as String] as? Int == 0,
                  let pid = window[kCGWindowOwnerPID as String] as? pid_t, pid != ownPid,
                  let bounds = window[kCGWindowBounds as String] as? NSDictionary,
                  let frame = CGRect(dictionaryRepresentation: bounds), frame.intersects(display.bounds)
            else { continue }
            let bundle = NSRunningApplication(processIdentifier: pid)?.bundleIdentifier ?? ""
            if granted.contains(bundle) {
                if Occlusion.coveredFraction(frame.intersection(display.bounds), above: above) >= Occlusion.hiddenFraction {
                    hidden.append(Hidden(pid: pid, bundle: bundle, frame: frame))
                }
            } else {
                above.append(frame)
            }
        }
        return hidden
    }
}
