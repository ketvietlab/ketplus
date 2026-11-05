import AppKit
import ComputerProtocol
import Foundation

/// Background control (ADR 0031): accessibility actions and keyboard events sent to
/// one process. The user's pointer, keyboard focus and frontmost app stay theirs.
extension AgentTools {
    private static let actNames: [String: String] = [
        "increment": "AXIncrement", "decrement": "AXDecrement", "confirm": "AXConfirm",
        "cancel": "AXCancel", "raise": "AXRaise", "pick": "AXPick", "scroll_to_visible": "AXScrollToVisible",
    ]
    private static let shownActions: [String: String] = [
        "AXPress": "press", "AXShowMenu": "show_menu", "AXIncrement": "increment", "AXDecrement": "decrement",
        "AXConfirm": "confirm", "AXCancel": "cancel", "AXRaise": "raise", "AXPick": "pick",
        "AXScrollToVisible": "scroll_to_visible",
    ]

    func needsForeground(_ what: String) -> AgentFailure {
        AgentFailure(code: "NEEDS_FOREGROUND",
                     message: "\(what) has no background equivalent here. Retry with \"foreground\": true to use the "
                         + "real pointer: it moves the user's cursor and the target must be on top at that point.")
    }

    // MARK: observe / act

    func observe(_ params: [String: Any], _ conversation: String) throws -> ([String: Any], String?) {
        try requireAccessibility()
        let (bundle, pid) = try runningApp(params["app"], conversation, .view)
        let limit = min(max((params["max_nodes"] as? NSNumber)?.intValue ?? 300, 20), 1500)
        let (nodes, truncated) = AXTree.snapshot(pid, limit: limit)
        let geometry = currentGeometry(conversation)
        var elements: [String: AXUIElement] = [:]
        var lines: [String] = []
        for (index, node) in nodes.enumerated() {
            let reference = "n\(index + 1)"
            elements[reference] = node.element
            lines.append(String(repeating: "  ", count: min(node.depth, 12)) + describe(node, reference, geometry))
        }
        let snapshot = references.store(elements, app: bundle, conversation: conversation)
        return (["snapshotId": snapshot, "app": bundle, "nodes": nodes.count, "truncated": truncated,
                 "tree": lines.joined(separator: "\n")], bundle)
    }

