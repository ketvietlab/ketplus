import Foundation

/// Pure decisions for background control (ADR 0031): which accessibility action a
/// click becomes, snapshot references, the kill-switch scope and scroll steps.
/// The agent app supplies the accessibility data; these rules are unit-tested.

/// What the agent needs to know about one accessibility element.
public struct AXNodeInfo: Equatable, Sendable {
    public var role: String
    public var subrole: String
    public var actions: [String]

    public init(role: String, subrole: String = "", actions: [String] = []) {
        self.role = role
        self.subrole = subrole
        self.actions = actions
    }

    public var isSecureText: Bool { subrole == "AXSecureTextField" }
    public var isTextInput: Bool {
        ["AXTextField", "AXTextArea", "AXComboBox", "AXSearchField"].contains(role)
    }
}

/// An accessibility action that replaces a pointer event.
public enum BackgroundAction: String, Equatable, Sendable {
    case press, focus, select
    case showMenu = "show_menu"
}

public enum BackgroundPlanner {
    /// The action for a click at a point, given the element path there, deepest
    /// element first. Nil means no element can take the click without the real pointer.
    public static func click(path: [AXNodeInfo], rightButton: Bool) -> (index: Int, action: BackgroundAction)? {
        for (index, node) in path.enumerated() {
            if node.role == "AXWindow" || node.role == "AXApplication" { return nil }
            if rightButton {
                if node.actions.contains("AXShowMenu") { return (index, .showMenu) }
                continue
            }
            if node.isTextInput { return (index, .focus) }
            if node.actions.contains("AXPress") { return (index, .press) }
            if node.role == "AXRow" || node.role == "AXCell" { return (index, .select) }
        }
        return nil
    }
}

/// Element references from the latest `observe` of a conversation. Any input
/// action or a newer observation makes earlier references stale, as in browser.v0.
public final class ReferenceTable<Element>: @unchecked Sendable {
    public enum Failure: Error, Equatable {
        case stale
        case unknownReference
    }

    private let lock = NSLock()
    private var counter = 0
    private var snapshots: [String: (id: String, app: String, elements: [String: Element])] = [:]

    public init() {}

    /// Stores a new snapshot for the conversation and returns its ID.
    public func store(_ elements: [String: Element], app: String, conversation: String) -> String {
        lock.withLock {
            counter += 1
            let id = "s\(counter)"
            snapshots[conversation] = (id, app, elements)
            return id
        }
    }

    public func resolve(snapshot: String, reference: String, conversation: String) throws -> (Element, app: String) {
        try lock.withLock {
            guard let current = snapshots[conversation], current.id == snapshot else { throw Failure.stale }
            guard let element = current.elements[reference] else { throw Failure.unknownReference }
            return (element, current.app)
        }
    }

    public func invalidate(_ conversation: String) {
        lock.withLock { _ = snapshots.removeValue(forKey: conversation) }
    }
}

public enum KillSwitchPolicy {
    /// How long after real-pointer input Esc stops the agent wherever the user is.
    public static let foregroundWindow: TimeInterval = 15

    /// Esc stops the agent when the user is in an app the agent controls, or just
    /// after the agent used the real pointer. Otherwise it belongs to the user's app.
    public static func consumesEscape(frontmostIsGranted: Bool, secondsSinceForeground: TimeInterval?) -> Bool {
        if frontmostIsGranted { return true }
        guard let seconds = secondsSinceForeground else { return false }
        return seconds < foregroundWindow
    }
}

public enum KeyboardTarget {
    /// Explicit app, else the app the conversation last acted on, else the frontmost app.
    public static func resolve(explicit: String?, lastTarget: String?, frontmost: String) -> String {
        if let explicit, !explicit.isEmpty { return explicit }
        if let lastTarget, !lastTarget.isEmpty { return lastTarget }
        return frontmost
    }
}

/// Menu items carry their shortcut, so a background app can run one through its menu
/// bar: events posted to an inactive app skip its key equivalents.
public enum MenuShortcut {
    /// `AXMenuItemCmdModifiers` bits: shift 1, option 2, control 4, no command 8.
    public static func matches(itemCharacter: String, itemModifiers: Int, combo: KeyCombo) -> Bool {
        guard let character = combo.character, !itemCharacter.isEmpty,
              itemCharacter.uppercased() == character else { return false }
        var expected = 0
        if combo.modifiers.contains(.shift) { expected |= 1 }
        if combo.modifiers.contains(.option) { expected |= 2 }
        if combo.modifiers.contains(.control) { expected |= 4 }
        if !combo.modifiers.contains(.command) { expected |= 8 }
        return itemModifiers == expected
    }
}

public enum ScrollPlan {
    /// Fraction of a scroll bar moved per wheel click.
    public static let step = 0.05

    /// New scroll bar value (0...1). Down and right increase it.
    public static func value(from current: Double, increasing: Bool, amount: Int) -> Double {
        let delta = Double(amount) * step * (increasing ? 1 : -1)
        return min(1, max(0, current + delta))
    }
}
