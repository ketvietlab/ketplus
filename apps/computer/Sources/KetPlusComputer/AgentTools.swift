import AppKit
import Carbon.HIToolbox
import ComputerProtocol
import Foundation

/// Tool semantics and policy enforcement (ADR 0030, 0031). Runs on socket threads;
/// UI work hops to the main thread. Background tools are in AgentTools+Background.
final class AgentTools {
    let paths: AgentPaths
    let control: ControlState
    private let audit: AuditLog
    let lock = NSLock()
    private var geometries: [String: ScreenGeometry] = [:]
    private let indicator: ControlIndicator
    let cursor: VirtualCursor
    private var killSwitch: KillSwitch?
    let references = ReferenceTable<AXUIElement>()
    /// The app each conversation last acted on: the default keyboard target.
    var lastTargets: [String: String] = [:]
    /// When the agent last used the real pointer or HID keyboard.
    var lastForeground: Date?

    @MainActor
    init(paths: AgentPaths, control: ControlState, indicator: ControlIndicator, cursor: VirtualCursor) {
        self.paths = paths
        self.control = control
        self.indicator = indicator
        self.cursor = cursor
        audit = AuditLog(paths: paths)
        indicator.onStop = { [weak self] in self?.userStopped(by: "stop") }
    }

    // MARK: lifecycle hooks (main thread)

    @MainActor
    func installKillSwitch() {
        guard killSwitch == nil, Permissions.accessibility else { return }
        let killSwitch = KillSwitch(isArmed: { [weak self] in self?.escapeStopsAgent() ?? false }) { [weak self] in
            MainActor.assumeIsolated { self?.userStopped(by: "esc") }
        }
        if killSwitch.install() { self.killSwitch = killSwitch }
    }

    @MainActor
    func expireIdleLease() {
        if control.expireIdleLease() {
            indicator.hide()
            cursor.hide()
        }
    }

    /// Esc is the user's own key unless they are in an app this agent controls or
    /// the agent just used the real pointer (ADR 0031).
    private func escapeStopsAgent() -> Bool {
        guard let holder = control.activeHolder else { return false }
        let since = lock.withLock { lastForeground.map { Date().timeIntervalSince($0) } }
        return KillSwitchPolicy.consumesEscape(
            frontmostIsGranted: control.grantedApps(holder).contains(WindowTargets.frontmostApplication),
            secondsSinceForeground: since)
    }

