import CryptoKit
import Foundation

/// Per-conversation credentials minted by the daemon (modules/computer) and checked
/// by the agent app. Both sides must produce identical values; the test vectors in
/// ConversationTokenTests and tests/ComputerToolLaunchTest.cpp pin the format.
public enum ConversationToken {
    /// hex(HMAC-SHA256(hostKey, "computer.v0:" + conversation))
    public static func mint(key: Data, conversation: String) -> String {
        let mac = HMAC<SHA256>.authenticationCode(for: Data(("computer.v0:" + conversation).utf8),
                                                  using: SymmetricKey(data: key))
        return Data(mac).map { String(format: "%02x", $0) }.joined()
    }

    /// The conversation of a local MCP client outside KetPlus (`--local-client <name>`).
    /// The prefix keeps its grants apart from daemon conversations; nil for a bad name.
    public static func localConversation(_ name: String) -> String? {
        guard name.range(of: "^[A-Za-z0-9._-]{1,64}$", options: .regularExpression) != nil else { return nil }
        return "local:" + name
    }

    public static func verify(key: Data, conversation: String, token: String) -> Bool {
        guard !key.isEmpty, !conversation.isEmpty else { return false }
        let expected = Array(mint(key: key, conversation: conversation).utf8)
        let presented = Array(token.utf8)
        guard expected.count == presented.count else { return false }
        return zip(expected, presented).reduce(0) { $0 | ($1.0 ^ $1.1) } == 0
    }

    /// First 32 hex digits of SHA-256(conversation): a file name that does not
    /// reveal the conversation ID.
    public static func fileStem(_ conversation: String) -> String {
        let digest = SHA256.hash(data: Data(conversation.utf8))
        return String(digest.map { String(format: "%02x", $0) }.joined().prefix(32))
    }
}
