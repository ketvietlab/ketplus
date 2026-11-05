import AppKit
import ComputerProtocol

/// Running applications and launching them, for list_apps / open_application.
enum AppCatalog {
    struct Entry {
        let name: String
        let bundleIdentifier: String
        let frontmost: Bool
    }

    static func running() -> [Entry] {
        let front = NSWorkspace.shared.frontmostApplication?.bundleIdentifier
        return NSWorkspace.shared.runningApplications
            .filter { $0.activationPolicy == .regular }
            .compactMap { app in
                guard let id = app.bundleIdentifier else { return nil }
                return Entry(name: app.localizedName ?? id, bundleIdentifier: id, frontmost: id == front)
            }
            .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    /// Matches a bundle ID exactly, or a running or installed app name case-insensitively.
    static func resolve(_ query: String) -> (name: String, bundleIdentifier: String)? {
        let trimmed = query.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return nil }
        if let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: trimmed) {
            return (displayName(url) ?? trimmed, trimmed)
        }
        if let app = running().first(where: {
            $0.bundleIdentifier == trimmed || $0.name.caseInsensitiveCompare(trimmed) == .orderedSame
        }) {
            return (app.name, app.bundleIdentifier)
        }
        for folder in ["/Applications", "/System/Applications", "/System/Applications/Utilities",
                       NSHomeDirectory() + "/Applications"] {
            let url = URL(fileURLWithPath: folder).appendingPathComponent(trimmed + ".app")
            if let id = Bundle(url: url)?.bundleIdentifier { return (displayName(url) ?? trimmed, id) }
        }
        return nil
    }

    /// Launches an app, bringing it to the front only when `activate` is set.
    static func open(_ bundleIdentifier: String, activate: Bool) throws {
        guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: bundleIdentifier) else {
            throw AgentFailure(code: "APP_NOT_FOUND", message: "No installed application has ID \(bundleIdentifier).")
        }
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.activates = activate
        let done = DispatchSemaphore(value: 0)
        var failure: Error?
        NSWorkspace.shared.openApplication(at: url, configuration: configuration) { _, error in
            failure = error
            done.signal()
        }
        if done.wait(timeout: .now() + 15) == .timedOut {
            throw AgentFailure(code: "APP_LAUNCH_TIMEOUT", message: "\(bundleIdentifier) did not open within 15 seconds.")
        }
        if let failure { throw AgentFailure(code: "APP_LAUNCH_FAILED", message: failure.localizedDescription) }
    }

    private static func displayName(_ url: URL) -> String? {
        FileManager.default.displayName(atPath: url.path).replacingOccurrences(of: ".app", with: "")
    }
}
