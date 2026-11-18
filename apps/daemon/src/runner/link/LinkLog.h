#pragma once

#include <QLoggingCategory>

// Lines about the desktop's runner peer links. They carry codes, states and counts only: never
// SDP, candidates, ICE server credentials, tokens, signatures or frame content.
Q_DECLARE_LOGGING_CATEGORY(lcRunnerLink)

#include <QJsonValue>
#include <QString>

namespace ketplus::daemon::runner::link {

// A code that arrived from elsewhere as a log may carry it: 1-64 printable ASCII characters
// (0x21-0x7E), otherwise `unknown`.
[[nodiscard]] QString loggableCode(const QJsonValue& code);

} // namespace ketplus::daemon::runner::link
