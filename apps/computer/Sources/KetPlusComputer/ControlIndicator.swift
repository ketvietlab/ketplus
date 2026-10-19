import AppKit

/// A click-through banner shown while a conversation holds the control lease.
/// It belongs to this agent, so screenshots never include it.
@MainActor
final class ControlIndicator {
    private var panel: NSPanel?
    private let label = NSTextField(labelWithString: "")

    func show(apps: [String]) {
        let text = apps.isEmpty
            ? "KetPlus agent is controlling this Mac · press Esc to stop"
            : "KetPlus agent is controlling \(apps.joined(separator: ", ")) · press Esc to stop"
        let panel = self.panel ?? makePanel()
        label.stringValue = text
        label.sizeToFit()
        let width = label.frame.width + 32
        if let screen = NSScreen.main {
            let frame = screen.visibleFrame
            panel.setFrame(NSRect(x: frame.midX - width / 2, y: frame.maxY - 44, width: width, height: 32), display: true)
        }
        label.frame.origin = NSPoint(x: 16, y: (32 - label.frame.height) / 2)
        panel.orderFrontRegardless()
        self.panel = panel
    }

    func hide() {
        panel?.orderOut(nil)
    }

    /// The banner window, for `--render-previews`.
    var window: NSWindow? { panel }

    private func makePanel() -> NSPanel {
        let panel = NSPanel(contentRect: .zero, styleMask: [.nonactivatingPanel, .borderless],
                            backing: .buffered, defer: false)
        panel.level = .statusBar
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.ignoresMouseEvents = true
        panel.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle]
        let background = NSVisualEffectView()
        background.material = .hudWindow
        background.state = .active
        background.wantsLayer = true
        background.layer?.cornerRadius = 10
        label.font = .systemFont(ofSize: 13, weight: .semibold)
        label.textColor = .labelColor
        background.addSubview(label)
        panel.contentView = background
        return panel
    }
}
