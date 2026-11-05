#include "workspace/ReadOnlyWorkspaceFiles.h"
#include "workspace/WorkspaceExposure.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QDateTime>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QThreadPool>
#include <QTimeZone>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ketplus::daemon {
namespace {

// APFS and NTFS are case-insensitive by default, so ".GIT/config" names the same file.
bool isGitSegment(const QString& segment) {
    return segment.compare(QStringLiteral(".git"), Qt::CaseInsensitive) == 0;
}

enum class OpenResult { Opened, Missing, Symlink, NotRegular, Failed };

// Opens a regular file beneath root without following a symlink at any component. Checking
// paths first and opening them later let a component be swapped for a link that escapes the
// worktree in between; descriptor-relative opens close that window. Non-blocking open keeps a
// FIFO from stalling the daemon before fstat rejects it.
OpenResult openBeneath(const QString& root, const QString& clean, QFile* file, qint64* size,
                       QByteArray* stamp = nullptr) {
    const QStringList segments = clean.split(u'/', Qt::SkipEmptyParts);
    if (root.isEmpty() || segments.isEmpty())
        return OpenResult::Missing;
#if defined(Q_OS_UNIX)
    int directory = ::open(QFile::encodeName(root).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
        return OpenResult::Failed;
    for (qsizetype index = 0; index < segments.size(); ++index) {
        const bool last = index + 1 == segments.size();
        const QByteArray name = QFile::encodeName(segments.at(index));
        const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (last ? O_NONBLOCK : O_DIRECTORY);
        const int next = ::openat(directory, name.constData(), flags);
        if (next < 0) {
            const int openError = errno;
            struct stat link {};
            const bool symlink = ::fstatat(directory, name.constData(), &link,
                                           AT_SYMLINK_NOFOLLOW) == 0 &&
                                 S_ISLNK(link.st_mode);
            ::close(directory);
            if (symlink)
                return OpenResult::Symlink;
            if (openError == ENOENT || openError == ENOTDIR)
                return OpenResult::Missing;
            return OpenResult::Failed;
        }
        ::close(directory);
        directory = next;
    }
    struct stat status {};
    if (::fstat(directory, &status) != 0) {
        ::close(directory);
        return OpenResult::Failed;
    }
    if (!S_ISREG(status.st_mode)) {
        ::close(directory);
        return OpenResult::NotRegular;
    }
    if (!file->open(directory, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(directory);
        return OpenResult::Failed;
    }
    *size = status.st_size;
    if (stamp) {
#if defined(Q_OS_DARWIN)
        const auto modified = status.st_mtimespec;
        const auto changed = status.st_ctimespec;
#else
        const auto modified = status.st_mtim;
        const auto changed = status.st_ctim;
#endif
        *stamp = QStringLiteral("size=%1;mtime=%2.%3;ctime=%4.%5;ino=%6;dev=%7")
                     .arg(qint64(status.st_size))
                     .arg(qint64(modified.tv_sec))
                     .arg(qint64(modified.tv_nsec))
                     .arg(qint64(changed.tv_sec))
                     .arg(qint64(changed.tv_nsec))
                     .arg(quint64(status.st_ino))
                     .arg(quint64(status.st_dev))
                     .toLatin1();
    }
    return OpenResult::Opened;
#else
    QString current = root;
    for (const QString& segment : segments) {
        current = QDir(current).filePath(segment);
        if (QFileInfo(current).isSymLink())
            return OpenResult::Symlink;
    }
    const QFileInfo info(current);
    if (!info.exists())
        return OpenResult::Missing;
    if (!info.isFile())
        return OpenResult::NotRegular;
    file->setFileName(current);
    if (!file->open(QIODevice::ReadOnly))
        return OpenResult::Failed;
    *size = info.size();
    if (stamp) {
        *stamp = QStringLiteral("size=%1;mtime=%2;ctime=%3")
                     .arg(info.size())
                     .arg(info.lastModified(QTimeZone::UTC).toMSecsSinceEpoch())
                     .arg(info.metadataChangeTime(QTimeZone::UTC).toMSecsSinceEpoch())
                     .toLatin1();
    }
    return OpenResult::Opened;
#endif
}

QJsonObject failure(const QString& code) {
    return {{QStringLiteral("ok"), false}, {QStringLiteral("error_code"), code}};
}

bool validOpaqueId(const QJsonValue& value) {
    const QString id = value.toString();
    return !id.isEmpty() && id.size() <= 128 && !id.contains(u'\n');
}

bool textContent(const QByteArray& data) {
    if (data.contains('\0'))
        return false;
    QStringDecoder decoder(QStringDecoder::Utf8);
    (void)decoder(data);
    return !decoder.hasError();
}

QString relativeClean(const QString& value) {
    if (value.isEmpty() || value.size() > 1024 || QDir::isAbsolutePath(value) ||
        value.contains(u'\0') || value.contains(u'\\'))
        return {};
    const QString clean = QDir::cleanPath(value);
    if (clean == QStringLiteral(".") || clean == QStringLiteral("..") ||
        clean.startsWith(QStringLiteral("../")) || clean.contains(QStringLiteral("/../")))
        return {};
    const QStringList segments = clean.split(u'/', Qt::SkipEmptyParts);
    if (std::any_of(segments.cbegin(), segments.cend(), isGitSegment))
        return {};
    return clean;
}

} // namespace

ReadOnlyWorkspaceFiles::ReadOnlyWorkspaceFiles(WorkspaceFileScope scope, RevisionFence fence)
    : scope_(std::move(scope)), fence_(fence),
      canonicalRoot_(QFileInfo(scope_.rootPath).canonicalFilePath()) {}

QStringList ReadOnlyWorkspaceFiles::hardenedGitArguments(const QString& worktree) {
    // Command-line -c overrides repository config. fsmonitor is the read-path hook that a hostile
    // .git/config can use to execute a program during status/diff; the others keep read commands
    // from spawning pagers, external diff tools, or credential/ssh helpers.
    return {QStringLiteral("--no-optional-locks"),
            QStringLiteral("-c"),
            QStringLiteral("core.fsmonitor=false"),
            QStringLiteral("-c"),
            QStringLiteral("core.untrackedCache=false"),
            QStringLiteral("-c"),
            QStringLiteral("core.pager=cat"),
            QStringLiteral("-c"),
            QStringLiteral("diff.external="),
            QStringLiteral("-c"),
            QStringLiteral("core.sshCommand="),
            QStringLiteral("-c"),
            QStringLiteral("credential.helper="),
            QStringLiteral("-C"),
            worktree};
}

QProcessEnvironment ReadOnlyWorkspaceFiles::hardenedGitEnvironment() {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    environment.remove(QStringLiteral("GIT_EXTERNAL_DIFF"));
    environment.remove(QStringLiteral("GIT_DIR"));
    environment.remove(QStringLiteral("GIT_WORK_TREE"));
    environment.remove(QStringLiteral("GIT_INDEX_FILE"));
    environment.remove(QStringLiteral("GIT_CONFIG_PARAMETERS"));
    environment.remove(QStringLiteral("GIT_CONFIG_COUNT"));
    return environment;
}

void ReadOnlyWorkspaceFiles::revisionAsync(WorkspaceFileScope scope, QObject* context,
                                           RevisionCallback callback) {
    const QPointer<QObject> guard(context);
    QThreadPool::globalInstance()->start(
        [scope = std::move(scope), guard, callback = std::move(callback)]() mutable {
            QString error;
            const QString value = ReadOnlyWorkspaceFiles(std::move(scope)).revision(&error);
            if (!guard)
                return;
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, callback = std::move(callback), value, error] {
                    if (guard)
                        callback(value, error);
                },
                Qt::QueuedConnection);
        });
}