    @MainActor
    private func userStopped(by source: String) {
        if let stopped = control.cancelActive() {
            references.invalidate(stopped)
            audit.record(conversation: stopped, tool: source, target: nil, outcome: "cancelled_by_user")
        }
        indicator.hide()
        cursor.hide()
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
            var detail: [String: Any] = [:]
            if method == "type" { detail["characters"] = (params["text"] as? String)?.count ?? 0 }
            if method == "act", params["action"] as? String == "set_value" {
                detail["characters"] = (params["value"] as? String)?.count ?? 0
            }
            if let mode = result.value["mode"] { detail["mode"] = mode }
            if let decision = result.value["decision"] { detail["decision"] = decision }
            audit.record(conversation: conversation, tool: method, target: result.target, outcome: "ok", detail: detail)
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
                               message: "The user stopped computer use (Esc or Stop). Stop and ask the user before continuing.")
        }
        try takeControl(conversation)
        // Input changes the UI, so element references from the last observe go stale
        // (act resolves its reference first, then invalidates).
        if !["request_access", "screenshot", "zoom", "observe", "act"].contains(method) {
            references.invalidate(conversation)
        }
        let foreground = params["foreground"] as? Bool ?? false
        switch method {
        case "request_access": return (try requestAccess(params, conversation), nil)
        case "open_application": return try openApplication(params, conversation)
        case "screenshot": return (try screenshot(conversation, region: nil, display: display(for: params["display"])), nil)
        case "zoom": return (try zoom(params, conversation), nil)
        case "observe": return try observe(params, conversation)
        case "act": return try act(params, conversation)
        case "left_click", "right_click", "middle_click", "double_click", "triple_click":
            return foreground ? try click(method, params, conversation)
                              : try backgroundClick(method, params, conversation)
        case "mouse_move":
            guard foreground else { throw needsForeground("mouse_move") }
            let (point, app) = try pointerTarget(params["coordinate"], conversation, required: true)
            try InputControl.move(to: point)
            markForeground()
            return (["ok": true], app)
        case "left_click_drag":
            guard foreground else { throw needsForeground("left_click_drag") }
            return try drag(params, conversation)
        case "scroll":
            return foreground ? try scroll(params, conversation) : try backgroundScroll(params, conversation)
        case "type", "key":
            return foreground ? try keyboard(method, params, conversation)
                              : try backgroundKeyboard(method, params, conversation)
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
        let work = workDisplay()
        DispatchQueue.main.async { [self] in
            MainActor.assumeIsolated {
                installKillSwitch()
                // The user may have pressed Stop since this call took the lease.
                guard control.activeHolder == conversation else { return }
                indicator.show(apps: names.sorted(), on: work.flatMap(Displays.screen))
            }
        }
    }

    private func deniedApps() -> Set<String> {
        Set(setting("deniedApps") as? [String] ?? [])
    }

    func tier(_ bundle: String) -> AppTier { AppPolicy.tier(for: bundle, denied: deniedApps()) }

    private func listApps(_ conversation: String) -> [String: Any] {
        let granted = control.grantedApps(conversation)
        let work = workDisplay()
        return ["apps": AppCatalog.running().map { app in
            ["name": app.name, "bundleId": app.bundleIdentifier, "tier": tier(app.bundleIdentifier).rawValue,
             "granted": granted.contains(app.bundleIdentifier), "frontmost": app.frontmost] as [String: Any]
        }, "displays": allDisplays().map { displaySummary($0, work: work) }]
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
        var decision = "none_needed"
        if !wanted.isEmpty {
            guard Permissions.screenRecording, Permissions.accessibility else {
                throw AgentFailure(code: "PERMISSION_REQUIRED",
                                   message: "KetPlus Computer needs Screen Recording and Accessibility in System Settings > Privacy & Security.")
            }
            let work = workDisplay()
            let request = ApprovalPanel.Request(conversation: conversation, apps: wanted,
                                                reason: String((params["reason"] as? String ?? "").prefix(300)),
                                                workDisplay: allDisplays().count > 1 ? work?.name : nil)
            let answer = DispatchQueue.main.sync { MainActor.assumeIsolated { ApprovalPanel.ask(request) } }
            decision = answer.rawValue
            approved = answer == .allow
            if approved { control.grant(Set(wanted.map(\.bundleIdentifier)), to: conversation) }
        }
        let moved = approved ? moveToWorkDisplay(wanted.map(\.bundleIdentifier), conversation: conversation) : []
        return ["granted": control.grantedApps(conversation).sorted(),
                "denied": approved ? [] : wanted.map(\.name),
                "blocked": refused, "decision": decision, "movedToWorkDisplay": moved]
    }

    func requireGranted(_ bundle: String, _ action: ActionKind, _ conversation: String) throws {
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
        let activate = params["foreground"] as? Bool ?? false
        let moved = try openOnWorkDisplay(app.bundleIdentifier, activate: activate, conversation: conversation)
        lock.withLock { lastTargets[conversation] = app.bundleIdentifier }
        return (["opened": app.bundleIdentifier, "frontmost": activate, "movedToWorkDisplay": moved], app.bundleIdentifier)
    }

    // MARK: capture

    private func screenshot(_ conversation: String, region: CGRect?, display: DisplayInfo) throws -> [String: Any] {
        guard Permissions.screenRecording else {
            throw AgentFailure(code: "PERMISSION_REQUIRED",
                               message: "Allow KetPlus Computer in System Settings > Privacy & Security > Screen & System Audio Recording.")
        }
        let granted = control.grantedApps(conversation).filter { tier($0) != .blocked }
        // Apps such as Qt do not repaint hidden windows, so their pixels would be old. On a
        // separate work display the agent raises them (no activation); elsewhere it warns.
        let work = workDisplay()
        var raised: [String] = [], covered: [String] = []
        for window in WindowStack.hiddenGranted(granted, on: display) {
            if display.id == work?.id, !display.isMain, Displays.raiseWindow(of: window.pid, frame: window.frame) {
                raised.append(window.bundle)
            } else {
                covered.append(window.bundle)
            }
        }
        if !raised.isEmpty { usleep(300_000) }
        let shot: Screenshot
        do {
            shot = try waitFor { try await ScreenCapture.capture(including: granted, region: region, display: display.id) }
        } catch let error as CaptureError {
            let code = switch error {
            case .nothingGranted: "APP_NOT_GRANTED"
            case .nothingOnDisplay: "NOTHING_ON_DISPLAY"
            default: "CAPTURE_FAILED"
            }
            throw AgentFailure(code: code, message: error.description)
        } catch {
            throw AgentFailure(code: "CAPTURE_FAILED", message: String(describing: error))
        }
        if region == nil { lock.withLock { geometries[conversation] = shot.geometry } }
        return ["png": shot.png.base64EncodedString(), "width": shot.geometry.imageWidth,
                "height": shot.geometry.imageHeight, "visibleApps": shot.visibleApps,
                "raised": Array(Set(raised)).sorted(), "covered": Array(Set(covered)).sorted(),
                "display": displaySummary(display, work: work)]
    }

    private func zoom(_ params: [String: Any], _ conversation: String) throws -> [String: Any] {
        let region = (params["region"] as? [Any] ?? []).compactMap { ($0 as? NSNumber)?.intValue }
        let geometry = try geometry(conversation)
        guard let rect = geometry.displayRect(imageRegion: region) else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "region must be [x0, y0, x1, y1] inside the last screenshot.")
        }
        guard let display = display(of: geometry) else {
            throw AgentFailure(code: "NO_SCREENSHOT", message: "The last screenshot's display is gone. Take a new screenshot.")
        }
        return try screenshot(conversation, region: rect, display: display)
    }

    // MARK: pointer

    func geometry(_ conversation: String) throws -> ScreenGeometry {
        guard let geometry = lock.withLock({ geometries[conversation] }) else {
            throw AgentFailure(code: "NO_SCREENSHOT", message: "Take a screenshot first; coordinates refer to it.")
        }
        return geometry
    }

    /// The last full screenshot's geometry, or the display's when none was taken.
    func currentGeometry(_ conversation: String) -> ScreenGeometry {
        lock.withLock { geometries[conversation] }
            ?? ScreenGeometry(displayBounds: workDisplay()?.bounds ?? CGDisplayBounds(CGMainDisplayID()))
    }

    func coordinate(_ value: Any?) -> (Int, Int)? {
        guard let pair = value as? [Any], pair.count == 2,
              let x = (pair[0] as? NSNumber)?.intValue, let y = (pair[1] as? NSNumber)?.intValue else { return nil }
        return (x, y)
    }

    /// Global point for an image coordinate of the last screenshot.
    func globalPoint(_ x: Int, _ y: Int, _ conversation: String) throws -> CGPoint {
        let geometry = try geometry(conversation)
        guard geometry.contains(imageX: x, imageY: y) else {
            throw AgentFailure(code: "OUT_OF_BOUNDS",
                               message: "(\(x), \(y)) is outside the \(geometry.imageWidth)x\(geometry.imageHeight) screenshot.")
        }
        return geometry.globalPoint(imageX: x, imageY: y)
    }

    /// Global point for an image coordinate (or the cursor), after checking the
    /// app that will actually receive the real pointer's event: the topmost one.
    private func pointerTarget(_ value: Any?, _ conversation: String, required: Bool) throws -> (CGPoint, String) {
        let point: CGPoint
        if let (x, y) = coordinate(value) {
            point = try globalPoint(x, y, conversation)
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

    func requireAccessibility() throws {
        guard Permissions.accessibility else {
            throw AgentFailure(code: "PERMISSION_REQUIRED",
                               message: "Allow KetPlus Computer in System Settings > Privacy & Security > Accessibility.")
        }
    }

    func modifiers(_ params: [String: Any]) throws -> KeyCombo.Modifiers {
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
        markForeground()
        return (["ok": true], app)
    }

    private func drag(_ params: [String: Any], _ conversation: String) throws -> ([String: Any], String?) {
        let flags = try modifiers(params)
        let (start, app) = try pointerTarget(params["start_coordinate"], conversation, required: true)
        let (end, _) = try pointerTarget(params["coordinate"], conversation, required: true)
        try InputControl.drag(from: start, to: end, modifiers: flags)
        markForeground()
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
        markForeground()
        return (["ok": true], app)
    }

    // MARK: keyboard

    func keyText(_ params: [String: Any]) throws -> String {
        guard let text = params["text"] as? String, !text.isEmpty, text.count <= 4000 else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "text must be 1 to 4000 characters.")
        }
        return text
    }

    func keyCombo(_ text: String, _ params: [String: Any]) throws -> (KeyCombo, Int) {
        guard let combo = KeyCombo.parse(text) else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "Unknown key \"\(text)\".")
        }
        let count = (params["repeat"] as? NSNumber)?.intValue ?? 1
        guard (1...100).contains(count) else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "repeat must be 1 to 100.")
        }
        return (combo, count)
    }

    private func keyboard(_ method: String, _ params: [String: Any], _ conversation: String) throws -> ([String: Any], String?) {
        let text = try keyText(params)
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
            let (combo, count) = try keyCombo(text, params)
            try InputControl.press(combo, repeat: count)
        }
        markForeground()
        return (["ok": true], app)
    }

    private func cursorPosition(_ conversation: String) -> [String: Any] {
        guard let geometry = lock.withLock({ geometries[conversation] }) else { return ["coordinate": NSNull()] }
        let point = geometry.imagePoint(global: InputControl.cursorLocation)
        return ["coordinate": [point.x, point.y]]
    }

    func markForeground() {
        lock.withLock { lastForeground = Date() }
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