    func act(_ params: [String: Any], _ conversation: String) throws -> ([String: Any], String?) {
        try requireAccessibility()
        guard let snapshot = params["snapshot_id"] as? String, let reference = params["ref"] as? String,
              let action = params["action"] as? String else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "act needs snapshot_id, ref and action.")
        }
        let element: AXUIElement
        let bundle: String
        do {
            (element, bundle) = try references.resolve(snapshot: snapshot, reference: reference, conversation: conversation)
        } catch ReferenceTable<AXUIElement>.Failure.stale {
            throw AgentFailure(code: "STALE_SNAPSHOT",
                               message: "Snapshot \(snapshot) is out of date: any input changes the UI. Call observe again.")
        } catch {
            throw AgentFailure(code: "UNKNOWN_REF", message: "\(reference) is not in snapshot \(snapshot).")
        }
        references.invalidate(conversation)
        let node = AXTree.node(element)
        try requireGranted(bundle, action == "set_value" ? .keyboard : .pointer, conversation)
        if let frame = node.frame { glide(to: CGPoint(x: frame.midX, y: frame.midY)) }
        let done: Bool
        switch action {
        case "press": done = AXTree.perform(.press, on: element)
        case "focus": done = AXTree.perform(.focus, on: element)
        case "select": done = AXTree.perform(.select, on: element)
        case "show_menu": done = AXTree.perform(.showMenu, on: element)
        case "set_value":
            guard let value = params["value"] as? String, value.count <= 4000 else {
                throw AgentFailure(code: "INVALID_PARAMS", message: "set_value needs value of at most 4000 characters.")
            }
            guard !node.info.isSecureText else { throw secureInput() }
            done = AXTree.setValue(value, on: element)
        default:
            guard let name = Self.actNames[action] else {
                throw AgentFailure(code: "INVALID_PARAMS", message: "Unknown action \(action).")
            }
            done = AXTree.performNamed(name, on: element)
        }
        guard done else {
            throw AgentFailure(code: "ACTION_FAILED",
                               message: "\(bundle) did not accept \(action) on \(reference). Take a screenshot and try another way.")
        }
        lock.withLock { lastTargets[conversation] = bundle }
        return (["ok": true, "mode": "background"], bundle)
    }

    // MARK: pointer

    func backgroundClick(_ method: String, _ params: [String: Any], _ conversation: String) throws -> ([String: Any], String?) {
        guard method == "left_click" || method == "right_click" else { throw needsForeground(method) }
        guard try modifiers(params).isEmpty else { throw needsForeground("A click with modifier keys") }
        let (point, owner) = try backgroundTarget(params, conversation, method)
        let path = AXTree.path(owner.pid, at: point)
        guard let plan = BackgroundPlanner.click(path: path.map(\.info), rightButton: method == "right_click") else {
            throw needsForeground("No accessible control at that point, so this click")
        }
        glide(to: point)
        guard AXTree.perform(plan.action, on: path[plan.index].element) else {
            throw AgentFailure(code: "ACTION_FAILED", message: "\(owner.bundle) did not accept \(plan.action.rawValue).")
        }
        lock.withLock { lastTargets[conversation] = owner.bundle }
        let node = path[plan.index]
        return (["ok": true, "mode": "background", "action": plan.action.rawValue,
                 "element": "\(role(node.info.role)) \(quoted(node.name))"], owner.bundle)
    }

    func backgroundScroll(_ params: [String: Any], _ conversation: String) throws -> ([String: Any], String?) {
        guard let direction = ScrollDirection(rawValue: params["scroll_direction"] as? String ?? ""),
              let amount = (params["scroll_amount"] as? NSNumber)?.intValue, (1...50).contains(amount) else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "scroll needs scroll_direction and scroll_amount 1-50.")
        }
        guard try modifiers(params).isEmpty else { throw needsForeground("A scroll with modifier keys") }
        let (point, owner) = try backgroundTarget(params, conversation, "scroll")
        glide(to: point)
        let vertical = direction == .up || direction == .down
        guard AXTree.scroll(owner.pid, at: point, vertical: vertical,
                            increasing: direction == .down || direction == .right, amount: amount) else {
            throw needsForeground("No accessible scroll bar at that point, so this scroll")
        }
        lock.withLock { lastTargets[conversation] = owner.bundle }
        return (["ok": true, "mode": "background"], owner.bundle)
    }

    /// The granted app under an image coordinate, ignoring other apps' windows on top.
    private func backgroundTarget(_ params: [String: Any], _ conversation: String, _ method: String) throws
        -> (CGPoint, (bundle: String, pid: pid_t)) {
        guard let (x, y) = coordinate(params["coordinate"]) else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "\(method) needs coordinate [x, y] in the background.")
        }
        let point = try globalPoint(x, y, conversation)
        try requireAccessibility()
        guard let owner = WindowTargets.grantedOwner(at: point, among: control.grantedApps(conversation)) else {
            throw AgentFailure(code: "APP_NOT_GRANTED", message: "No granted app has a window at (\(x), \(y)).")
        }
        try requireGranted(owner.bundle, .pointer, conversation)
        return (point, owner)
    }

    // MARK: keyboard

    /// Keyboard events go to one process: the `app` parameter, else the app the
    /// conversation last acted on, else the frontmost app.
    func backgroundKeyboard(_ method: String, _ params: [String: Any], _ conversation: String) throws
        -> ([String: Any], String?) {
        let text = try keyText(params)
        try requireAccessibility()
        let explicit = try (params["app"] as? String).map { query -> String in
            guard let app = AppCatalog.resolve(query) else {
                throw AgentFailure(code: "APP_NOT_FOUND", message: "No application matches \"\(query)\".")
            }
            return app.bundleIdentifier
        }
        let bundle = KeyboardTarget.resolve(explicit: explicit, lastTarget: lock.withLock { lastTargets[conversation] },
                                            frontmost: WindowTargets.frontmostApplication)
        try requireGranted(bundle, .keyboard, conversation)
        guard let pid = WindowTargets.pid(of: bundle) else {
            throw AgentFailure(code: "APP_NOT_RUNNING", message: "\(bundle) is not running. Call open_application first.")
        }
        // Only the target's focus matters: the user may type a password elsewhere meanwhile.
        let focused = AXTree.focusedElement(pid)
        if focused?.info.isSecureText == true { throw secureInput() }
        if let frame = focused?.frame { glide(to: CGPoint(x: frame.midX, y: frame.midY)) }
        var delivery = "event"
        if method == "type" {
            try InputControl.type(text, to: pid)
        } else {
            let (combo, count) = try keyCombo(text, params)
            // An inactive app ignores posted key equivalents; its menu bar still runs them.
            if count == 1, combo == KeyCombo.parse("super+a"), AXTree.selectAllText(pid) {
                delivery = "text_selection"
            } else if count == 1, !combo.modifiers.isDisjoint(with: [.command, .control, .option]),
                      AXTree.pressMenuShortcut(pid, combo) {
                delivery = "menu"
            } else {
                try InputControl.press(combo, repeat: count, to: pid)
            }
        }
        lock.withLock { lastTargets[conversation] = bundle }
        return (["ok": true, "mode": "background", "app": bundle, "delivery": delivery], bundle)
    }

    // MARK: helpers

    private func runningApp(_ query: Any?, _ conversation: String, _ kind: ActionKind) throws -> (String, pid_t) {
        guard let query = query as? String, let app = AppCatalog.resolve(query) else {
            throw AgentFailure(code: "APP_NOT_FOUND", message: "No application matches the app parameter.")
        }
        try requireGranted(app.bundleIdentifier, kind, conversation)
        guard let pid = WindowTargets.pid(of: app.bundleIdentifier) else {
            throw AgentFailure(code: "APP_NOT_RUNNING",
                               message: "\(app.bundleIdentifier) is not running. Call open_application first.")
        }
        return (app.bundleIdentifier, pid)
    }

    private func secureInput() -> AgentFailure {
        AgentFailure(code: "SECURE_INPUT", message: "A password or other secure field has focus. Ask the user to enter it.")
    }

    /// Shows the agent's pointer gliding to a point before the action lands there.
    private func glide(to point: CGPoint) {
        let cursor = self.cursor
        DispatchQueue.main.async { MainActor.assumeIsolated { cursor.glide(to: point) } }
        usleep(200_000)
    }

    private func describe(_ node: AXTree.Node, _ reference: String, _ geometry: ScreenGeometry) -> String {
        var parts = ["[\(reference)] \(role(node.info.role))"]
        if !node.name.isEmpty { parts.append(quoted(node.name)) }
        if let value = node.value, !value.isEmpty { parts.append("value=\(quoted(value, limit: 200))") }
        if node.info.isSecureText { parts.append("secure") }
        let actions = node.info.actions.compactMap { Self.shownActions[$0] }
        if !actions.isEmpty { parts.append("actions=\(actions.joined(separator: ","))") }
        if node.focused { parts.append("focused") }
        if !node.enabled { parts.append("disabled") }
        if let frame = node.frame, !geometry.displayBounds.contains(CGPoint(x: frame.midX, y: frame.midY)) {
            parts.append("offscreen")
        } else if let frame = node.frame {
            let origin = geometry.imagePoint(global: frame.origin)
            let end = geometry.imagePoint(global: CGPoint(x: frame.maxX, y: frame.maxY))
            parts.append("@\(origin.x),\(origin.y),\(end.x - origin.x)x\(end.y - origin.y)")
        }
        return parts.joined(separator: " ")
    }

    private func role(_ raw: String) -> String {
        raw.hasPrefix("AX") ? String(raw.dropFirst(2)).lowercased() : raw.lowercased()
    }

    private func quoted(_ text: String, limit: Int = 120) -> String {
        let flat = text.replacingOccurrences(of: "\n", with: " ")
        return "\"" + (flat.count > limit ? String(flat.prefix(limit)) + "…" : flat) + "\""
    }
}
