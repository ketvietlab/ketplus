#include "DaemonCredentialFile.h"

#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ketplus::remote {

QByteArray readLocalDaemonCredential(const QString& path, QString* error) {
    if (error)
        error->clear();
    const auto refuse = [error](const QString& reason) {
        if (error)
            *error = reason;
        return QByteArray{};
    };
    if (path.isEmpty() || path.contains(QChar::Null))
        return refuse(QStringLiteral("The local daemon credential path is invalid."));

    QByteArray bytes;
#ifdef Q_OS_UNIX
    // Inspect the opened object, not a pathname that could change before open.
    // NONBLOCK also prevents a substituted FIFO from blocking authentication.
    const auto name = QFile::encodeName(path);
    int fd;
    do {
        fd = ::open(name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return refuse(QStringLiteral("Unable to open the local daemon credential safely."));
    const auto close = qScopeGuard([fd] { ::close(fd); });
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::getuid() ||
        (info.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        return refuse(QStringLiteral("The local daemon credential has unsafe permissions."));
    if (info.st_size < 0 || info.st_size > maximumDaemonCredentialFileBytes)
        return refuse(QStringLiteral("The local daemon credential is too large."));
    // A second bound covers growth after fstat, without allocating for the file size.
    bytes.resize(maximumDaemonCredentialFileBytes + 1);
    qsizetype used = 0;
    while (used < bytes.size()) {
        const auto count = ::read(fd, bytes.data() + used, size_t(bytes.size() - used));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return refuse(QStringLiteral("Unable to read the local daemon credential."));
        if (count == 0)
            break;
        used += count;
    }
    bytes.resize(used);
#else
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile())
        return refuse(QStringLiteral("The local daemon credential is not a regular file."));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return refuse(QStringLiteral("Unable to read the local daemon credential."));
    bytes = file.read(maximumDaemonCredentialFileBytes + 1);
    if (file.error() != QFileDevice::NoError)
        return refuse(QStringLiteral("Unable to read the local daemon credential."));
#endif
    if (bytes.size() > maximumDaemonCredentialFileBytes)
        return refuse(QStringLiteral("The local daemon credential is too large."));
    const auto token = bytes.trimmed();
    if (token.size() < 32)
        return refuse(QStringLiteral("The local daemon credential is invalid."));
    return token;
}

} // namespace ketplus::remote
