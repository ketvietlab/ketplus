import AppKit
import ComputerProtocol

/// The native approval that grants apps to one conversation. It belongs to this
/// agent, which is never captured or controllable, so the model can neither see
/// nor press it (ADR 0026).
@MainActor
enum ApprovalPanel {
    static let timeout: TimeInterval = 120
    /// Allow stays disabled this long after the panel appears, so a click meant
    /// for something else cannot land on it.
    static let armDelay: TimeInterval = 1

    struct Request {
        let conversation: String
        let apps: [(name: String, bundleIdentifier: String)]
        let reason: String
        /// Set when the agent works on another display than the main one.
        var workDisplay: String? = nil
    }

    enum Decision: String {
        case allow, deny
        /// Timed out, or ended by Stop or Esc outside the panel.
        case dismissed
    }

    /// Blocks until the user answers or the timeout passes. Only a press of the
    /// Allow button itself grants access.
    static func ask(_ request: Request) -> Decision {
        #if KETPLUS_COMPUTER_TESTING
        // Test builds only (build-app.sh --testing): release builds never compile this.
        if let answer = ProcessInfo.processInfo.environment["KETPLUS_COMPUTER_TEST_APPROVAL"] {
            return answer == "allow" ? .allow : .deny
        }
        #endif
        let alert = makeAlert(request)
        let buttons = Buttons()
        let allow = alert.buttons[0]
        let deny = alert.buttons[1]
        allow.target = buttons
        allow.action = #selector(Buttons.allow(_:))
        deny.target = buttons
        deny.action = #selector(Buttons.deny(_:))
        allow.isEnabled = false
        NSApp.activate(ignoringOtherApps: true)
        // The modal loop does not service the main queue, so these are run-loop timers.
        let timers = [
            Timer(timeInterval: 0, repeats: false) { _ in
                // Activation can be refused; keep the panel above the user's windows anyway.
                alert.window.level = .modalPanel
                alert.window.orderFrontRegardless()
            },
            Timer(timeInterval: armDelay, repeats: false) { _ in allow.isEnabled = true },
            Timer(timeInterval: timeout, repeats: false) { _ in NSApp.abortModal() },
        ]
        timers.forEach { RunLoop.main.add($0, forMode: .modalPanel) }
        defer { timers.forEach { $0.invalidate() } }
        _ = alert.runModal()
        return buttons.decision
    }

    private final class Buttons: NSObject {
        var decision = Decision.dismissed

        @objc func allow(_ sender: NSButton) {
            decision = .allow
            NSApp.stopModal()
        }

        @objc func deny(_ sender: NSButton) {
            decision = .deny
            NSApp.stopModal()
        }
    }

    /// The approval alert; also rendered by `--render-previews`.
    static func makeAlert(_ request: Request) -> NSAlert {
        let alert = NSAlert()
        alert.alertStyle = .warning
        // The app ships without an icon; without this the alert shows a blank placeholder.
        let symbol = NSImage.SymbolConfiguration(pointSize: 40, weight: .regular)
        alert.icon = NSImage(systemSymbolName: "cursorarrow.click.2", accessibilityDescription: nil)?
            .withSymbolConfiguration(symbol)
        alert.messageText = "Allow a KetPlus agent to control \(request.apps.map(\.name).joined(separator: ", "))?"
        var lines = [
            "Conversation \(String(ConversationToken.fileStem(request.conversation).prefix(8))) asks to see and use:",
        ]
        for app in request.apps {
            var line = "• \(app.name) (\(app.bundleIdentifier))"
            if let warning = AppPolicy.warning(for: app.bundleIdentifier) { line += " — \(warning)" }
            lines.append(line)
        }
        if !request.reason.isEmpty { lines.append("\nReason given by the agent: \(request.reason)") }
        if let display = request.workDisplay {
            lines.append("\nTheir windows move to the \(display) display, where the agent works.")
        }
        lines.append("\nAccess lasts for this conversation until you press Stop or Esc. Other apps stay hidden from the agent.")
        alert.informativeText = lines.joined(separator: "\n")
        alert.addButton(withTitle: "Allow for this conversation")
        alert.addButton(withTitle: "Deny")
        alert.buttons[1].keyEquivalent = "\u{1b}"
        alert.buttons[0].keyEquivalent = ""

        return alert
    }
}
