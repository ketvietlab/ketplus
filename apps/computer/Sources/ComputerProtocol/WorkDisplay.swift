import CoreGraphics

/// The display the agent works on (ADR 0028): screenshots show it, and granted
/// apps' windows move there, so the user keeps the main display.
public enum DisplayPreference: String, Sendable {
    /// The first display that is not the main one; the main display when it is alone.
    case secondary
    case main

    /// The `display` setting; anything else means the default.
    public init(setting: String?) {
        self = DisplayPreference(rawValue: setting ?? "") ?? .secondary
    }
}

public struct DisplayInfo: Equatable, Sendable {
    public let id: UInt32
    public let name: String
    /// Global points, top-left origin.
    public let bounds: CGRect
    /// Bounds without the menu bar and Dock, global points, top-left origin.
    public let visibleFrame: CGRect
    public let isMain: Bool

    public init(id: UInt32, name: String, bounds: CGRect, visibleFrame: CGRect, isMain: Bool) {
        self.id = id
        self.name = name
        self.bounds = bounds
        self.visibleFrame = visibleFrame
        self.isMain = isMain
    }
}

public enum WorkDisplay {
    public static func choose(_ displays: [DisplayInfo], preference: DisplayPreference) -> DisplayInfo? {
        let main = displays.first(where: \.isMain) ?? displays.first
        guard preference == .secondary else { return main }
        return displays.first { !$0.isMain } ?? main
    }

    /// Display for a `display` tool argument: "work" or nil, "main", or a display ID.
    public static func resolve(_ argument: String?, displays: [DisplayInfo], work: DisplayInfo?) -> DisplayInfo? {
        switch argument {
        case nil, "", "work": return work
        case "main": return displays.first(where: \.isMain)
        case let value?: return UInt32(value).flatMap { id in displays.first { $0.id == id } }
        }
    }

    /// True when the window's centre lies on the display.
    public static func isOn(_ window: CGRect, _ display: DisplayInfo) -> Bool {
        display.bounds.contains(CGPoint(x: window.midX, y: window.midY))
    }

    /// Where a window moved to the display goes: its size, shrunk to fit the visible
    /// frame, cascaded by `index` so that several windows stay distinguishable.
    public static func place(_ window: CGRect, on display: DisplayInfo, index: Int) -> CGRect {
        let area = display.visibleFrame
        let size = CGSize(width: min(window.width, area.width), height: min(window.height, area.height))
        let step = CGFloat(28 * (index % 8))
        let x = min(area.minX + 40 + step, area.maxX - size.width)
        let y = min(area.minY + 40 + step, area.maxY - size.height)
        return CGRect(origin: CGPoint(x: max(area.minX, x), y: max(area.minY, y)), size: size)
    }
}
