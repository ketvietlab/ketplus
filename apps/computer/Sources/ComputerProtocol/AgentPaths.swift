import Foundation

/// Files shared by the daemon, the MCP front and the agent app.
///
/// `<KetPlus data>/computer` is 0700. It holds the agent socket, the host key the
/// daemon uses to mint per-conversation tokens (0600), markers for conversations
/// whose running turn was started remotely, and per-conversation audit logs.
/// modules/computer derives the same paths (ADR 0026).
public struct AgentPaths: Sendable {
    public static let bundleIdentifier = "dev.ketplus.computer"
    public static let protocolVersion = "computer.v0"

    public let directory: URL
    public let settingsFile: URL

    public init(directory: URL, settingsFile: URL) {
        self.directory = directory
        self.settingsFile = settingsFile
    }

    public static func standard(environment: [String: String] = ProcessInfo.processInfo.environment) -> AgentPaths {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dataRoot = support.appendingPathComponent("Ketsuite/KetPlus", isDirectory: true)
        let directory = environment["KETPLUS_COMPUTER_DIR"].flatMap { $0.isEmpty ? nil : $0 }
            .map { URL(fileURLWithPath: $0, isDirectory: true) }
            ?? dataRoot.appendingPathComponent("computer", isDirectory: true)
        let settings = environment["KETPLUS_COMPUTER_TOOLS_CONFIG"].flatMap { $0.isEmpty ? nil : $0 }
            .map { URL(fileURLWithPath: $0) }
            ?? dataRoot.appendingPathComponent("computer-tools.json")
        return AgentPaths(directory: directory, settingsFile: settings)
    }

    public var socket: URL { directory.appendingPathComponent("agent.sock") }
    public var hostKey: URL { directory.appendingPathComponent("host.key") }

    public func remoteTurnMarker(conversation: String) -> URL {
        directory.appendingPathComponent("remote-turns", isDirectory: true)
            .appendingPathComponent(ConversationToken.fileStem(conversation))
    }

    public func auditLog(conversation: String) -> URL {
        directory.appendingPathComponent("audit", isDirectory: true)
            .appendingPathComponent(ConversationToken.fileStem(conversation) + ".jsonl")
    }
}
