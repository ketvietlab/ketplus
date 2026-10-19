import ComputerProtocol
import Foundation

/// One JSON line per action in `<computer>/audit/<conversation stem>.jsonl` (0600).
/// Typed text is recorded by length only; images are never written.
struct AuditLog {
    let paths: AgentPaths

    func record(conversation: String, tool: String, target: String?, outcome: String, detail: [String: Any] = [:]) {
        var entry: [String: Any] = [
            "time": ISO8601DateFormatter().string(from: Date()),
            "tool": tool,
            "outcome": outcome,
        ]
        if let target { entry["app"] = target }
        entry.merge(detail) { current, _ in current }
        guard var line = try? JSONSerialization.data(withJSONObject: entry, options: [.sortedKeys]) else { return }
        line.append(0x0A)
        let url = paths.auditLog(conversation: conversation)
        let manager = FileManager.default
        try? manager.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true,
                                     attributes: [.posixPermissions: 0o700])
        if !manager.fileExists(atPath: url.path) {
            manager.createFile(atPath: url.path, contents: nil, attributes: [.posixPermissions: 0o600])
        }
        guard let handle = try? FileHandle(forWritingTo: url) else { return }
        defer { try? handle.close() }
        _ = try? handle.seekToEnd()
        try? handle.write(contentsOf: line)
    }
}
