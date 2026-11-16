#include "runtime/executor/NativeJobControlAddress.h"

#include <QCryptographicHash>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace ketplus::daemon::executor {
namespace {
QString controlDirectory() {
#ifdef Q_OS_MACOS
    return QStringLiteral("/private/tmp/ketplus-jobs-%1").arg(geteuid());
#else
    return QStringLiteral("/tmp/ketplus-jobs-%1").arg(geteuid());
#endif
}
} // namespace

QString controlAddress(const QString& directory, const QString& lease) {
    const auto hash = QCryptographicHash::hash(directory.toUtf8() + '\0' + lease.toUtf8(),
                                               QCryptographicHash::Sha256)
                          .toHex()
                          .left(40);
    return controlDirectory() + '/' + QString::fromLatin1(hash) + ".sock";
}

bool ensureControlDirectory(QString* error) {
    const auto path = controlDirectory().toLocal8Bit();
    if (mkdir(path.constData(), 0700) != 0 && errno != EEXIST) {
        if (error)
            *error = "Unable to create the private executor control directory.";
        return false;
    }
    struct stat status{};
    if (lstat(path.constData(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 0077) != 0) {
        if (error)
            *error = "The executor control directory is not private or has an unexpected owner.";
        return false;
    }
    return true;
}
} // namespace ketplus::daemon::executor
