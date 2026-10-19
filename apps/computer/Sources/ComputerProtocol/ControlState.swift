import Foundation

/// Per-conversation application grants and the single control lease (ADR 0026).
///
/// One conversation controls the desktop at a time. Its lease ends after
/// `idleTimeout` without an action, or when the user presses Esc, which also
/// revokes that conversation's grants so it must ask again.
public final class ControlState: @unchecked Sendable {
    public enum Failure: Error, Equatable {
        case locked(by: String)
    }

    public let idleTimeout: TimeInterval
    private let now: () -> Date
    private let lock = NSLock()
    private var grants: [String: Set<String>] = [:]
    private var cancelled: Set<String> = []
    private var holder: String?
    private var lastActivity = Date.distantPast

    public init(idleTimeout: TimeInterval = 90, now: @escaping () -> Date = Date.init) {
        self.idleTimeout = idleTimeout
        self.now = now
    }

    public func grantedApps(_ conversation: String) -> Set<String> {
        lock.withLock { grants[conversation] ?? [] }
    }

    public func grant(_ apps: Set<String>, to conversation: String) {
        lock.withLock {
            grants[conversation, default: []].formUnion(apps)
            cancelled.remove(conversation)
        }
    }

    /// True after the user stopped this conversation with Esc, until it is granted apps again.
    public func isCancelled(_ conversation: String) -> Bool {
        lock.withLock { cancelled.contains(conversation) }
    }

    /// Takes or renews the lease. Fails while another conversation holds an active one.
    public func acquire(_ conversation: String) throws {
        try lock.withLock {
            let current = now()
            if let holder, holder != conversation, current.timeIntervalSince(lastActivity) < idleTimeout {
                throw Failure.locked(by: holder)
            }
            holder = conversation
            lastActivity = current
        }
    }

    /// The conversation holding an unexpired lease, if any.
    public var activeHolder: String? {
        lock.withLock {
            guard let holder, now().timeIntervalSince(lastActivity) < idleTimeout else { return nil }
            return holder
        }
    }

    /// Esc: ends the lease and revokes the holder's grants. Returns the conversation it stopped.
    @discardableResult
    public func cancelActive() -> String? {
        lock.withLock {
            guard let stopped = holder else { return nil }
            grants[stopped] = nil
            cancelled.insert(stopped)
            holder = nil
            lastActivity = .distantPast
            return stopped
        }
    }

    /// Ends an expired lease without revoking grants. Returns true when one ended.
    @discardableResult
    public func expireIdleLease() -> Bool {
        lock.withLock {
            guard holder != nil, now().timeIntervalSince(lastActivity) >= idleTimeout else { return false }
            holder = nil
            return true
        }
    }
}
