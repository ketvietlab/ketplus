import AppKit
import ComputerProtocol
import Foundation

/// The work display (ADR 0028): screenshots default to it and granted apps' windows
/// move there, so the agent stays off the user's main display.
extension AgentTools {
    /// Reads computer-tools.json on every call, so Settings changes apply at once.
    func setting(_ key: String) -> Any? {
        guard let data = try? Data(contentsOf: paths.settingsFile),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return object[key]
    }

    func workDisplay() -> DisplayInfo? {
        let preference = DisplayPreference(setting: setting("display") as? String)
        return onMain { Displays.work(preference) }
    }

    func allDisplays() -> [DisplayInfo] { onMain { Displays.all() } }

    /// The display a `display` tool argument names: work (default), main or an ID.
    func display(for argument: Any?) throws -> DisplayInfo {
        let text = (argument as? String) ?? (argument as? NSNumber)?.stringValue
        guard let display = WorkDisplay.resolve(text, displays: allDisplays(), work: workDisplay()) else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "display must be work, main or an ID from list_apps.")
        }
        return display
    }

    /// The display a screenshot geometry was taken on.
    func display(of geometry: ScreenGeometry) -> DisplayInfo? {
        allDisplays().first { $0.bounds == geometry.displayBounds }
    }

    /// Moves the windows of running apps onto the work display, cascading after the
    /// conversation's granted windows already there. Returns the apps that moved.
    func moveToWorkDisplay(_ bundles: [String], conversation: String) -> [String] {
        guard let work = workDisplay() else { return [] }
        var placed = control.grantedApps(conversation).subtracting(bundles)
            .compactMap(WindowTargets.pid(of:))
            .reduce(0) { $0 + Displays.windowsOn(work, of: $1) }
        return bundles.filter { bundle in
            guard let pid = WindowTargets.pid(of: bundle) else { return false }
            let moved = Displays.moveWindows(of: pid, to: work, startIndex: placed)
            placed += moved
            return moved > 0
        }
    }

    /// Opens or reveals a granted app on the work display. A launched app gets a few
    /// seconds to show its first window.
    func openOnWorkDisplay(_ bundle: String, activate: Bool, conversation: String) throws -> Bool {
        let wasRunning = WindowTargets.pid(of: bundle) != nil
        if wasRunning {
            if activate {
                _ = onMain { NSRunningApplication.runningApplications(withBundleIdentifier: bundle).first?.activate() }
            }
        } else {
            try AppCatalog.open(bundle, activate: activate)
        }
        for attempt in 0..<(wasRunning ? 1 : 15) {
            if attempt > 0 { usleep(200_000) }
            if !moveToWorkDisplay([bundle], conversation: conversation).isEmpty { return true }
        }
        return false
    }

    func displaySummary(_ display: DisplayInfo, work: DisplayInfo?) -> [String: Any] {
        ["id": Int(display.id), "name": display.name, "main": display.isMain, "work": display.id == work?.id,
         "x": Int(display.bounds.minX), "y": Int(display.bounds.minY),
         "width": Int(display.bounds.width), "height": Int(display.bounds.height)]
    }

    func onMain<T>(_ body: @MainActor () -> T) -> T {
        if Thread.isMainThread { return MainActor.assumeIsolated(body) }
        return DispatchQueue.main.sync { MainActor.assumeIsolated(body) }
    }
}
