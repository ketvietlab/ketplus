#pragma once

#include <QString>

namespace ketplus::daemon::executor {
// macOS sockaddr_un has a short pathname bound. Job data paths can legitimately
// exceed it; only the control socket lives under this private, short root.
QString controlAddress(const QString& directory, const QString& lease);
bool ensureControlDirectory(QString* error);
} // namespace ketplus::daemon::executor
