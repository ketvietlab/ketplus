#pragma once

#include <QString>

// Field rules shared by validation and decode (internal to the library).
namespace ketplus::runner::protocol::rules {

// 1..maxChars printable ASCII characters without space (0x21-0x7E).
[[nodiscard]] bool isToken(const QString& text, qsizetype maxChars);
[[nodiscard]] bool isJobId(const QString& text);
[[nodiscard]] bool isAttempt(qint64 value);

} // namespace ketplus::runner::protocol::rules
