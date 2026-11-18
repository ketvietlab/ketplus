#include "run/WorkDirectory.h"

#include "Environment.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <utility>

namespace ketplus::runner::app {
namespace {

QString systemError(const QString& what, const int code) {
    return QStringLiteral("%1: %2").arg(what, QString::fromLocal8Bit(std::strerror(code)));
}

bool inside(const QString& path, const QString& root) {
    return path == root || path.startsWith(root + QLatin1Char('/'));
}

// The real path, or the cleaned absolute one when it does not exist yet.
QString canonical(const QString& path) {
    const QString real = QFileInfo(path).canonicalFilePath();
    return real.isEmpty() ? QDir::cleanPath(QDir::current().absoluteFilePath(path)) : real;
}

} // namespace

QString WorkDirectory::resolve(const QString& option, const QString& runnerId,
                               const Environment& environment) {
    if (!option.isEmpty())
        return QDir::cleanPath(QDir::current().absoluteFilePath(option));
    const QString tail = QStringLiteral("/ketplus-runner/%1/work").arg(runnerId);
    const QString xdg = environment.variable("XDG_CACHE_HOME");
    if (!xdg.isEmpty() && QDir::isAbsolutePath(xdg))
        return QDir::cleanPath(xdg + tail);
    const QString home = environment.variable("HOME");
    if (home.isEmpty() || !QDir::isAbsolutePath(home))
        return {};
    return QDir::cleanPath(home + QStringLiteral("/.cache") + tail);
}

std::optional<WorkDirectory> WorkDirectory::open(const QString& path, const QString& statePath,
                                                 QString* error, const uid_t owner) {
    if (path.isEmpty()) {
        *error = QStringLiteral("No work directory: pass --work-dir or set HOME");
        return std::nullopt;
    }
    const QString state = canonical(statePath);
    if (inside(canonical(path), state) || inside(state, canonical(path))) {
        *error = QStringLiteral("The work directory and the state directory must not contain "
                                "each other");
        return std::nullopt;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = QStringLiteral("Cannot create the parent of %1").arg(path);
        return std::nullopt;
    }
    const QByteArray name = QFile::encodeName(path);
    if (::mkdir(name.constData(), 0700) != 0 && errno != EEXIST) {
        *error = systemError(QStringLiteral("Cannot create %1").arg(path), errno);
        return std::nullopt;
    }
    struct stat status{};
    if (::lstat(name.constData(), &status) != 0) {
        *error = systemError(QStringLiteral("Cannot read %1").arg(path), errno);
        return std::nullopt;
    }
    if (!S_ISDIR(status.st_mode)) {
        *error = QStringLiteral("%1 is not a directory").arg(path);
        return std::nullopt;
    }
    if (status.st_uid != owner) {
        *error = QStringLiteral("%1 belongs to another user").arg(path);
        return std::nullopt;
    }
    if ((status.st_mode & 077) != 0) {
        *error = QStringLiteral("%1 is accessible to other users; run chmod 700 on it").arg(path);
        return std::nullopt;
    }
    const QByteArray lockPath = QFile::encodeName(path + QStringLiteral("/.lock"));
    const int fd = ::open(lockPath.constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        *error = systemError(QStringLiteral("Cannot open the lock of %1").arg(path), errno);
        return std::nullopt;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int code = errno;
        ::close(fd);
        *error = code == EWOULDBLOCK
                     ? QStringLiteral("Another ketplus-runner is using %1").arg(path)
                     : systemError(QStringLiteral("Cannot lock %1").arg(path), code);
        return std::nullopt;
    }
    return WorkDirectory(path, fd);
}

WorkDirectory::WorkDirectory(WorkDirectory&& other) noexcept
    : m_path(std::move(other.m_path)), m_lockFd(std::exchange(other.m_lockFd, -1)) {}

WorkDirectory& WorkDirectory::operator=(WorkDirectory&& other) noexcept {
    if (this != &other) {
        if (m_lockFd >= 0)
            ::close(m_lockFd);
        m_path = std::move(other.m_path);
        m_lockFd = std::exchange(other.m_lockFd, -1);
    }
    return *this;
}

WorkDirectory::~WorkDirectory() {
    if (m_lockFd >= 0)
        ::close(m_lockFd);
}

} // namespace ketplus::runner::app
