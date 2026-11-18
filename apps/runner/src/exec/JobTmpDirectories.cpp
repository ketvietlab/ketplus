#include "exec/JobTmpDirectories.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ketplus::runner::exec {
namespace {

constexpr QFile::Permissions ownerOnly = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
constexpr QFile::Permissions groupOrOther = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                            QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;

bool validTaskId(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}-[1-9][0-9]{0,17}$"));
    return pattern.match(name).hasMatch();
}

bool simpleName(const QString& name) {
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..") &&
           !name.contains(u'/') && !name.contains(QChar::Null);
}

// Empties the directory open at `fd` and closes it. Each directory is made owner-writable first,
// so a job that took away its own permissions cannot keep its files.
bool emptyDirectory(const int fd) {
    if (::fchmod(fd, S_IRWXU) != 0) {
        ::close(fd);
        return false;
    }
    DIR* dir = ::fdopendir(fd);
    if (!dir) {
        ::close(fd);
        return false;
    }
    bool ok = true;
    while (true) {
        errno = 0;
        const dirent* entry = ::readdir(dir);
        if (!entry) {
            ok = ok && errno == 0;
            break;
        }
        const char* name = entry->d_name;
        if ((name[0] == '.' && name[1] == '\0') ||
            (name[0] == '.' && name[1] == '.' && name[2] == '\0'))
            continue;
        struct stat info{};
        if (::fstatat(::dirfd(dir), name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
            ok = false;
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            const int child =
                ::openat(::dirfd(dir), name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0 || !emptyDirectory(child)) {
                ok = false;
                continue;
            }
            ok = ::unlinkat(::dirfd(dir), name, AT_REMOVEDIR) == 0 && ok;
        } else {
            ok = ::unlinkat(::dirfd(dir), name, 0) == 0 && ok;
        }
    }
    ::closedir(dir);
    return ok;
}

} // namespace

JobTmpDirectories::JobTmpDirectories(QString root) : m_root(QDir::cleanPath(std::move(root))) {}

QString JobTmpDirectories::canonicalRoot() const {
    if (!QDir::isAbsolutePath(m_root))
        return {};
    if (!QFileInfo::exists(m_root) &&
        (!QDir().mkpath(m_root) || !QFile::setPermissions(m_root, ownerOnly)))
        return {};
    const QFileInfo info(m_root);
    if (!info.isDir() || info.isSymLink() || (info.permissions() & groupOrOther))
        return {};
    return info.canonicalFilePath();
}

std::optional<QString> JobTmpDirectories::create(const QString& taskId) {
    const QString root = canonicalRoot();
    if (root.isEmpty() || !validTaskId(taskId))
        return std::nullopt;
    if (!remove(taskId)) // also true when there is nothing
        return std::nullopt;
    const QByteArray path = QFile::encodeName(root + u'/' + taskId);
    if (::mkdir(path.constData(), S_IRWXU) != 0 || ::chmod(path.constData(), S_IRWXU) != 0)
        return std::nullopt;
    return root + u'/' + taskId;
}

QStringList JobTmpDirectories::names() const {
    if (!QDir::isAbsolutePath(m_root) || QFileInfo(m_root).isSymLink())
        return {};
    return QDir(m_root).entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                  QDir::NoDotAndDotDot);
}

bool JobTmpDirectories::remove(const QString& name) const {
    if (!simpleName(name) || !QDir::isAbsolutePath(m_root))
        return false;
    const QByteArray root = QFile::encodeName(m_root);
    const int rootFd = ::open(root.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (rootFd < 0)
        return errno == ENOENT;
    const QByteArray leaf = QFile::encodeName(name);
    struct stat info{};
    bool ok = true;
    if (::fstatat(rootFd, leaf.constData(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        ok = errno == ENOENT;
    } else if (S_ISDIR(info.st_mode)) {
        const int fd =
            ::openat(rootFd, leaf.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ok = fd >= 0 && emptyDirectory(fd) &&
             ::unlinkat(rootFd, leaf.constData(), AT_REMOVEDIR) == 0;
    } else {
        ok = ::unlinkat(rootFd, leaf.constData(), 0) == 0;
    }
    ::close(rootFd);
    return ok;
}

} // namespace ketplus::runner::exec
