import Darwin
import Foundation

public enum SocketError: Error, CustomStringConvertible {
    case pathTooLong(String)
    case system(String, Int32)
    case closed
    case messageTooLarge

    public var description: String {
        switch self {
        case .pathTooLong(let path): return "Socket path is too long: \(path)"
        case .system(let call, let code): return "\(call) failed: \(String(cString: strerror(code)))"
        case .closed: return "Connection closed"
        case .messageTooLarge: return "Message exceeds the frame limit"
        }
    }
}

/// Newline-delimited JSON over a Unix domain stream socket.
public final class LineConnection {
    /// A downscaled PNG is well under this; it bounds memory per frame.
    public static let maximumFrameBytes = 32 * 1024 * 1024

    public let descriptor: Int32
    private var buffer = Data()

    public init(descriptor: Int32) {
        self.descriptor = descriptor
        var on: Int32 = 1
        setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
    }

    deinit { close(descriptor) }

    public static func connect(to path: String) throws -> LineConnection {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw SocketError.system("socket", errno) }
        var address = try unixAddress(path)
        let result = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard result == 0 else {
            let code = errno
            close(fd)
            throw SocketError.system("connect", code)
        }
        return LineConnection(descriptor: fd)
    }

    /// Effective user ID of the peer process.
    public func peerUserID() -> uid_t? {
        var uid: uid_t = 0
        var gid: gid_t = 0
        return getpeereid(descriptor, &uid, &gid) == 0 ? uid : nil
    }

    public func readObject() throws -> [String: Any] {
        while true {
            if let newline = buffer.firstIndex(of: 0x0A) {
                let line = buffer.subdata(in: buffer.startIndex..<newline)
                buffer.removeSubrange(buffer.startIndex...newline)
                guard let object = try JSONSerialization.jsonObject(with: line) as? [String: Any] else {
                    throw SocketError.closed
                }
                return object
            }
            guard buffer.count <= Self.maximumFrameBytes else { throw SocketError.messageTooLarge }
            var chunk = [UInt8](repeating: 0, count: 64 * 1024)
            let count = read(descriptor, &chunk, chunk.count)
            if count == 0 { throw SocketError.closed }
            if count < 0 {
                if errno == EINTR { continue }
                throw SocketError.system("read", errno)
            }
            buffer.append(chunk, count: count)
        }
    }

    public func writeObject(_ object: [String: Any]) throws {
        var data = try JSONSerialization.data(withJSONObject: object)
        data.append(0x0A)
        try data.withUnsafeBytes { raw in
            var offset = 0
            while offset < raw.count {
                let written = write(descriptor, raw.baseAddress! + offset, raw.count - offset)
                if written < 0 {
                    if errno == EINTR { continue }
                    throw SocketError.system("write", errno)
                }
                offset += written
            }
        }
    }
}

public final class LineListener {
    public let descriptor: Int32

    /// Binds a fresh socket at `path`, replacing a stale one, readable only by the owner.
    public init(path: String) throws {
        unlink(path)
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw SocketError.system("socket", errno) }
        var address = try unixAddress(path)
        let previous = umask(0o077)
        let bound = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        umask(previous)
        guard bound == 0, listen(fd, 8) == 0 else {
            let code = errno
            close(fd)
            throw SocketError.system("bind/listen", code)
        }
        descriptor = fd
    }

    deinit { close(descriptor) }

    public func accept() throws -> LineConnection {
        while true {
            let fd = Darwin.accept(descriptor, nil, nil)
            if fd >= 0 { return LineConnection(descriptor: fd) }
            if errno != EINTR { throw SocketError.system("accept", errno) }
        }
    }
}

private func unixAddress(_ path: String) throws -> sockaddr_un {
    var address = sockaddr_un()
    address.sun_family = sa_family_t(AF_UNIX)
    let bytes = Array(path.utf8)
    let capacity = MemoryLayout.size(ofValue: address.sun_path)
    guard bytes.count < capacity else { throw SocketError.pathTooLong(path) }
    withUnsafeMutableBytes(of: &address.sun_path) { raw in
        raw.copyBytes(from: bytes)
        raw[bytes.count] = 0
    }
    return address
}
