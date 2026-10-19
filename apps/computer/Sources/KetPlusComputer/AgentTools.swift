import AppKit
import Carbon.HIToolbox
import ComputerProtocol
import Foundation

/// Tool semantics and policy enforcement (ADR 0026). Runs on socket threads;
/// UI work hops to the main thread.
final class AgentTools {
    private let paths: AgentPaths
    private let control: ControlState
    private let audit: AuditLog
    private let lock = NSLock()
    private var geometries: [String: ScreenGeometry] = [:]
    private let indicator: ControlIndicator
    private var killSwitch: KillSwitch?

    init(paths: AgentPaths, control: ControlState, indicator: ControlIndicator) {
        self.paths = paths
        self.control = control
        self.indicator = indicator
        audit = AuditLog(paths: paths)
    }

    // MARK: lifecycle hooks (main thread)

    @MainActor
    func installKillSwitch() {
        guard killSwitch == nil, Permissions.accessibility else { return }
        let killSwitch = KillSwitch(isArmed: { [control] in control.activeHolder != nil }) { [weak self] in
            MainActor.assumeIsolated { self?.userPressedEscape() }
        }
        if killSwitch.install() { self.killSwitch = killSwitch }
    }

    @MainActor
    func expireIdleLease() {
        if control.expireIdleLease() { indicator.hide() }
    }

    @MainActor
    private func userPressedEscape() {
        if let stopped = control.cancelActive() {
            audit.record(conversation: stopped, tool: "esc", target: nil, outcome: "cancelled_by_user")
        }
        indicator.hide()
        if NSApp.modalWindow != nil { NSApp.abortModal() }
    }

    // MARK: dispatch

    func status() -> [String: Any] {
        [
            "protocol": AgentPaths.protocolVersion,
            "bundleIdentifier": Bundle.main.bundleIdentifier ?? "",
            "version": Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "",
            "pid": Int(getpid()),
            "parentPid": Int(getppid()),
            "screenRecording": Permissions.screenRecording,
            "accessibility": Permissions.accessibility,
            "controlling": control.activeHolder != nil,
            "killSwitch": killSwitch != nil,
        ]
    }

    func call(_ method: String, _ params: [String: Any], conversation: String) throws -> [String: Any] {
        do {
            let result = try perform(method, params, conversation)
            audit.record(conversation: conversation, tool: method, target: result.target, outcome: "ok",
                         detail: method == "type" ? ["characters": (params["text"] as? String)?.count ?? 0] : [:])
            return result.value
        } catch let failure as AgentFailure {
            audit.record(conversation: conversation, tool: method, target: nil, outcome: failure.code)
            throw failure
        }
    }

    private func perform(_ method: String, _ params: [String: Any], _ conversation: String) throws
        -> (value: [String: Any], target: String?) {
        switch method {
        case "list_apps": return (listApps(conversation), nil)
        case "cursor_position": return (cursorPosition(conversation), nil)
        default: break
        }
        if method != "request_access" && control.isCancelled(conversation) {
            throw AgentFailure(code: "CANCELLED",
                               message: "The user pressed Esc to stop computer use. Stop and ask the user before continuing.")
        }
        try takeControl(conversation)
        switch method {
        case "request_access": return (try requestAccess(params, conversation), nil)
        case "open_application": return try openApplication(params, conversation)
        case "screenshot": return (try screenshot(conversation, region: nil), nil)
        case "zoom": return (try zoom(params, conversation), nil)
        case "left_click", "right_click", "middle_click", "double_click", "triple_click":
            return try click(method, params, conversation)
        case "mouse_move":
            let (point, app) = try pointerTarget(params["coordinate"], conversation, required: true)
            try InputControl.move(to: point)
            return (["ok": true], app)
        case "left_click_drag": return try drag(params, conversation)
        case "scroll": return try scroll(params, conversation)
        case "type", "key": return try keyboard(method, params, conversation)
        default: throw AgentFailure(code: "UNKNOWN_METHOD", message: "Unknown method \(method).")
        }
    }

    // MARK: access

    private func takeControl(_ conversation: String) throws {
        do {
            try control.acquire(conversation)
        } catch ControlState.Failure.locked {
            throw AgentFailure(code: "CONTROL_LOCKED",
                               message: "Another KetPlus conversation is controlling this Mac. Try again later.")
        }
        let names = control.grantedApps(conversation).compactMap { id in
            AppCatalog.running().first { $0.bundleIdentifier == id }?.name
        }
        DispatchQueue.main.async { [self] in
            MainActor.assumeIsolated {
                installKillSwitch()
                indicator.show(apps: names.sorted())
            }
        }
    }

    private func deniedApps() -> Set<String> {
        guard let data = try? Data(contentsOf: paths.settingsFile),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let list = object["deniedApps"] as? [String] else { return [] }
        return Set(list)
    }

    private func tier(_ bundle: String) -> AppTier { AppPolicy.tier(for: bundle, denied: deniedApps()) }

