import ApplicationServices
import CoreGraphics

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
}
