import AppKit
import ComputerProtocol

/// `KetPlusComputer --render-previews <dir>` draws the approval alert and the control
/// banner into PNGs in light and dark appearance, from this process's own views, so UI
/// changes can be checked without Screen Recording.
@MainActor
enum Previews {
    static func render(into directory: URL) throws {
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let request = ApprovalPanel.Request(
            conversation: "preview-conversation",
            apps: [("Simulator", "com.apple.iphonesimulator"), ("Xcode", "com.apple.dt.Xcode"),
                   ("Finder", "com.apple.finder")],
            reason: "Launch the app in the iOS Simulator and tap through onboarding.")
        let indicator = ControlIndicator()
        let cursor = VirtualCursor()
        for (name, appearance) in [("light", NSAppearance.Name.aqua), ("dark", .darkAqua)] {
            NSApp.appearance = NSAppearance(named: appearance)
            let alert = ApprovalPanel.makeAlert(request)
            alert.layout()
            try write(alert.window, to: directory.appendingPathComponent("approval-\(name).png"))
            indicator.show(apps: ["Simulator", "Xcode"])
            try write(indicator.window, to: directory.appendingPathComponent("indicator-\(name).png"))
            indicator.hide()
            cursor.glide(to: CGPoint(x: 400, y: 400))
            try write(cursor.window, to: directory.appendingPathComponent("cursor-\(name).png"))
            cursor.hide()
        }
    }

    private static func write(_ window: NSWindow?, to url: URL) throws {
        guard let window, let view = window.contentView,
              let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else {
            throw AgentFailure(code: "PREVIEW_FAILED", message: "Nothing to render for \(url.lastPathComponent).")
        }
        view.cacheDisplay(in: view.bounds, to: rep)
        // The window server draws the alert's backdrop, so composite the content onto
        // the appearance's window colour; otherwise dark text sits on transparency.
        let image = NSImage(size: view.bounds.size)
        image.lockFocus()
        view.effectiveAppearance.performAsCurrentDrawingAppearance {
            NSColor.windowBackgroundColor.setFill()
            view.bounds.fill()
        }
        rep.draw(in: view.bounds, from: .zero, operation: .sourceOver, fraction: 1,
                 respectFlipped: false, hints: nil)
        image.unlockFocus()
        guard let tiff = image.tiffRepresentation, let output = NSBitmapImageRep(data: tiff),
              let data = output.representation(using: .png, properties: [:]) else {
            throw AgentFailure(code: "PREVIEW_FAILED", message: "PNG encoding failed.")
        }
        try data.write(to: url)
    }
}
