#include "state/StateDirectory.h"

#include "Environment.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <utility>

namespace ketplus::runner::app {
namespace {

QByteArray native(const QString& path) { return QFile::encodeName(path); }

QString systemError(const QString& what, const int code) {
    return QStringLiteral("%1: %2").arg(what, QString::fromLocal8Bit(std::strerror(code)));
}

bool privateTo(const struct stat& status, const uid_t owner) {
    return status.st_uid == owner && (status.st_mode & 077) == 0;
}

bool syncDirectory(const QString& path) {
    const int fd = ::open(native(path).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    return synced;
}

// Creates `path` with mode 0700, or checks the one that is there.
bool privateDirectory(const QString& path, const uid_t owner, QString* error) {
    const QByteArray name = native(path);
    if (::mkdir(name.constData(), 0700) != 0 && errno != EEXIST) {
        *error = systemError(QStringLiteral("Cannot create %1").arg(path), errno);
        return false;
    }
    struct stat status{};
    if (::lstat(name.constData(), &status) != 0) {
        *error = systemError(QStringLiteral("Cannot read %1").arg(path), errno);
        return false;
    }
    if (!S_ISDIR(status.st_mode)) {
        *error = QStringLiteral("%1 is not a directory").arg(path);
        return false;
    }
    if (status.st_uid != owner) {
        *error = QStringLiteral("%1 belongs to another user").arg(path);
        return false;
    }
    if ((status.st_mode & 077) != 0) {
        *error = QStringLiteral("%1 is accessible to other users; run chmod 700 on it").arg(path);
        return false;
    }
    return true;
}

} // namespace

QString StateDirectory::resolve(const QString& option, const Environment& environment) {
    if (!option.isEmpty())
        return QDir::cleanPath(QDir::current().absoluteFilePath(option));
    const QString configured = environment.variable("KETPLUS_RUNNER_STATE_DIR");
    if (!configured.isEmpty())
        return QDir::cleanPath(QDir::current().absoluteFilePath(configured));
    const QString xdg = environment.variable("XDG_STATE_HOME");
    if (!xdg.isEmpty() && QDir::isAbsolutePath(xdg))
        return QDir::cleanPath(xdg + QStringLiteral("/ketplus-runner"));
    const QString home = environment.variable("HOME");
    if (home.isEmpty() || !QDir::isAbsolutePath(home))
        return {};
    return QDir::cleanPath(home + QStringLiteral("/.local/state/ketplus-runner"));
}

std::optional<StateDirectory> StateDirectory::open(const QString& path, QString* error,
                                                   const uid_t owner) {
    if (path.isEmpty()) {
        *error = QStringLiteral("No state directory: pass --state-dir or set HOME");
        return std::nullopt;
    }
    const QString parent = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parent)) {
        *error = QStringLiteral("Cannot create %1").arg(parent);
        return std::nullopt;
    }
    if (!privateDirectory(path, owner, error))
        return std::nullopt;
    const QString lockPath = path + QStringLiteral("/lock");
    const int fd =
        ::open(native(lockPath).constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        *error = systemError(QStringLiteral("Cannot open %1").arg(lockPath), errno);
        return std::nullopt;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int code = errno;
        ::close(fd);
        *error = code == EWOULDBLOCK
                     ? QStringLiteral("Another ketplus-runner is using %1").arg(path)
                     : systemError(QStringLiteral("Cannot lock %1").arg(lockPath), code);
        return std::nullopt;
    }
    return StateDirectory(path, fd, owner);
}

StateDirectory::StateDirectory(StateDirectory&& other) noexcept
    : m_path(std::move(other.m_path)), m_lockFd(std::exchange(other.m_lockFd, -1)),
      m_owner(other.m_owner) {}

StateDirectory& StateDirectory::operator=(StateDirectory&& other) noexcept {
    if (this != &other) {
        if (m_lockFd >= 0)
            ::close(m_lockFd);
        m_path = std::move(other.m_path);
        m_lockFd = std::exchange(other.m_lockFd, -1);
        m_owner = other.m_owner;
    }
    return *this;
}

StateDirectory::~StateDirectory() {
    if (m_lockFd >= 0)
        ::close(m_lockFd); // releases the lock
}

QString StateDirectory::absolute(const QString& name) const {
    return m_path + QLatin1Char('/') + name;
}

