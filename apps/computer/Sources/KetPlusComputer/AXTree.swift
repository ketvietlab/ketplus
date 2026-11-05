import ApplicationServices
import ComputerProtocol
import Foundation

/// Reads and drives one application's accessibility tree (ADR 0031). Nothing here
/// moves the pointer or changes the frontmost app.
enum AXTree {
    struct Node {
        let element: AXUIElement
        let depth: Int
        let info: AXNodeInfo
        let name: String
        let value: String?
        /// Global top-left points.
        let frame: CGRect?
        let focused: Bool
        let enabled: Bool
    }

    static let maxDepth = 40

    static func application(_ pid: pid_t) -> AXUIElement {
        let app = AXUIElementCreateApplication(pid)
        AXUIElementSetMessagingTimeout(app, 1.5)
        // Chromium and Electron build their tree only when an assistive client asks.
        AXUIElementSetAttributeValue(app, "AXManualAccessibility" as CFString, kCFBooleanTrue)
        return app
    }

    /// Up to `limit` nodes of the app's windows in document order. Containers with
    /// neither a name, a value nor an action are skipped, but their children are kept.
    static func snapshot(_ pid: pid_t, limit: Int) -> (nodes: [Node], truncated: Bool) {
        var nodes: [Node] = []
        var truncated = false
        // Some apps (Qt among them) list the application among their windows or children,
        // which would repeat the menu bar at every level until maxDepth. Visit each element once.
        var seen: [CFHashCode: [AXUIElement]] = [:]
        func firstVisit(_ element: AXUIElement) -> Bool {
            let hash = CFHash(element)
            if seen[hash, default: []].contains(where: { CFEqual($0, element) }) { return false }
            seen[hash, default: []].append(element)
            return true
        }
        func walk(_ element: AXUIElement, depth: Int) {
            guard depth < maxDepth, firstVisit(element) else { return }
            guard nodes.count < limit else { truncated = true; return }
            let node = describe(element, depth: depth)
            guard node.info.role != "AXApplication" else {
                for child in children(element) { walk(child, depth: depth) }
                return
            }
            if node.info.role == "AXWindow" || !node.name.isEmpty || node.value != nil || !node.info.actions.isEmpty
                || node.info.isTextInput {
                nodes.append(node)
            }
            for child in children(element) { walk(child, depth: depth + 1) }
        }
        for window in windows(pid) { walk(window, depth: 0) }
        return (nodes, truncated)
    }

    /// Element path at a global point inside the app's own windows, deepest first.
    /// Other apps' windows on top do not matter: the tree belongs to the target app.
    static func path(_ pid: pid_t, at point: CGPoint) -> [Node] {
        for window in windows(pid) {
            guard let frame = frame(window), frame.contains(point) else { continue }
            var path = [describe(window, depth: 0)]
            var current = window
            var depth = 1
            while depth < maxDepth,
                  let next = children(current).reversed().first(where: { self.frame($0)?.contains(point) == true }) {
                path.append(describe(next, depth: depth))
                current = next
                depth += 1
            }
            return path.reversed()
        }
        return []
    }

    static func perform(_ action: BackgroundAction, on element: AXUIElement) -> Bool {
        switch action {
        case .press: return AXUIElementPerformAction(element, kAXPressAction as CFString) == .success
        case .showMenu: return AXUIElementPerformAction(element, kAXShowMenuAction as CFString) == .success
        case .focus:
            return AXUIElementSetAttributeValue(element, kAXFocusedAttribute as CFString, kCFBooleanTrue) == .success
        case .select:
            return AXUIElementSetAttributeValue(element, kAXSelectedAttribute as CFString, kCFBooleanTrue) == .success
        }
    }

    static func performNamed(_ action: String, on element: AXUIElement) -> Bool {
        AXUIElementPerformAction(element, action as CFString) == .success
    }

    static func setValue(_ text: String, on element: AXUIElement) -> Bool {
        AXUIElementSetAttributeValue(element, kAXValueAttribute as CFString, text as CFString) == .success
    }

    /// Presses the enabled menu bar item whose shortcut is `combo`. Returns false when
    /// the app has none.
    static func pressMenuShortcut(_ pid: pid_t, _ combo: KeyCombo) -> Bool {
        guard combo.character != nil, let bar: AXUIElement = attribute(application(pid), kAXMenuBarAttribute) else {
            return false
        }
        var visited = 0
        func search(_ element: AXUIElement, depth: Int) -> AXUIElement? {
            visited += 1
            guard depth < 8, visited < 4000 else { return nil }
            for child in children(element) {
                if attribute(child, kAXRoleAttribute) == "AXMenuItem",
                   let character: String = attribute(child, kAXMenuItemCmdCharAttribute),
                   MenuShortcut.matches(itemCharacter: character,
                                        itemModifiers: (attribute(child, kAXMenuItemCmdModifiersAttribute) as NSNumber?)?.intValue ?? 0,
                                        combo: combo),
                   // Items that act on the key window stay disabled while the app is inactive.
                   attribute(child, kAXEnabledAttribute) ?? false {
                    return child
                }
                if let found = search(child, depth: depth + 1) { return found }
            }
            return nil
        }
        guard let item = search(bar, depth: 0) else { return false }
        return AXUIElementPerformAction(item, kAXPressAction as CFString) == .success
    }

