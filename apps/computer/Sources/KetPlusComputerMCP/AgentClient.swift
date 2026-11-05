import ComputerProtocol
import Foundation

struct AgentCallError: Error {
    let code: String
    let message: String
}

/// Forwards computer.v0 calls to the agent app, starting it through
/// LaunchServices so that it is its own responsible process for TCC. The daemon
/// supplies the conversation and its token in the server's environment; a local
/// client outside KetPlus mints its own from the user-only host key instead.
final class AgentClient {
    private let paths = AgentPaths.standard()
    private let conversation: String
    private let token: String
    private var connection: LineConnection?
    private var nextID = 1

    init(environment: [String: String] = ProcessInfo.processInfo.environment, localClient: String? = nil) throws {
        guard let localClient else {
            conversation = environment["KETPLUS_COMPUTER_CONVERSATION"] ?? ""
            token = environment["KETPLUS_COMPUTER_TOKEN"] ?? ""
            return
        }
        guard let local = ConversationToken.localConversation(localClient) else {
            throw AgentCallError(code: "INVALID_PARAMS", message: "--local-client needs a name of letters, digits, '.', '_' or '-'.")
        }
        // Anyone who can read host.key can already mint tokens; it is 0600 and owned by this user.
        guard let key = try? Data(contentsOf: paths.hostKey), !key.isEmpty else {
            throw AgentCallError(code: "UNAUTHORIZED", message: "No host key at \(paths.hostKey.path). Open KetPlus once to create it.")
        }
        conversation = local
        token = ConversationToken.mint(key: key, conversation: local)
    }

    func call(_ method: String, _ params: [String: Any] = [:]) throws -> [String: Any] {
        do {
            return try send(method, params)
        } catch is SocketError {
            // The agent may have restarted; reconnect once.
            connection = nil
            return try send(method, params)
        }
    }

    private func send(_ method: String, _ params: [String: Any]) throws -> [String: Any] {
        let connection = try connected()
        let id = nextID
        nextID += 1
        try connection.writeObject(["id": id, "conversation": conversation, "token": token,
                                    "method": method, "params": params])
        let response = try connection.readObject()
        if let error = response["error"] as? [String: Any] {
            throw AgentCallError(code: error["code"] as? String ?? "INTERNAL",
                                 message: error["message"] as? String ?? "The agent failed.")
        }
        return response["result"] as? [String: Any] ?? [:]
    }

    private func connected() throws -> LineConnection {
        if let connection { return connection }
        if let ready = try? LineConnection.connect(to: paths.socket.path) {
            connection = ready
            return ready
        }
        try launchAgent()
        let deadline = Date().addingTimeInterval(10)
        while Date() < deadline {
            if let ready = try? LineConnection.connect(to: paths.socket.path) {
                connection = ready
                return ready
            }
            usleep(100_000)
        }
        throw AgentCallError(code: "AGENT_UNAVAILABLE", message: "KetPlus Computer did not start within 10 seconds.")
    }

    private func launchAgent() throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        // -g keeps the user's frontmost app; -a goes through LaunchServices, which does not
        // pass this process's environment, so forward the directory override explicitly.
        var arguments = ["-g"]
        if let directory = ProcessInfo.processInfo.environment["KETPLUS_COMPUTER_DIR"], !directory.isEmpty {
            arguments += ["--env", "KETPLUS_COMPUTER_DIR=\(directory)"]
        }
        process.arguments = arguments + ["-a", agentBundle().path]
        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            throw AgentCallError(code: "AGENT_UNAVAILABLE", message: "Could not open \(agentBundle().path).")
        }
    }

    /// KetPlus Computer.app/Contents/MacOS/ketplus-computer-mcp -> KetPlus Computer.app
    private func agentBundle() -> URL {
        let executable = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
        return executable.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    }
}
