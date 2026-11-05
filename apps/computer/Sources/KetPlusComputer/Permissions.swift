import ApplicationServices
import CoreGraphics
import Foundation

/// TCC state of this process. The agent app is the only KetPlus process that
/// asks for these grants (ADR 0030).
enum Permissions {
    static var screenRecording: Bool { CGPreflightScreenCaptureAccess() }
    static var accessibility: Bool { AXIsProcessTrusted() }

    /// Shows the system prompt once per process; later calls only report state.
    @discardableResult
    static func requestScreenRecording() -> Bool {
        screenRecording || CGRequestScreenCaptureAccess()
    }

    @discardableResult
    static func requestAccessibility() -> Bool {
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        return AXIsProcessTrustedWithOptions(options)
    }

    /// Asks macOS for whichever grant is missing, so the first use shows the system prompts
    /// that Settings → Computer use describes. Prompts run on the main thread without blocking
    /// the tool call; the agent retries after the user answers.
    static func promptForMissing(screenRecording needsScreen: Bool, accessibility needsAccessibility: Bool) {
        DispatchQueue.main.async {
            if needsScreen && !screenRecording { requestScreenRecording() }
            if needsAccessibility && !accessibility { requestAccessibility() }
        }
    }
}
