import Foundation

/// Where the agent app publishes its socket and per-launch token.
///
/// The directory is created 0700 and the token file 0600, so only the signed-in
/// user can reach the agent. The spike uses one per-launch token; ADR 0026 moves
/// this to per-conversation tokens issued by the daemon.
public struct AgentPaths: Sendable {
    public static let bundleIdentifier = "dev.ketplus.computer"
    public static let protocolVersion = "computer.v0"

    public let directory: URL

    public init(directory: URL) {
        self.directory = directory
    }

    public static func standard() -> AgentPaths {
        if let override = ProcessInfo.processInfo.environment["KETPLUS_COMPUTER_DIR"], !override.isEmpty {
            return AgentPaths(directory: URL(fileURLWithPath: override, isDirectory: true))
        }
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return AgentPaths(directory: support.appendingPathComponent("Ketsuite/KetPlus/computer", isDirectory: true))
    }

    public var socket: URL { directory.appendingPathComponent("agent.sock") }
    public var token: URL { directory.appendingPathComponent("agent.token") }
}
