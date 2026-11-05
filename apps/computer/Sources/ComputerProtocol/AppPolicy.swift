/// Built-in application tiers (ADR 0030). The agent app enforces them; model
/// guidance cannot change them.
public enum AppTier: String, Sendable {
    /// Never captured or controlled.
    case blocked
    /// Captured but not clicked or typed into.
    case viewOnly = "view_only"
    /// Captured and clicked, never typed into.
    case clickOnly = "click_only"
    case full
}

public enum ActionKind: Sendable {
    /// screenshot, zoom
    case view
    /// clicks, drags, scrolls, pointer moves
    case pointer
    /// type, key
    case keyboard
}

public enum AppPolicy {
    public static let ketPlusBundleIdentifiers: Set<String> = ["dev.ketplus.editor", AgentPaths.bundleIdentifier]

    /// Credentials, passwords and KetPlus itself (the agent must not approve its own grants).
    static let blocked: Set<String> = [
        "com.apple.keychainaccess", "com.apple.Passwords",
        "com.1password.1password", "com.agilebits.onepassword7", "com.bitwarden.desktop",
        "com.lastpass.LastPass", "com.dashlane.dashlanephonefinal", "in.sinew.Enpass-Desktop",
    ]

    /// Browsers other than Safari: Chromium work belongs to ketplus_browser (ADR 0014).
    static let viewOnly: Set<String> = [
        "com.google.Chrome", "com.google.Chrome.canary", "com.microsoft.edgemac", "org.mozilla.firefox",
        "company.thebrowser.Browser", "com.brave.Browser", "com.operasoftware.Opera",
        "com.vivaldi.Vivaldi", "org.chromium.Chromium", "com.google.chrome.for.testing",
    ]

    /// Terminals and IDEs: typing into them is equivalent to shell access.
    static let clickOnly: Set<String> = [
        "com.apple.Terminal", "com.googlecode.iterm2", "dev.warp.Warp-Stable", "net.kovidgoyal.kitty",
        "io.alacritty", "com.mitchellh.ghostty", "com.microsoft.VSCode", "com.todesktop.230313mzl4w4u92",
        "com.apple.dt.Xcode", "dev.zed.Zed", "com.sublimetext.4",
    ]

    static let elevated: [String: String] = [
        "com.apple.finder": "Can read or write any file, and controls the desktop and Dock.",
        "com.apple.systempreferences": "Can change system settings.",
    ]

    public static func tier(for bundleIdentifier: String, denied: Set<String> = []) -> AppTier {
        if ketPlusBundleIdentifiers.contains(bundleIdentifier) || blocked.contains(bundleIdentifier)
            || denied.contains(bundleIdentifier) || bundleIdentifier.isEmpty {
            return .blocked
        }
        if viewOnly.contains(bundleIdentifier) { return .viewOnly }
        if clickOnly.contains(bundleIdentifier) || bundleIdentifier.hasPrefix("com.jetbrains.") { return .clickOnly }
        return .full
    }

    public static func allows(_ tier: AppTier, _ action: ActionKind) -> Bool {
        switch (tier, action) {
        case (.blocked, _): return false
        case (.viewOnly, .view): return true
        case (.viewOnly, _): return false
        case (.clickOnly, .keyboard): return false
        case (.clickOnly, _), (.full, _): return true
        }
    }

    /// Text shown in the approval panel for apps with broad reach.
    public static func warning(for bundleIdentifier: String) -> String? {
        if let text = elevated[bundleIdentifier] { return text }
        switch tier(for: bundleIdentifier) {
        case .clickOnly: return "Terminal or IDE: clicks only, no typing (typing would equal shell access)."
        case .viewOnly: return "Browser: view only. Chromium work uses the KetPlus browser."
        default: return nil
        }
    }
}
