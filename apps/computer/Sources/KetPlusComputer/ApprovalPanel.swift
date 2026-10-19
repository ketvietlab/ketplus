import AppKit
import ComputerProtocol

/// The native approval that grants apps to one conversation. It belongs to this
/// agent, which is never captured or controllable, so the model can neither see
/// nor press it (ADR 0026).
@MainActor
enum ApprovalPanel {
    static let timeout: TimeInterval = 120

    struct Request {
        let conversation: String
        let apps: [(name: String, bundleIdentifier: String)]
        let reason: String
    }

    /// Blocks until the user answers or the timeout passes (a denial).
    static func ask(_ request: Request) -> Bool {
        #if KETPLUS_COMPUTER_TESTING
        // Test builds only (build-app.sh --testing): release builds never compile this.
        if let answer = ProcessInfo.processInfo.environment["KETPLUS_COMPUTER_TEST_APPROVAL"] {
            return answer == "allow"
        }
        #endif
        let alert = makeAlert(request)
        NSApp.activate(ignoringOtherApps: true)
        let timer = Timer(timeInterval: timeout, repeats: false) { _ in
            NSApp.abortModal()
        }
        RunLoop.main.add(timer, forMode: .modalPanel)
        defer { timer.invalidate() }
        return alert.runModal() == .alertFirstButtonReturn
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
        lines.append("\nAccess lasts for this conversation until you press Esc. Other apps stay hidden from the agent.")
        alert.informativeText = lines.joined(separator: "\n")
        alert.addButton(withTitle: "Allow for this conversation")
        alert.addButton(withTitle: "Deny")
        alert.buttons[1].keyEquivalent = "\u{1b}"
        alert.buttons[0].keyEquivalent = ""

        return alert
    }
}
