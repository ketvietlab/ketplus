/// Parses toolset key strings ("Return", "ctrl+s", "super+shift+Tab") into a
/// macOS virtual key code and modifiers. Names follow xdotool keysyms, as the
/// computer_toolset_20260801 `key` member does.
public struct KeyCombo: Equatable, Sendable {
    public struct Modifiers: OptionSet, Sendable {
        public let rawValue: Int
        public init(rawValue: Int) { self.rawValue = rawValue }
        public static let shift = Modifiers(rawValue: 1)
        public static let control = Modifiers(rawValue: 2)
        public static let option = Modifiers(rawValue: 4)
        public static let command = Modifiers(rawValue: 8)
        public static let function = Modifiers(rawValue: 16)
    }

    public let keyCode: UInt16?
    public let modifiers: Modifiers
    /// The printed character of a single-character key, uppercased ("A", "/"), as menu
    /// items show it. Nil for named keys such as Return.
    public let character: String?

    public init(keyCode: UInt16?, modifiers: Modifiers, character: String? = nil) {
        self.keyCode = keyCode
        self.modifiers = modifiers
        self.character = character
    }

    public static func parseModifiers(_ text: String) -> Modifiers? {
        if text.trimmingCharacters(in: .whitespaces).isEmpty { return [] }
        var result: Modifiers = []
        for part in text.lowercased().split(separator: "+") {
            guard let modifier = modifierNames[String(part).trimmingCharacters(in: .whitespaces)] else { return nil }
            result.insert(modifier)
        }
        return result
    }

    /// nil when a part is unknown. A combination of modifiers only has a nil keyCode.
    public static func parse(_ text: String) -> KeyCombo? {
        if text == "+" { return KeyCombo(keyCode: keyCodes["plus"], modifiers: [.shift]) }
        let parts = text.split(separator: "+", omittingEmptySubsequences: false).map(String.init)
        guard !parts.contains(where: \.isEmpty) else { return nil }
        var modifiers: Modifiers = []
        for part in parts.dropLast() {
            guard let modifier = modifierNames[part.lowercased()] else { return nil }
            modifiers.insert(modifier)
        }
        let last = parts.last!
        if let modifier = modifierNames[last.lowercased()] {
            return KeyCombo(keyCode: nil, modifiers: modifiers.union(modifier))
        }
        guard let code = keyCodes[last] ?? keyCodes[last.lowercased()] else { return nil }
        return KeyCombo(keyCode: code, modifiers: modifiers, character: last.count == 1 ? last.uppercased() : nil)
    }

    static let modifierNames: [String: Modifiers] = [
        "shift": .shift, "ctrl": .control, "control": .control, "alt": .option, "option": .option,
        "super": .command, "cmd": .command, "command": .command, "meta": .command, "fn": .function,
    ]

    /// US ANSI virtual key codes (Carbon kVK_*).
    static let keyCodes: [String: UInt16] = {
        var map: [String: UInt16] = [
            "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7, "c": 8, "v": 9, "b": 11,
            "q": 12, "w": 13, "e": 14, "r": 15, "y": 16, "t": 17, "1": 18, "2": 19, "3": 20, "4": 21,
            "6": 22, "5": 23, "=": 24, "equal": 24, "9": 25, "7": 26, "-": 27, "minus": 27, "8": 28, "0": 29,
            "]": 30, "bracketright": 30, "o": 31, "u": 32, "[": 33, "bracketleft": 33, "i": 34, "p": 35,
            "l": 37, "j": 38, "'": 39, "apostrophe": 39, "k": 40, ";": 41, "semicolon": 41,
            "\\": 42, "backslash": 42, ",": 43, "comma": 43, "/": 44, "slash": 44, "n": 45, "m": 46,
            ".": 47, "period": 47, "`": 50, "grave": 50, "plus": 24,
            "Return": 36, "return": 36, "Enter": 36, "enter": 36, "KP_Enter": 76,
            "Tab": 48, "tab": 48, "space": 49, "Space": 49, "BackSpace": 51, "backspace": 51,
            "Escape": 53, "escape": 53, "Esc": 53, "esc": 53, "Delete": 117, "delete": 117,
            "Home": 115, "home": 115, "End": 119, "end": 119, "Page_Up": 116, "Prior": 116,
            "Page_Down": 121, "Next": 121, "Left": 123, "Right": 124, "Down": 125, "Up": 126,
            "left": 123, "right": 124, "down": 125, "up": 126,
        ]
        let functionKeys: [UInt16] = [122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111]
        for (index, code) in functionKeys.enumerated() {
            map["F\(index + 1)"] = code
            map["f\(index + 1)"] = code
        }
        return map
    }()
}
