#pragma once

#include <QByteArray>
#include <QString>

namespace ketplus::remote {

// Bound raw bytes before trimming; generated credentials occupy 43 bytes.
inline constexpr qint64 maximumDaemonCredentialFileBytes = 4096;

// Refuse unsupported files without modifying them. Errors never include contents.
QByteArray readLocalDaemonCredential(const QString& path, QString* error = nullptr);

} // namespace ketplus::remote
