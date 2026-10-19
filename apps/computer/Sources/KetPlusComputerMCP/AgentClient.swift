import ComputerProtocol
import Foundation

struct AgentCallError: Error {
    let code: String
    let message: String
}

/// Forwards computer.v0 calls to the agent app, starting it through
/// LaunchServices so that it is its own responsible process for TCC.
final class AgentClient {
    private let paths = AgentPaths.standard()
    private var connection: LineConnection?
    private var token = ""
    private var nextID = 1

    func call(_ method: String, _ params: [String: Any] = [:]) throws -> [String: Any] {
        do {
            return try send(method, params)
        } catch is SocketError {
            // The agent may have restarted with a new token; reconnect once.
            connection = nil
            return try send(method, params)
        }
    }

    private func send(_ method: String, _ params: [String: Any]) throws -> [String: Any] {
        let connection = try connected()
        let id = nextID
        nextID += 1
        try connection.writeObject(["id": id, "token": token, "method": method, "params": params])
        let response = try connection.readObject()
        if let error = response["error"] as? [String: Any] {
            throw AgentCallError(code: error["code"] as? String ?? "INTERNAL",
                                 message: error["message"] as? String ?? "The agent failed.")
        }
        return response["result"] as? [String: Any] ?? [:]
    }

    private func connected() throws -> LineConnection {
        if let connection { return connection }
        if let ready = try? open() { return ready }
        try launchAgent()
        let deadline = Date().addingTimeInterval(10)
        while Date() < deadline {
            if let ready = try? open() { return ready }
            usleep(100_000)
        }
        throw AgentCallError(code: "AGENT_UNAVAILABLE", message: "KetPlus Computer did not start within 10 seconds.")
    }

    private func open() throws -> LineConnection {
        let value = try String(contentsOf: paths.token, encoding: .utf8)
        let ready = try LineConnection.connect(to: paths.socket.path)
        token = value.trimmingCharacters(in: .whitespacesAndNewlines)
        connection = ready
        return ready
    }

    private func launchAgent() throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        // -g keeps the user's frontmost app; -a goes through LaunchServices.
        process.arguments = ["-g", "-a", agentBundle().path]
        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            throw AgentCallError(code: "AGENT_UNAVAILABLE", message: "Could not open \(agentBundle().path).")
        }
    }

    /// KetPlus Computer.app/Contents/MacOS/ketplus-computer-mcp -> KetPlus Computer.app
    private func agentBundle() -> URL {
        if let override = ProcessInfo.processInfo.environment["KETPLUS_COMPUTER_APP"], !override.isEmpty {
            return URL(fileURLWithPath: override)
        }
        let executable = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
        return executable.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    }
}