QByteArray ReadOnlyWorkspaceFiles::git(const QStringList& arguments, const int maximumBytes,
                                       QString* errorCode) const {
    const std::optional<QProcessEnvironment> environment = readEnvironment();
    if (!environment) {
        if (errorCode)
            *errorCode = QStringLiteral("unavailable");
        return {};
    }
    QProcess process;
    process.setProgram(QStringLiteral("git"));
    process.setArguments(hardenedGitArguments(canonicalRoot_) + arguments);
    process.setProcessEnvironment(*environment);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QIODevice::ReadOnly);
    if (!process.waitForStarted(2'000) || !process.waitForFinished(10'000)) {
        process.kill();
        process.waitForFinished();
        if (errorCode)
            *errorCode = QStringLiteral("unavailable");
        return {};
    }
    QByteArray output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errorCode)
            *errorCode = QStringLiteral("unavailable");
        return {};
    }
    if (output.size() > maximumBytes) {
        if (errorCode)
            *errorCode = QStringLiteral("oversized");
        return {};
    }
    return output;
}

std::optional<QProcessEnvironment> ReadOnlyWorkspaceFiles::readEnvironment() const {
    if (!filterDrivers_) {
        // Reading config runs nothing; the overrides then win over every config file.
        QProcess process;
        process.setProgram(QStringLiteral("git"));
        process.setArguments(hardenedGitArguments(canonicalRoot_) +
                             QStringList{QStringLiteral("config"), QStringLiteral("-z"),
                                         QStringLiteral("--name-only"),
                                         QStringLiteral("--get-regexp"),
                                         QStringLiteral("^filter\\.")});
        process.setProcessEnvironment(hardenedGitEnvironment());
        process.start(QIODevice::ReadOnly);
        if (!process.waitForStarted(2'000) || !process.waitForFinished(5'000)) {
            process.kill();
            process.waitForFinished();
            return std::nullopt;
        }
        // Exit 1 means no filter is configured; anything else leaves the drivers unknown.
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() > 1)
            return std::nullopt;
        QStringList drivers;
        for (const QByteArray& name : process.readAllStandardOutput().split('\0')) {
            const QString key = QString::fromUtf8(name);
            const qsizetype last = key.lastIndexOf(u'.');
            const QString driver = last > 7 ? key.mid(7, last - 7) : QString{};
            if (!driver.isEmpty() && !drivers.contains(driver))
                drivers.append(driver);
        }
        filterDrivers_ = drivers;
    }
    // GIT_CONFIG_KEY_n keeps key and value apart, so a driver name containing '=' (valid in
    // Git) cannot split an override the way `-c filter.a=b.clean=` would.
    QProcessEnvironment environment = hardenedGitEnvironment();
    int count = 0;
    const auto override = [&environment, &count](const QString& key, const QString& value) {
        environment.insert(QStringLiteral("GIT_CONFIG_KEY_%1").arg(count), key);
        environment.insert(QStringLiteral("GIT_CONFIG_VALUE_%1").arg(count), value);
        ++count;
    };
    for (const QString& driver : std::as_const(*filterDrivers_)) {
        for (const char* key : {"clean", "smudge", "process"})
            override(QStringLiteral("filter.%1.%2").arg(driver, QLatin1String(key)), {});
        override(QStringLiteral("filter.%1.required").arg(driver), QStringLiteral("false"));
    }
    environment.insert(QStringLiteral("GIT_CONFIG_COUNT"), QString::number(count));
    return environment;
}