bool StateDirectory::ensureSubdirectory(const QString& name, QString* error) const {
    return privateDirectory(absolute(name), m_owner, error);
}

StateDirectory::ReadResult StateDirectory::read(const QString& name, const qint64 maxBytes) const {
    ReadResult result;
    const QString file = absolute(name);
    const int fd = ::open(native(file).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return result;
        result.status = ReadResult::Status::Invalid;
        result.error = systemError(QStringLiteral("Cannot read %1").arg(file), errno);
        return result;
    }
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || !privateTo(status, m_owner) ||
        status.st_size > maxBytes) {
        ::close(fd);
        result.status = ReadResult::Status::Invalid;
        result.error = QStringLiteral("%1 is not a private file of this user (expected mode "
                                      "0600) or is too large")
                           .arg(file);
        return result;
    }
    QFile input;
    if (!input.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        result.status = ReadResult::Status::Invalid;
        result.error = QStringLiteral("Cannot read %1").arg(file);
        return result;
    }
    result.data = input.read(maxBytes + 1);
    if (result.data.size() > maxBytes) {
        result.status = ReadResult::Status::Invalid;
        result.error = QStringLiteral("%1 is too large").arg(file);
        result.data.clear();
        return result;
    }
    result.status = ReadResult::Status::Ok;
    return result;
}

bool StateDirectory::exists(const QString& name) const {
    struct stat status{};
    return ::lstat(native(absolute(name)).constData(), &status) == 0;
}

bool StateDirectory::write(const QString& name, const QByteArray& data, QString* error) const {
    const QString target = absolute(name);
    const QString directory = QFileInfo(target).absolutePath();
    if (directory != m_path && !ensureSubdirectory(QFileInfo(name).path(), error))
        return false;
    const QString temporary =
        QStringLiteral("%1/.%2.%3.tmp")
            .arg(directory, QFileInfo(target).fileName())
            .arg(QRandomGenerator::system()->generate64(), 16, 16, QLatin1Char('0'));
    const QByteArray temporaryName = native(temporary);
    const int fd = ::open(temporaryName.constData(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        *error = systemError(QStringLiteral("Cannot write %1").arg(target), errno);
        return false;
    }
    qsizetype written = 0;
    int failure = 0;
    while (written < data.size()) {
        const ssize_t count =
            ::write(fd, data.constData() + written, static_cast<size_t>(data.size() - written));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            failure = count < 0 ? errno : EIO;
            break;
        }
        written += count;
    }
    if (failure == 0 && ::fsync(fd) != 0)
        failure = errno;
    if (::close(fd) != 0 && failure == 0)
        failure = errno;
    if (failure == 0 && ::rename(temporaryName.constData(), native(target).constData()) != 0)
        failure = errno;
    if (failure != 0) {
        ::unlink(temporaryName.constData());
        *error = systemError(QStringLiteral("Cannot write %1").arg(target), failure);
        return false;
    }
    if (!syncDirectory(directory)) {
        *error = systemError(QStringLiteral("Cannot sync %1").arg(directory), errno);
        return false;
    }
    return true;
}

bool StateDirectory::remove(const QString& name, QString* error) const {
    const QString target = absolute(name);
    if (::unlink(native(target).constData()) != 0) {
        if (errno == ENOENT)
            return true;
        *error = systemError(QStringLiteral("Cannot remove %1").arg(target), errno);
        return false;
    }
    if (!syncDirectory(QFileInfo(target).absolutePath())) {
        *error = systemError(QStringLiteral("Cannot sync %1").arg(m_path), errno);
        return false;
    }
    return true;
}

std::optional<QStringList> StateDirectory::list(const QString& subdirectory, QString* error) const {
    if (!ensureSubdirectory(subdirectory, error))
        return std::nullopt;
    const QString path = absolute(subdirectory);
    DIR* directory = ::opendir(native(path).constData());
    if (directory == nullptr) {
        *error = systemError(QStringLiteral("Cannot read %1").arg(path), errno);
        return std::nullopt;
    }
    QStringList names;
    while (const dirent* entry = ::readdir(directory)) {
        const QString name = QFile::decodeName(entry->d_name);
        if (name.startsWith(QLatin1Char('.')))
            continue; // ".", ".." and temporary files of an interrupted write
        names.append(name);
    }
    ::closedir(directory);
    names.sort();
    return names;
}

} // namespace ketplus::runner::app
