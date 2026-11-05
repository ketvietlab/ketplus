import AppKit

/// The agent's own pointer (ADR 0027): a click-through overlay that glides to each
/// background action, so the user sees where the agent works while the real
/// pointer stays theirs. It belongs to this agent, so screenshots never include it.
@MainActor
final class VirtualCursor {
    private var panel: NSPanel?
    private static let size = NSSize(width: 92, height: 34)

    /// Glides to a global top-left point and stays there until hidden.
    func glide(to point: CGPoint) {
        let panel = self.panel ?? makePanel()
        let primaryHeight = NSScreen.screens.first?.frame.height ?? 0
        // The arrow tip sits at the panel's top-left corner.
        let origin = NSPoint(x: point.x, y: primaryHeight - point.y - Self.size.height)
        if !panel.isVisible {
            panel.setFrameOrigin(NSPoint(x: origin.x - 40, y: origin.y + 40))
            panel.orderFrontRegardless()
        }
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.25
            context.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            panel.animator().setFrameOrigin(origin)
        }
        self.panel = panel
    }

    func hide() {
        panel?.orderOut(nil)
    }

    private func makePanel() -> NSPanel {
        let panel = NSPanel(contentRect: NSRect(origin: .zero, size: Self.size),
                            styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.level = .statusBar
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.ignoresMouseEvents = true
        panel.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle]
        let content = NSView(frame: NSRect(origin: .zero, size: Self.size))
        let arrow = NSImageView(frame: NSRect(x: 0, y: Self.size.height - 22, width: 18, height: 22))
        let symbol = NSImage.SymbolConfiguration(pointSize: 18, weight: .semibold)
            .applying(.init(paletteColors: [.systemIndigo]))
        arrow.image = NSImage(systemSymbolName: "cursorarrow", accessibilityDescription: nil)?
            .withSymbolConfiguration(symbol)
        let label = NSTextField(labelWithString: "KetPlus")
        label.font = .systemFont(ofSize: 11, weight: .semibold)
        label.textColor = .white
        label.alignment = .center
        label.wantsLayer = true
        label.drawsBackground = true
        label.backgroundColor = .systemIndigo
        label.layer?.cornerRadius = 6
        label.layer?.masksToBounds = true
        label.frame = NSRect(x: 16, y: 0, width: 60, height: 18)
        content.addSubview(arrow)
        content.addSubview(label)
        panel.contentView = content
        return panel
    }

    /// The overlay window, for `--render-previews`.
    var window: NSWindow? { panel }
}