bool ReadOnlyWorkspaceFiles::insideRepository() const {
    for (QDir directory(canonicalRoot_);;) {
        if (QFileInfo::exists(directory.filePath(QStringLiteral(".git"))))
            return true;
        if (!directory.cdUp())
            return false;
    }
}

std::optional<QStringList>
ReadOnlyWorkspaceFiles::ignoredPaths(const QStringList& relativePaths) const {
    // Outside a repository nothing is ignored. Inside one, a Git failure (corrupt index, bad
    // config, timeout) leaves the policy unknown, and callers must not share anything.
    if (relativePaths.isEmpty() || !insideRepository())
        return QStringList{};
    const std::optional<QProcessEnvironment> environment = readEnvironment();
    if (!environment)
        return std::nullopt;
    QProcess process;
    process.setProgram(QStringLiteral("git"));
    process.setArguments(hardenedGitArguments(canonicalRoot_) +
                         QStringList{QStringLiteral("check-ignore"), QStringLiteral("-z"),
                                     QStringLiteral("--stdin")});
    process.setProcessEnvironment(*environment);
    process.start();
    if (!process.waitForStarted(2'000))
        return std::nullopt;
    for (const QString& path : relativePaths) {
        process.write(path.toUtf8());
        process.write("\0", 1);
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(10'000)) {
        process.kill();
        process.waitForFinished();
        return std::nullopt;
    }
    // Exit 1 means nothing is ignored.
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() > 1)
        return std::nullopt;
    QStringList ignored;
    for (const QByteArray& path : process.readAllStandardOutput().split('\0')) {
        if (!path.isEmpty())
            ignored.append(QString::fromUtf8(path));
    }
    return ignored;
}

QString ReadOnlyWorkspaceFiles::exposureError(const QString& relativePath) const {
    if (workspace_exposure::isSecretPath(relativePath))
        return QStringLiteral("permission_denied");
    const std::optional<QStringList> ignored = ignoredPaths({relativePath});
    if (!ignored)
        return QStringLiteral("unavailable");
    return ignored->isEmpty() ? QString{} : QStringLiteral("permission_denied");
}

QString ReadOnlyWorkspaceFiles::revision(QString* errorCode) const {
    if (canonicalRoot_.isEmpty()) {
        if (errorCode)
            *errorCode = QStringLiteral("missing");
        return {};
    }
    QString error;
    const QByteArray head = git(
        {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("HEAD^{commit}")},
        128, &error);
    const QByteArray status = git({QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
                                   QStringLiteral("-z"), QStringLiteral("--untracked-files=all")},
                                  4 * 1024 * 1024, &error);
    if (!error.isEmpty()) {
        if (errorCode)
            *errorCode = error;
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("ketplus-worktree-revision-v2\0", 29));
    hash.addData(head.trimmed());
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(status);
    // HEAD fences clean files. `status --untracked-files=all` already names every staged,
    // unstaged and untracked path (plus rename/copy sources), so no further git runs are needed.
    QSet<QString> dirtyPaths;
    const QList<QByteArray> records = status.split('\0');
    for (qsizetype index = 0; index < records.size(); ++index) {
        const QByteArray& record = records.at(index);
        if (record.size() < 4)
            continue;
        dirtyPaths.insert(QString::fromUtf8(record.sliced(3)));
        const QByteArray state = record.first(2);
        if ((state.contains('R') || state.contains('C')) && index + 1 < records.size()) {
            ++index;
            if (!records.at(index).isEmpty())
                dirtyPaths.insert(QString::fromUtf8(records.at(index)));
        }
    }
    QStringList ordered = dirtyPaths.values();
    ordered.sort(Qt::CaseSensitive);
    // Content is hashed only for small files and within a total budget; the rest is fenced by
    // size/mtime/ctime/inode. Sorted order and per-file sizes make the budget split deterministic
    // for an unchanged worktree, so repeated computations agree.
    qint64 hashedBytes = 0;
    for (const QString& relative : ordered) {
        hash.addData(relative.toUtf8());
        hash.addData(QByteArrayView("\0", 1));
        QFile file;
        qint64 size = 0;
        QByteArray stamp;
        if (openBeneath(canonicalRoot_, relativeClean(relative), &file, &size, &stamp) !=
            OpenResult::Opened) {
            hash.addData("missing-or-unsupported");
            continue;
        }
        if (size > maximumRevisionHashFileBytes ||
            hashedBytes + size > maximumRevisionHashTotalBytes) {
            hash.addData("stat:");
            hash.addData(stamp);
            hash.addData(QByteArrayView("\0", 1));
            continue;
        }
        hash.addData("content:");
        qint64 remaining = size;
        while (remaining > 0 && !file.atEnd()) {
            const QByteArray chunk = file.read(qMin<qint64>(64 * 1024, remaining));
            if (chunk.isEmpty())
                break;
            remaining -= chunk.size();
            hash.addData(chunk);
        }
        hashedBytes += size;
        hash.addData(QByteArrayView("\0", 1));
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool ReadOnlyWorkspaceFiles::scopeMatches(const QJsonObject& request) const {
    return validOpaqueId(request.value(QStringLiteral("owner_subject_id"))) &&
           request.value(QStringLiteral("owner_subject_id")).toString() == scope_.ownerSubjectId &&
           request.value(QStringLiteral("workspace_id")).toString() == scope_.workspaceId &&
           request.value(QStringLiteral("machine_id")).toString() == scope_.machineId &&
           request.value(QStringLiteral("repository_id")).toString() == scope_.repositoryId &&
           request.value(QStringLiteral("worktree_id")).toString() == scope_.worktreeId;
}

QString ReadOnlyWorkspaceFiles::resolved(const QString& relativePath, QString* errorCode) const {
    const QString clean = relativeClean(relativePath);
    if (clean.isEmpty() || canonicalRoot_.isEmpty()) {
        *errorCode = QStringLiteral("invalid_path");
        return {};
    }
    QString current = canonicalRoot_;
    for (const QString& segment : clean.split(u'/', Qt::SkipEmptyParts)) {
        const QFileInfo candidate(QDir(current).filePath(segment));
        if (candidate.isSymLink()) {
            *errorCode = QStringLiteral("symlink_unsupported");
            return {};
        }
        current = candidate.absoluteFilePath();
    }
    const QString normalized = QFileInfo(current).absoluteFilePath();
    if (normalized != canonicalRoot_ && !normalized.startsWith(canonicalRoot_ + u'/')) {
        *errorCode = QStringLiteral("invalid_path");
        return {};
    }
    return normalized;
}

QJsonObject ReadOnlyWorkspaceFiles::fencedError(const QString& code) const {
    return failure(code.isEmpty() ? QStringLiteral("unavailable") : code);
}

QString ReadOnlyWorkspaceFiles::startRevision(const QString& expected) const {
    if (expected.isEmpty())
        return QStringLiteral("stale_revision");
    if (fence_ == RevisionFence::Published)
        return {};
    QString error;
    if (revision(&error) != expected)
        return error.isEmpty() ? QStringLiteral("stale_revision") : error;
    return {};
}

bool ReadOnlyWorkspaceFiles::finishRevision(const QString& expected, QJsonObject* result) const {
    if (fence_ == RevisionFence::Live) {
        QString error;
        const QString after = revision(&error);
        if (!error.isEmpty() || after != expected) {
            *result = failure(QStringLiteral("stale_revision"));
            return false;
        }
    }
    result->insert(QStringLiteral("ok"), true);
    result->insert(QStringLiteral("revision"), expected);
    return true;
}

QJsonObject ReadOnlyWorkspaceFiles::tree(const QJsonObject& request) const {
    if (!scopeMatches(request))
        return failure(QStringLiteral("scope_forbidden"));
    const QString expected = request.value(QStringLiteral("revision")).toString();
    if (const QString error = startRevision(expected); !error.isEmpty())
        return failure(error);
    const int limit =
        qBound(1, request.value(QStringLiteral("limit")).toInt(100), maximumTreeEntries);
    const int offset = qBound(0, request.value(QStringLiteral("offset")).toInt(), 1'000'000);
    const bool modifiedOnly = request.value(QStringLiteral("modified_only")).toBool();
    const QString parent = request.value(QStringLiteral("path")).toString(QStringLiteral("."));
    QString error;
    const QString directory =
        parent == QStringLiteral(".") ? canonicalRoot_ : resolved(parent, &error);
    if (directory.isEmpty())
        return fencedError(error);
    const QFileInfo directoryInfo(directory);
    if (!directoryInfo.exists())
        return failure(QStringLiteral("missing"));
    if (!directoryInfo.isDir() || !directoryInfo.isReadable())
        return failure(QStringLiteral("permission_denied"));
    QFileInfoList entries;
    QHash<QString, QString> changeKinds;
    if (modifiedOnly) {
        if (parent != QStringLiteral("."))
            return failure(QStringLiteral("invalid_path"));
        QString gitError;
        const QByteArray status =
            git({QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("-z"),
                 QStringLiteral("--untracked-files=all")},
                2 * 1024 * 1024, &gitError);
        if (!gitError.isEmpty())
            return failure(gitError);
        const QList<QByteArray> records = status.split('\0');
        for (qsizetype index = 0; index < records.size(); ++index) {
            const QByteArray& record = records.at(index);
            if (record.size() < 4)
                continue;
            const QByteArray state = record.first(2);
            const QString relative = QString::fromUtf8(record.sliced(3));
            QString pathError;
            const QString absolute = resolved(relative, &pathError);
            if (relative.isEmpty() || absolute.isEmpty())
                continue;
            QString kind = QStringLiteral("modified");
            if (state == "??" || state.contains('A'))
                kind = QStringLiteral("added");
            else if (state.contains('D'))
                kind = QStringLiteral("deleted");
            else if (state.contains('R') || state.contains('C'))
                kind = QStringLiteral("renamed");
            changeKinds.insert(absolute, kind);
            // Porcelain -z emits the original path as the next NUL record for rename/copy.
            if ((state.contains('R') || state.contains('C')) && index + 1 < records.size())
                ++index;
        }
        for (const QString& path : changeKinds.keys())
            entries.append(QFileInfo(path));
        std::sort(entries.begin(), entries.end(),
                  [](const QFileInfo& left, const QFileInfo& right) {
                      return left.absoluteFilePath() < right.absoluteFilePath();
                  });
    } else {
        entries = QDir(directory).entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
    }
    entries.erase(
        std::remove_if(
            entries.begin(), entries.end(),
            [this](const QFileInfo& info) {
                return isGitSegment(info.fileName()) ||
                       workspace_exposure::isSecretPath(
                           QDir(canonicalRoot_).relativeFilePath(info.absoluteFilePath()));
            }),
        entries.end());
    // Ignored entries (build output, local config, secrets) are not shared with devices.
    QStringList candidates;
    candidates.reserve(entries.size());
    for (const QFileInfo& info : std::as_const(entries))
        candidates.append(QDir(canonicalRoot_).relativeFilePath(info.absoluteFilePath()));
    const std::optional<QStringList> ignoredList = ignoredPaths(candidates);
    if (!ignoredList)
        return failure(QStringLiteral("unavailable"));
    const QSet<QString> ignored(ignoredList->cbegin(), ignoredList->cend());
    if (!ignored.isEmpty())
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                           [this, &ignored](const QFileInfo& info) {
                               return ignored.contains(
                                   QDir(canonicalRoot_).relativeFilePath(info.absoluteFilePath()));
                           }),
            entries.end());
    QJsonArray values;
    for (int index = offset; index < entries.size() && values.size() < limit; ++index) {
        const QFileInfo& info = entries.at(index);
        const QString path = QDir(canonicalRoot_).relativeFilePath(info.absoluteFilePath());
        QJsonObject value{{QStringLiteral("path"), path},
                          {QStringLiteral("name"), info.fileName()},
                          {QStringLiteral("kind"), info.isSymLink() ? QStringLiteral("unsupported")
                                                   : info.isDir()   ? QStringLiteral("directory")
                                                                    : QStringLiteral("file")},
                          {QStringLiteral("size"), info.isFile() ? info.size() : 0},
                          {QStringLiteral("readable"), info.isReadable() && !info.isSymLink()}};
        if (modifiedOnly)
            value.insert(QStringLiteral("change_kind"), changeKinds.value(info.absoluteFilePath()));
        values.append(value);
    }
    QJsonObject result{{QStringLiteral("entries"), values},
                       {QStringLiteral("next_offset"),
                        offset + values.size() < entries.size() ? offset + values.size() : -1}};
    (void)finishRevision(expected, &result);
    return result;
}

QJsonObject ReadOnlyWorkspaceFiles::file(const QJsonObject& request) const {
    if (!scopeMatches(request))
        return failure(QStringLiteral("scope_forbidden"));
    const QString expected = request.value(QStringLiteral("revision")).toString();
    if (const QString error = startRevision(expected); !error.isEmpty())
        return failure(error);
    const QString relative = relativeClean(request.value(QStringLiteral("path")).toString());
    QString error;
    if (relative.isEmpty() || resolved(relative, &error).isEmpty())
        return failure(error.isEmpty() ? QStringLiteral("invalid_path") : error);
    // Git cannot evaluate ignore rules beyond a symlink, so that is reported first.
    if (const QString denied = exposureError(relative); !denied.isEmpty())
        return failure(denied);
    QFile source;
    qint64 size = 0;
    switch (openBeneath(canonicalRoot_, relative, &source, &size)) {
    case OpenResult::Opened:
        break;
    case OpenResult::Missing:
        return failure(QStringLiteral("missing"));
    case OpenResult::Symlink:
        return failure(QStringLiteral("symlink_unsupported"));
    case OpenResult::NotRegular:
    case OpenResult::Failed:
        return failure(QStringLiteral("permission_denied"));
    }
    if (size > maximumFileBytes)
        return failure(QStringLiteral("oversized"));
    const QByteArray data = source.read(maximumFileBytes + 1);
    if (data.size() > maximumFileBytes)
        return failure(QStringLiteral("oversized"));
    if (!textContent(data))
        return failure(QStringLiteral("binary"));
    QJsonObject result{
        {QStringLiteral("path"), request.value(QStringLiteral("path"))},
        {QStringLiteral("content"), QString::fromUtf8(data)},
        {QStringLiteral("byte_count"), data.size()},
        {QStringLiteral("digest_sha256"),
         QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex())}};
    (void)finishRevision(expected, &result);
    return result;
}

QJsonObject ReadOnlyWorkspaceFiles::diff(const QJsonObject& request) const {
    if (!scopeMatches(request))
        return failure(QStringLiteral("scope_forbidden"));
    const QString expected = request.value(QStringLiteral("revision")).toString();
    if (const QString error = startRevision(expected); !error.isEmpty())
        return failure(error);
    QString error;
    const QString relative = relativeClean(request.value(QStringLiteral("path")).toString());
    const QString absolute = relative.isEmpty() ? QString{} : resolved(relative, &error);
    if (absolute.isEmpty())
        return fencedError(error.isEmpty() ? QStringLiteral("invalid_path") : error);
    if (const QString denied = exposureError(relative); !denied.isEmpty())
        return failure(denied);
    // Exposure is decided for this one path, so the diff may select nothing else: no wildcard
    // expansion (literal pathspec) and no directory, on disk, in the index, or only in HEAD.
    if (QFileInfo(absolute).isDir())
        return failure(QStringLiteral("invalid_path"));
    const QString pathspec = QStringLiteral(":(literal)") + relative;
    const QByteArray tracked =
        git({QStringLiteral("ls-files"), QStringLiteral("-z"), QStringLiteral("--"), pathspec},
            64 * 1024, &error);
    const QByteArray changed = git({QStringLiteral("diff-index"), QStringLiteral("--name-only"),
                                    QStringLiteral("-z"), QStringLiteral("--no-renames"),
                                    QStringLiteral("HEAD"), QStringLiteral("--"), pathspec},
                                   64 * 1024, &error);
    if (!error.isEmpty())
        return fencedError(error);
    for (const QByteArray& selected : tracked.split('\0') + changed.split('\0')) {
        if (!selected.isEmpty() && QString::fromUtf8(selected) != relative)
            return failure(QStringLiteral("invalid_path"));
    }
    // diff-index, unlike porcelain `git diff`, never rewrites the index for stat-only changes.
    QByteArray output =
        git({QStringLiteral("diff-index"), QStringLiteral("-p"), QStringLiteral("--no-ext-diff"),
             QStringLiteral("--no-textconv"), QStringLiteral("--unified=3"), QStringLiteral("HEAD"),
             QStringLiteral("--"), pathspec},
            maximumDiffBytes, &error);
    if (!error.isEmpty())
        return fencedError(error);
    // Git does not include untracked files in `git diff`. Represent them as a bounded synthetic
    // creation diff so Modified has useful and deterministic semantics without staging anything.
    if (tracked.isEmpty() && output.isEmpty()) {
        QJsonObject contentsRequest = request;
        const QJsonObject contents = file(contentsRequest);
        if (!contents.value(QStringLiteral("ok")).toBool())
            return contents;
        const QString text = contents.value(QStringLiteral("content")).toString();
        const QStringList lines = text.split(u'\n');
        output = QStringLiteral("diff --git a/%1 b/%1\nnew file mode 100644\n--- /dev/null\n"
                                "+++ b/%1\n@@ -0,0 +1,%2 @@\n")
                     .arg(relative, QString::number(lines.size()))
                     .toUtf8();
        for (const QString& line : lines) {
            output += '+';
            output += line.toUtf8();
            output += '\n';
            if (output.size() > maximumDiffBytes)
                return failure(QStringLiteral("oversized"));
        }
    }
    if (!textContent(output))
        return failure(QStringLiteral("binary"));
    QJsonObject result{{QStringLiteral("path"), relative},
                       {QStringLiteral("content"), QString::fromUtf8(output)},
                       {QStringLiteral("byte_count"), output.size()}};
    (void)finishRevision(expected, &result);
    return result;
}

QJsonObject ReadOnlyWorkspaceFiles::search(const QJsonObject& request) const {
    if (!scopeMatches(request))
        return failure(QStringLiteral("scope_forbidden"));
    const QString expected = request.value(QStringLiteral("revision")).toString();
    if (const QString error = startRevision(expected); !error.isEmpty())
        return failure(error);
    const QString query = request.value(QStringLiteral("query")).toString();
    if (query.size() < 2 || query.size() > 256)
        return failure(QStringLiteral("invalid_query"));
    const int limit =
        qBound(1, request.value(QStringLiteral("limit")).toInt(50), maximumSearchResults);
    QJsonArray matches;
    qsizetype returnedBytes = 0;
    int scannedFiles = 0;
    QElapsedTimer deadline;
    deadline.start();
    // In a repository, Git lists the candidates without its ignored folders (node_modules,
    // build output), so the budget goes to source files. Elsewhere, walk the tree.
    QString listError;
    const QByteArray listed =
        git({QStringLiteral("ls-files"), QStringLiteral("-z"), QStringLiteral("--cached"),
             QStringLiteral("--others"), QStringLiteral("--exclude-standard")},
            16 * 1024 * 1024, &listError);
    QStringList candidates;
    if (listError.isEmpty()) {
        for (const QByteArray& path : listed.split('\0')) {
            if (!path.isEmpty())
                candidates.append(QString::fromUtf8(path));
        }
    }
    // Walking the tree skips the ignore policy, so it is only for a folder that is not a
    // repository; a repository whose listing fails shares nothing.
    if (!listError.isEmpty() && insideRepository())
        return failure(listError);
    std::optional<QDirIterator> walk;
    if (!listError.isEmpty())
        walk.emplace(canonicalRoot_, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                     QDirIterator::Subdirectories);
    qsizetype nextCandidate = 0;
    const auto hasNext = [&] { return walk ? walk->hasNext() : nextCandidate < candidates.size(); };
    while (hasNext() && matches.size() < limit && returnedBytes < maximumDiffBytes &&
           scannedFiles < 5'000 && deadline.elapsed() < 350) {
        const QString absolute =
            walk ? walk->next() : QDir(canonicalRoot_).filePath(candidates.at(nextCandidate++));
        ++scannedFiles;
        const QFileInfo info(absolute);
        const QString relative = QDir(canonicalRoot_).relativeFilePath(absolute);
        const QStringList parts = relative.split(u'/', Qt::SkipEmptyParts);
        if (std::any_of(parts.cbegin(), parts.cend(), isGitSegment) ||
            workspace_exposure::isSecretPath(relative) || info.isSymLink() ||
            info.size() > maximumSearchFileBytes || !info.isReadable())
            continue;
        QFile file;
        qint64 size = 0;
        if (openBeneath(canonicalRoot_, relativeClean(relative), &file, &size) !=
                OpenResult::Opened ||
            size > maximumSearchFileBytes)
            continue;
        const QByteArray bytes = file.read(maximumSearchFileBytes + 1);
        if (bytes.size() > maximumSearchFileBytes || !textContent(bytes))
            continue;
        const QString text = QString::fromUtf8(bytes);
        qsizetype index = 0;
        while (matches.size() < limit &&
               (index = text.indexOf(query, index, Qt::CaseInsensitive)) >= 0) {
            const int line = text.left(index).count(u'\n') + 1;
            const qsizetype start = std::max(qsizetype{0}, text.lastIndexOf(u'\n', index) + 1);
            qsizetype end = text.indexOf(u'\n', index);
            if (end < 0)
                end = text.size();
            QString preview = text.mid(start, end - start).trimmed().left(240);
            returnedBytes += preview.toUtf8().size() + relative.toUtf8().size();
            if (returnedBytes > maximumDiffBytes)
                break;
            matches.append(QJsonObject{{QStringLiteral("path"), relative},
                                       {QStringLiteral("line"), line},
                                       {QStringLiteral("preview"), preview}});
            index += query.size();
        }
    }
    QJsonObject result{{QStringLiteral("matches"), matches},
                       {QStringLiteral("truncated"),
                        matches.size() == limit || returnedBytes >= maximumDiffBytes ||
                            (hasNext() && (scannedFiles >= 5'000 || deadline.elapsed() >= 350))}};
    (void)finishRevision(expected, &result);
    return result;
}

QString ReadOnlyWorkspaceFiles::readImage(const QString& relativePath, QByteArray* bytes) const {
    const QString relative = relativeClean(relativePath);
    QString error;
    if (relative.isEmpty() || resolved(relative, &error).isEmpty())
        return error.isEmpty() ? QStringLiteral("invalid_path") : error;
    if (const QString denied = exposureError(relative); !denied.isEmpty())
        return denied;
    QFile source;
    qint64 size = 0;
    switch (openBeneath(canonicalRoot_, relative, &source, &size)) {
    case OpenResult::Opened:
        break;
    case OpenResult::Missing:
        return QStringLiteral("missing");
    case OpenResult::Symlink:
        return QStringLiteral("symlink_unsupported");
    case OpenResult::NotRegular:
    case OpenResult::Failed:
        return QStringLiteral("permission_denied");
    }
    if (size > maximumImageBytes)
        return QStringLiteral("oversized");
    *bytes = source.read(maximumImageBytes + 1);
    if (bytes->size() > maximumImageBytes)
        return QStringLiteral("oversized");
    return {};
}

} // namespace ketplus::daemon
