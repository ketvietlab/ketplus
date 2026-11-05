#pragma once

#include <QString>
#include <QUrl>

namespace ketplus::daemon {

// The relay can sit under a path: the tenant gateway serves it at /native/{tenant}. A request
// path is appended to that base, never resolved against it, which would drop the prefix.
// `path` is already percent-encoded and may carry a query.
[[nodiscard]] inline QUrl controlEndpoint(const QUrl& base, const QString& path) {
    const QByteArray prefix =
        base.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::StripTrailingSlash)
            .toEncoded();
    return QUrl::fromEncoded(prefix + path.toUtf8(), QUrl::StrictMode);
}

} // namespace ketplus::daemon