    private func listApps(_ conversation: String) -> [String: Any] {
        let granted = control.grantedApps(conversation)
        return ["apps": AppCatalog.running().map { app in
            ["name": app.name, "bundleId": app.bundleIdentifier, "tier": tier(app.bundleIdentifier).rawValue,
             "granted": granted.contains(app.bundleIdentifier), "frontmost": app.frontmost] as [String: Any]
        }]
    }

    private func requestAccess(_ params: [String: Any], _ conversation: String) throws -> [String: Any] {
        guard let queries = params["apps"] as? [String], !queries.isEmpty, queries.count <= 8 else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "apps must list 1 to 8 application names or bundle IDs.")
        }
        var wanted: [(name: String, bundleIdentifier: String)] = []
        var refused: [String] = []
        let granted = control.grantedApps(conversation)
        for query in queries {
            guard let app = AppCatalog.resolve(query) else {
                throw AgentFailure(code: "APP_NOT_FOUND", message: "No application matches \"\(query)\".")
            }
            if tier(app.bundleIdentifier) == .blocked { refused.append(app.name); continue }
            if !granted.contains(app.bundleIdentifier) && !wanted.contains(where: { $0.bundleIdentifier == app.bundleIdentifier }) {
                wanted.append(app)
            }
        }
        var approved = false
        if !wanted.isEmpty {
            guard Permissions.screenRecording, Permissions.accessibility else {
                throw AgentFailure(code: "PERMISSION_REQUIRED",
                                   message: "KetPlus Computer needs Screen Recording and Accessibility in System Settings > Privacy & Security.")
            }
            let request = ApprovalPanel.Request(conversation: conversation, apps: wanted,
                                                reason: String((params["reason"] as? String ?? "").prefix(300)))
            approved = DispatchQueue.main.sync { MainActor.assumeIsolated { ApprovalPanel.ask(request) } }
            if approved { control.grant(Set(wanted.map(\.bundleIdentifier)), to: conversation) }
        }
        return ["granted": control.grantedApps(conversation).sorted(),
                "denied": approved ? [] : wanted.map(\.name),
                "blocked": refused]
    }

    private func requireGranted(_ bundle: String, _ action: ActionKind, _ conversation: String) throws {
        let appTier = tier(bundle)
        guard appTier != .blocked else {
            throw AgentFailure(code: "APP_BLOCKED", message: "\(bundle.isEmpty ? "This target" : bundle) cannot be controlled.")
        }
        guard control.grantedApps(conversation).contains(bundle) else {
            throw AgentFailure(code: "APP_NOT_GRANTED",
                               message: "\(bundle) is not granted to this conversation. Call request_access first.")
        }
        guard AppPolicy.allows(appTier, action) else {
            throw AgentFailure(code: "ACTION_NOT_ALLOWED",
                               message: "\(bundle) is \(appTier.rawValue): \(action == .keyboard ? "typing" : "input") is not allowed.")
        }
    }

    private func openApplication(_ params: [String: Any], _ conversation: String) throws -> ([String: Any], String?) {
        guard let query = params["app"] as? String, let app = AppCatalog.resolve(query) else {
            throw AgentFailure(code: "APP_NOT_FOUND", message: "No application matches the app parameter.")
        }
        try requireGranted(app.bundleIdentifier, .view, conversation)
        try AppCatalog.open(app.bundleIdentifier)
        return (["opened": app.bundleIdentifier], app.bundleIdentifier)
    }

    // MARK: capture

    private func screenshot(_ conversation: String, region: CGRect?) throws -> [String: Any] {
        guard Permissions.screenRecording else {
            throw AgentFailure(code: "PERMISSION_REQUIRED",
                               message: "Allow KetPlus Computer in System Settings > Privacy & Security > Screen & System Audio Recording.")
        }
        let granted = control.grantedApps(conversation).filter { tier($0) != .blocked }
        let shot: Screenshot
        do {
            shot = try waitFor { try await ScreenCapture.capture(including: granted, region: region) }
        } catch let error as CaptureError {
            throw AgentFailure(code: error == .nothingGranted ? "APP_NOT_GRANTED" : "CAPTURE_FAILED", message: error.description)
        } catch {
            throw AgentFailure(code: "CAPTURE_FAILED", message: String(describing: error))
        }
        if region == nil { lock.withLock { geometries[conversation] = shot.geometry } }
        return ["png": shot.png.base64EncodedString(), "width": shot.geometry.imageWidth,
                "height": shot.geometry.imageHeight, "visibleApps": shot.visibleApps]
    }

    private func zoom(_ params: [String: Any], _ conversation: String) throws -> [String: Any] {
        let region = (params["region"] as? [Any] ?? []).compactMap { ($0 as? NSNumber)?.intValue }
        guard let rect = try geometry(conversation).displayRect(imageRegion: region) else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "region must be [x0, y0, x1, y1] inside the last screenshot.")
        }
        return try screenshot(conversation, region: rect)
    }

    // MARK: pointer

    private func geometry(_ conversation: String) throws -> ScreenGeometry {
        guard let geometry = lock.withLock({ geometries[conversation] }) else {
            throw AgentFailure(code: "NO_SCREENSHOT", message: "Take a screenshot first; coordinates refer to it.")
        }
        return geometry
    }

    private func coordinate(_ value: Any?) -> (Int, Int)? {
        guard let pair = value as? [Any], pair.count == 2,
              let x = (pair[0] as? NSNumber)?.intValue, let y = (pair[1] as? NSNumber)?.intValue else { return nil }
        return (x, y)
    }

    /// Global point for an image coordinate (or the cursor), after checking the
    /// app that will actually receive the event.
    private func pointerTarget(_ value: Any?, _ conversation: String, required: Bool) throws -> (CGPoint, String) {
        let point: CGPoint
        if let (x, y) = coordinate(value) {
            let geometry = try geometry(conversation)
            guard geometry.contains(imageX: x, imageY: y) else {
                throw AgentFailure(code: "OUT_OF_BOUNDS",
                                   message: "(\(x), \(y)) is outside the \(geometry.imageWidth)x\(geometry.imageHeight) screenshot.")
            }
            point = geometry.globalPoint(imageX: x, imageY: y)
        } else if value == nil && !required {
            point = InputControl.cursorLocation
        } else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "coordinate must be [x, y] integers.")
        }
        try requireAccessibility()
        let app = WindowTargets.application(at: point)
        try requireGranted(app, .pointer, conversation)
        return (point, app)
    }

    private func requireAccessibility() throws {
        guard Permissions.accessibility else {
            throw AgentFailure(code: "PERMISSION_REQUIRED",
                               message: "Allow KetPlus Computer in System Settings > Privacy & Security > Accessibility.")
        }
    }

    private func modifiers(_ params: [String: Any]) throws -> KeyCombo.Modifiers {
        guard let parsed = KeyCombo.parseModifiers(params["text"] as? String ?? "") else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "text must be modifiers such as shift, ctrl, alt, super or ctrl+shift.")
        }
        return parsed
    }

    private func click(_ method: String, _ params: [String: Any], _ conversation: String) throws -> ([String: Any], String?) {
        let flags = try modifiers(params)
        let (point, app) = try pointerTarget(params["coordinate"], conversation, required: false)
        let button: MouseButton = method == "right_click" ? .right : method == "middle_click" ? .middle : .left
        let count = method == "double_click" ? 2 : method == "triple_click" ? 3 : 1
        try InputControl.click(button, count: count, at: point, modifiers: flags)
        return (["ok": true], app)
    }

    private func drag(_ params: [String: Any], _ conversation: String) throws -> ([String: Any], String?) {
        let flags = try modifiers(params)
        let (start, app) = try pointerTarget(params["start_coordinate"], conversation, required: true)
        let (end, _) = try pointerTarget(params["coordinate"], conversation, required: true)
        try InputControl.drag(from: start, to: end, modifiers: flags)
        return (["ok": true], app)
    }

    private func scroll(_ params: [String: Any], _ conversation: String) throws -> ([String: Any], String?) {
        guard let direction = ScrollDirection(rawValue: params["scroll_direction"] as? String ?? ""),
              let amount = (params["scroll_amount"] as? NSNumber)?.intValue, (1...50).contains(amount) else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "scroll needs scroll_direction and scroll_amount 1-50.")
        }
        let flags = try modifiers(params)
        let (point, app) = try pointerTarget(params["coordinate"], conversation, required: false)
        try InputControl.scroll(direction, amount: amount, at: point, modifiers: flags)
        return (["ok": true], app)
    }

    // MARK: keyboard

    private func keyboard(_ method: String, _ params: [String: Any], _ conversation: String) throws -> ([String: Any], String?) {
        guard let text = params["text"] as? String, !text.isEmpty, text.count <= 4000 else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "text must be 1 to 4000 characters.")
        }
        try requireAccessibility()
        let app = WindowTargets.frontmostApplication
        try requireGranted(app, .keyboard, conversation)
        if IsSecureEventInputEnabled() {
            throw AgentFailure(code: "SECURE_INPUT",
                               message: "A password or other secure field has focus. Ask the user to enter it.")
        }
        if method == "type" {
            try InputControl.type(text)
        } else {
            guard let combo = KeyCombo.parse(text) else {
                throw AgentFailure(code: "INVALID_PARAMS", message: "Unknown key \"\(text)\".")
            }
            let count = (params["repeat"] as? NSNumber)?.intValue ?? 1
            guard (1...100).contains(count) else {
                throw AgentFailure(code: "INVALID_PARAMS", message: "repeat must be 1 to 100.")
            }
            try InputControl.press(combo, repeat: count)
        }
        return (["ok": true], app)
    }

    private func cursorPosition(_ conversation: String) -> [String: Any] {
        guard let geometry = lock.withLock({ geometries[conversation] }) else { return ["coordinate": NSNull()] }
        let point = geometry.imagePoint(global: InputControl.cursorLocation)
        return ["coordinate": [point.x, point.y]]
    }

    private func waitFor<T>(_ operation: @escaping () async throws -> T) throws -> T {
        let semaphore = DispatchSemaphore(value: 0)
        var outcome: Result<T, Error>!
        Task.detached {
            do { outcome = .success(try await operation()) } catch { outcome = .failure(error) }
            semaphore.signal()
        }
        semaphore.wait()
        return try outcome.get()
    }
}
