#pragma once

#include <QString>

// Field rules shared by validation and decode, and by code that stores what frames name.
namespace ketplus::runner::protocol::rules {

// 1..maxChars printable ASCII characters without space (0x21-0x7E).
[[nodiscard]] bool isToken(const QString& text, qsizetype maxChars);
[[nodiscard]] bool isJobId(const QString& text);
[[nodiscard]] bool isAttempt(qint64 value);
// `workspaceKey` and `repositoryKey`: exactly 32 base64url characters.
[[nodiscard]] bool isKey(const QString& text);
// A reason or error code: `^[a-z][a-z0-9_]{0,63}$`.
[[nodiscard]] bool isReasonCode(const QString& text);
// `snapshotId` and other object ids: lowercase hex, 40 or 64 characters.
[[nodiscard]] bool isObjectId(const QString& text);
// Exactly `length` lowercase hex characters, such as a SHA-256 or a transferId.
[[nodiscard]] bool isLowerHex(const QString& text, qsizetype length);

} // namespace ketplus::runner::protocol::rules