    /// Selects all text of the app's focused text input: select-all for an inactive
    /// app, whose Select All menu item has no key window to act on.
    static func selectAllText(_ pid: pid_t) -> Bool {
        guard let focused = focusedElement(pid), focused.info.isTextInput, !focused.info.isSecureText else { return false }
        var range = CFRange(location: 0, length: (focused.value as NSString?)?.length ?? 0)
        guard let value = AXValueCreate(.cfRange, &range) else { return false }
        return AXUIElementSetAttributeValue(focused.element, kAXSelectedTextRangeAttribute as CFString, value) == .success
    }

    /// The app's focused element, for the secure-field check before typing.
    static func focusedElement(_ pid: pid_t) -> Node? {
        guard let element: AXUIElement = attribute(application(pid), kAXFocusedUIElementAttribute) else { return nil }
        return describe(element, depth: 0)
    }

    /// Moves the scroll bar of the scroll area containing `point`. Returns false when
    /// the app exposes no such scroll bar.
    static func scroll(_ pid: pid_t, at point: CGPoint, vertical: Bool, increasing: Bool, amount: Int) -> Bool {
        guard let area = path(pid, at: point).first(where: { $0.info.role == "AXScrollArea" }),
              let bar: AXUIElement = attribute(area.element, vertical ? kAXVerticalScrollBarAttribute
                                                                      : kAXHorizontalScrollBarAttribute),
              let current: NSNumber = attribute(bar, kAXValueAttribute) else { return false }
        let next = ScrollPlan.value(from: current.doubleValue, increasing: increasing, amount: amount)
        return AXUIElementSetAttributeValue(bar, kAXValueAttribute as CFString, NSNumber(value: next)) == .success
    }

    // MARK: reading

    private static func windows(_ pid: pid_t) -> [AXUIElement] {
        attribute(application(pid), kAXWindowsAttribute) ?? []
    }

    private static func children(_ element: AXUIElement) -> [AXUIElement] {
        attribute(element, kAXChildrenAttribute) ?? []
    }

    /// Current state of one element, for act.
    static func node(_ element: AXUIElement) -> Node { describe(element, depth: 0) }

    private static func describe(_ element: AXUIElement, depth: Int) -> Node {
        let role: String = attribute(element, kAXRoleAttribute) ?? ""
        let subrole: String = attribute(element, kAXSubroleAttribute) ?? ""
        var names: CFArray?
        let actions = AXUIElementCopyActionNames(element, &names) == .success ? (names as? [String] ?? []) : []
        let info = AXNodeInfo(role: role, subrole: subrole, actions: actions)
        let name = [kAXTitleAttribute, kAXDescriptionAttribute, kAXPlaceholderValueAttribute, kAXHelpAttribute]
            .lazy.compactMap { (key: String) -> String? in self.attribute(element, key) }
            .first { !$0.isEmpty } ?? ""
        var value: String?
        if !info.isSecureText, let raw: CFTypeRef = attribute(element, kAXValueAttribute) {
            if let text = raw as? String { value = text } else if let number = raw as? NSNumber { value = number.stringValue }
        }
        return Node(element: element, depth: depth, info: info, name: name, value: value,
                    frame: frame(element), focused: attribute(element, kAXFocusedAttribute) ?? false,
                    enabled: attribute(element, kAXEnabledAttribute) ?? true)
    }

    private static func frame(_ element: AXUIElement) -> CGRect? {
        guard let position: AXValue = attribute(element, kAXPositionAttribute),
              let size: AXValue = attribute(element, kAXSizeAttribute) else { return nil }
        var origin = CGPoint.zero
        var extent = CGSize.zero
        guard AXValueGetValue(position, .cgPoint, &origin), AXValueGetValue(size, .cgSize, &extent) else { return nil }
        return CGRect(origin: origin, size: extent)
    }

    private static func attribute<T>(_ element: AXUIElement, _ name: String) -> T? {
        var value: CFTypeRef?
        guard AXUIElementCopyAttributeValue(element, name as CFString, &value) == .success, let value else { return nil }
        if T.self == AXUIElement.self || T.self == AXValue.self {
            guard CFGetTypeID(value) == (T.self == AXUIElement.self ? AXUIElementGetTypeID() : AXValueGetTypeID()) else {
                return nil
            }
            return (value as! T)
        }
        if T.self == [AXUIElement].self {
            return (value as? [AnyObject])?.compactMap { item -> AXUIElement? in
                CFGetTypeID(item) == AXUIElementGetTypeID() ? (item as! AXUIElement) : nil
            } as? T
        }
        return value as? T
    }
}
