import ComputerProtocol
import CoreGraphics
import Darwin
import Foundation
import Security

private struct AgentFailure: Error {
    let code: String
    let message: String
}

/// Serves computer.v0 requests from the MCP front over the user-only socket.
final class AgentServer {
    private let paths: AgentPaths
    private let token: String
    private let lock = NSLock()
    private var lastGeometry: ScreenGeometry?

    init(paths: AgentPaths) throws {
        self.paths = paths
        token = try Self.randomToken()
    }

    func start() throws {
        let manager = FileManager.default
        try manager.createDirectory(at: paths.directory, withIntermediateDirectories: true,
                                    attributes: [.posixPermissions: 0o700])
        try manager.setAttributes([.posixPermissions: 0o700], ofItemAtPath: paths.directory.path)
        let listener = try LineListener(path: paths.socket.path)
        // Publish the token only once the socket accepts connections.
        manager.createFile(atPath: paths.token.path, contents: Data(token.utf8),
                           attributes: [.posixPermissions: 0o600])

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
            let id = request["id"] ?? NSNull()
            var response: [String: Any] = ["id": id]
            do {
                guard let presented = request["token"] as? String, Self.constantTimeEqual(presented, token) else {
                    throw AgentFailure(code: "UNAUTHORIZED", message: "The agent token is missing or invalid.")
                }
                let method = request["method"] as? String ?? ""
                let params = request["params"] as? [String: Any] ?? [:]
                response["result"] = try dispatch(method, params)
            } catch let failure as AgentFailure {
                response["error"] = ["code": failure.code, "message": failure.message]
            } catch {
                response["error"] = ["code": "INTERNAL", "message": String(describing: error)]
            }
            guard (try? connection.writeObject(response)) != nil else { return }
        }
    }

    private func dispatch(_ method: String, _ params: [String: Any]) throws -> [String: Any] {
        switch method {
        case "status": return status()
        case "screenshot": return try screenshot()
        case "left_click": return try leftClick(params)
        default: throw AgentFailure(code: "UNKNOWN_METHOD", message: "Unknown method \(method).")
        }
    }

    private func status() -> [String: Any] {
        [
            "protocol": AgentPaths.protocolVersion,
            "bundleIdentifier": Bundle.main.bundleIdentifier ?? "",
            "version": Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "",
            "pid": Int(getpid()),
            "parentPid": Int(getppid()),
            "screenRecording": Permissions.screenRecording,
            "accessibility": Permissions.accessibility,
        ]
    }

    private func screenshot() throws -> [String: Any] {
        guard Permissions.screenRecording || onMain({ Permissions.requestScreenRecording() }) else {
            throw AgentFailure(code: "PERMISSION_REQUIRED",
                               message: "Allow KetPlus Computer in System Settings > Privacy & Security > Screen & System Audio Recording, then retry.")
        }
        let started = Date()
        let shot: Screenshot
        do {
            shot = try waitFor { try await ScreenCapture.captureMainDisplay() }
        } catch {
            throw AgentFailure(code: "CAPTURE_FAILED", message: String(describing: error))
        }
        lock.withLock { lastGeometry = shot.geometry }
        return [
            "png": shot.png.base64EncodedString(),
            "width": shot.geometry.imageWidth,
            "height": shot.geometry.imageHeight,
            "captureMs": Int(Date().timeIntervalSince(started) * 1000),
        ]
    }

    private func leftClick(_ params: [String: Any]) throws -> [String: Any] {
        guard let coordinate = params["coordinate"] as? [Any], coordinate.count == 2,
              let x = (coordinate[0] as? NSNumber)?.intValue, let y = (coordinate[1] as? NSNumber)?.intValue
        else {
            throw AgentFailure(code: "INVALID_PARAMS", message: "coordinate must be [x, y] integers.")
        }
        // Coordinates refer to the last image the model received. Invalid input
        // is refused before any permission prompt.
        let geometry = lock.withLock { lastGeometry } ?? ScreenGeometry(displayBounds: CGDisplayBounds(CGMainDisplayID()))
        guard geometry.contains(imageX: x, imageY: y) else {
            throw AgentFailure(code: "OUT_OF_BOUNDS",
                               message: "(\(x), \(y)) is outside the \(geometry.imageWidth)x\(geometry.imageHeight) screenshot.")
        }
        guard Permissions.accessibility || onMain({ Permissions.requestAccessibility() }) else {
            throw AgentFailure(code: "PERMISSION_REQUIRED",
                               message: "Allow KetPlus Computer in System Settings > Privacy & Security > Accessibility, then retry.")
        }
        let point = geometry.globalPoint(imageX: x, imageY: y)
        try InputControl.leftClick(at: point)
        return ["clicked": [x, y], "globalPoint": [point.x, point.y]]
    }

    private func onMain<T>(_ body: () -> T) -> T {
        DispatchQueue.main.sync(execute: body)
    }

    private func waitFor<T>(_ operation: @escaping () async throws -> T) throws -> T {
        let semaphore = DispatchSemaphore(value: 0)
        var outcome: Result<T, Error>!
        Task.detached {
            do { outcome = .success(try await operation()) } catch { outcome = .failure(error) }
            semaphore.signal()
        }
        semaphore.wait()
        return try outcome.get()
    }

    private static func randomToken() throws -> String {
        var bytes = [UInt8](repeating: 0, count: 32)
        guard SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes) == errSecSuccess else {
            throw AgentFailure(code: "INTERNAL", message: "No secure random source.")
        }
        return bytes.map { String(format: "%02x", $0) }.joined()
    }

    private static func constantTimeEqual(_ a: String, _ b: String) -> Bool {
        let x = Array(a.utf8), y = Array(b.utf8)
        guard x.count == y.count else { return false }
        return zip(x, y).reduce(0) { $0 | ($1.0 ^ $1.1) } == 0
    }
}
