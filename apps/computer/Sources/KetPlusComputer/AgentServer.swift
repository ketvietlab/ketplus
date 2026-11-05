import ComputerProtocol
import Darwin
import Foundation

struct AgentFailure: Error {
    let code: String
    let message: String
}

/// Serves computer.v0 over the user-only socket. Every request except `status`
/// must carry a conversation ID and the daemon-minted token for it, and is
/// refused while that conversation's running turn was started remotely.
final class AgentServer {
    private let paths: AgentPaths
    private let tools: AgentTools

    init(paths: AgentPaths, tools: AgentTools) {
        self.paths = paths
        self.tools = tools
    }

    func start() throws {
        let manager = FileManager.default
        try manager.createDirectory(at: paths.directory, withIntermediateDirectories: true,
                                    attributes: [.posixPermissions: 0o700])
        try manager.setAttributes([.posixPermissions: 0o700], ofItemAtPath: paths.directory.path)
        let listener = try LineListener(path: paths.socket.path)
        let thread = Thread { [self] in
            while true {
                guard let connection = try? listener.accept() else { continue }
                Thread.detachNewThread { self.serve(connection) }
            }
        }
        thread.name = "computer.v0 accept"
        thread.start()
    }

    private func serve(_ connection: LineConnection) {
        guard connection.peerUserID() == getuid() else { return }
        while let request = try? connection.readObject() {
            var response: [String: Any] = ["id": request["id"] ?? NSNull()]
            do {
                response["result"] = try handle(request)
            } catch let failure as AgentFailure {
                response["error"] = ["code": failure.code, "message": failure.message]
            } catch {
                response["error"] = ["code": "INTERNAL", "message": String(describing: error)]
            }
            guard (try? connection.writeObject(response)) != nil else { return }
        }
    }

    private func handle(_ request: [String: Any]) throws -> [String: Any] {
        let method = request["method"] as? String ?? ""
        if method == "status" { return tools.status() }
        let conversation = request["conversation"] as? String ?? ""
        let token = request["token"] as? String ?? ""
        let key = (try? Data(contentsOf: paths.hostKey)) ?? Data()
        guard ConversationToken.verify(key: key, conversation: conversation, token: token) else {
            throw AgentFailure(code: "UNAUTHORIZED",
                               message: "This server was not started by KetPlus for this conversation.")
        }
        if FileManager.default.fileExists(atPath: paths.remoteTurnMarker(conversation: conversation).path) {
            throw AgentFailure(code: "REMOTE_TURN",
                               message: "Computer use is off for turns started from a remote device. Ask the user to continue at the Mac.")
        }
        return try tools.call(method, request["params"] as? [String: Any] ?? [:], conversation: conversation)
    }
}
