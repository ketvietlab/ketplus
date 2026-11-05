import AppKit

/// The banner shown while a conversation holds the control lease, with a Stop
/// button. It belongs to this agent, so screenshots never include it.
@MainActor
final class ControlIndicator {
    private var panel: NSPanel?
    private let label = NSTextField(labelWithString: "")
    private let stop = FirstClickButton(title: "Stop", target: nil, action: nil)
    /// Called when the user presses Stop.
    var onStop: (() -> Void)?

    func show(apps: [String]) {
        let text = apps.isEmpty
            ? "KetPlus agent is using this Mac"
            : "KetPlus agent is using \(apps.joined(separator: ", "))"
        let panel = self.panel ?? makePanel()
        label.stringValue = text
        label.sizeToFit()
        stop.sizeToFit()
        let height: CGFloat = 36
        let width = 16 + label.frame.width + 12 + stop.frame.width + 8
        if let screen = NSScreen.main {
            let frame = screen.visibleFrame
            panel.setFrame(NSRect(x: frame.midX - width / 2, y: frame.maxY - height - 12, width: width, height: height),
                           display: true)
        }
        label.frame.origin = NSPoint(x: 16, y: (height - label.frame.height) / 2)
        stop.frame.origin = NSPoint(x: width - stop.frame.width - 8, y: (height - stop.frame.height) / 2)
        panel.orderFrontRegardless()
        self.panel = panel
    }

    func hide() {
        panel?.orderOut(nil)
    }

    /// The banner window, for `--render-previews`.
    var window: NSWindow? { panel }

    @objc private func stopPressed(_ sender: NSButton) {
        onStop?()
    }

    private func makePanel() -> NSPanel {
        let panel = NSPanel(contentRect: .zero, styleMask: [.nonactivatingPanel, .borderless],
                            backing: .buffered, defer: false)
        panel.level = .statusBar
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.becomesKeyOnlyIfNeeded = true
        panel.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle]
        let background = NSVisualEffectView()
        background.material = .hudWindow
        background.state = .active
        background.wantsLayer = true
        background.layer?.cornerRadius = 10
        label.font = .systemFont(ofSize: 13, weight: .semibold)
        label.textColor = .labelColor
        stop.bezelStyle = .rounded
        stop.controlSize = .small
        stop.target = self
        stop.action = #selector(stopPressed(_:))
        background.addSubview(label)
        background.addSubview(stop)
        panel.contentView = background
        return panel
    }
}

/// Stop must work with one click although the banner never becomes key.
private final class FirstClickButton: NSButton {
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}
