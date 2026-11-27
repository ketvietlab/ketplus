#include "snapshot/SnapshotStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace ketplus::runner::snapshot {

namespace {

constexpr int maximumRefChars = 200;
constexpr int maximumSymrefDepth = 5;
constexpr qint64 maximumRefFileBytes = 4096;
constexpr qint64 maximumPackedRefsBytes = 64LL * 1024 * 1024;

QString prefix() { return QString::fromLatin1(storeRefPrefix); }

std::optional<Error> checkStore(const QString& storePath) {
    if (!QDir::isAbsolutePath(storePath) ||
        !QFileInfo::exists(QDir(storePath).filePath(QStringLiteral("HEAD"))))
        return Error{ErrorCode::InvalidArgument, QStringLiteral("no snapshot store")};
    return std::nullopt;
}

// The read-only view of the repository's objects the store was bound to.
QString alternate(const QString& storePath) {
    QFile file(QDir(storePath).filePath(QStringLiteral("ketplus-source-objects")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QStringLiteral("GIT_ALTERNATE_OBJECT_DIRECTORIES=") + QString::fromUtf8(file.readAll());
}

Result<GitOutput> runInStore(const QString& storePath, const QStringList& arguments,
                             const GitTool& tool) {
    GitRun run;
    run.gitDir = QDir::cleanPath(storePath);
    run.arguments = arguments;
    if (const QString objects = alternate(run.gitDir); !objects.isEmpty())
        run.extraEnvironment = {objects};
    return SealedGit(tool).run(run);
}

bool isRefPath(const QString& ref) {
    static const QRegularExpression form(
        QStringLiteral("^refs/[A-Za-z0-9_.+-]+(/[A-Za-z0-9_.+-]+)*$"));
    return form.match(ref).hasMatch() && !ref.contains(QStringLiteral("..")) &&
           !ref.contains(QStringLiteral("/.")) && !ref.endsWith(QStringLiteral(".lock"));
}

bool isHexOf(const QString& value, const QString& objectFormat) {
    const int length = objectFormat == QStringLiteral("sha256") ? 64 : 40;
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]+$"));
    return value.size() == length && hex.match(value).hasMatch();
}

// The first line of a small regular file, or nullopt when it is missing, a link or too large.
std::optional<QString> firstLine(const QString& path, bool* missing) {
    const QFileInfo info(path);
    *missing = !info.exists() && !info.isSymLink();
    if (!info.exists() || info.isSymLink() || !info.isFile() || info.size() > maximumRefFileBytes)
        return std::nullopt;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    return QString::fromUtf8(file.readLine(maximumRefFileBytes)).trimmed();
}

// The id packed-refs records for `ref`; empty when it holds none, nullopt when unreadable.
std::optional<QString> packedRef(const QString& commonDir, const QString& ref) {
    const QString path = QDir(commonDir).filePath(QStringLiteral("packed-refs"));
    const QFileInfo info(path);
    if (!info.exists())
        return QString{};
    if (info.isSymLink() || !info.isFile() || info.size() > maximumPackedRefsBytes)
        return std::nullopt;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char('^')))
            continue;
        const qsizetype space = line.indexOf(QLatin1Char(' '));
        if (space > 0 && line.mid(space + 1) == ref)
            return line.left(space);
    }
    return QString{};
}

} // namespace

bool isStoreRefName(const QString& name) {
    static const QRegularExpression form(QStringLiteral(
        "^[A-Za-z0-9_-]+(\\.[A-Za-z0-9_-]+)*(/[A-Za-z0-9_-]+(\\.[A-Za-z0-9_-]+)*)*$"));
    return name.size() <= maximumRefChars && form.match(name).hasMatch() &&
           !name.endsWith(QStringLiteral(".lock"));
}

Result<bool> keepRef(const QString& storePath, const QString& name, const QString& commit,
                     const GitTool& tool) {
    if (auto error = checkStore(storePath))
        return *error;
    if (!isStoreRefName(name) || !isObjectId(commit))
        return Error{ErrorCode::InvalidArgument, QStringLiteral("malformed ref or snapshot id")};
    const auto type =
        runInStore(storePath, {QStringLiteral("cat-file"), QStringLiteral("-t"), commit}, tool);
    if (!type || type.value().out.trimmed() != "commit")
        return Error{ErrorCode::InvalidArgument, QStringLiteral("unknown snapshot")};
    const auto updated = runInStore(
        storePath,
        {QStringLiteral("update-ref"), QStringLiteral("--no-deref"), prefix() + name, commit},
        tool);
    if (!updated)
        return updated.error();
    return true;
}

Result<bool> dropRef(const QString& storePath, const QString& name, const GitTool& tool) {
    if (auto error = checkStore(storePath))
        return *error;
    if (!isStoreRefName(name))
        return Error{ErrorCode::InvalidArgument, QStringLiteral("malformed ref")};
    const auto removed = runInStore(storePath,
                                    {QStringLiteral("update-ref"), QStringLiteral("--no-deref"),
                                     QStringLiteral("-d"), prefix() + name},
                                    tool);
    if (!removed)
        return removed.error();
    return true;
}

Result<QMap<QString, QString>> listRefs(const QString& storePath, const GitTool& tool) {
    if (auto error = checkStore(storePath))
        return *error;
    const auto listed = runInStore(storePath,
                                   {QStringLiteral("for-each-ref"),
                                    QStringLiteral("--format=%(objectname) %(refname)"), prefix()},
                                   tool);
    if (!listed)
        return listed.error();
    QMap<QString, QString> refs;
    for (const QByteArray& line : listed.value().out.split('\n')) {
        const QString text = QString::fromUtf8(line).trimmed();
        const qsizetype space = text.indexOf(QLatin1Char(' '));
        if (space <= 0)
            continue;
        const QString ref = text.mid(space + 1);
        if (ref.startsWith(prefix()))
            refs.insert(ref.mid(prefix().size()), text.left(space));
    }
    return refs;
}

Result<bool> pruneStore(const QString& storePath, const QDateTime& expireBefore,
                        const GitTool& tool) {
    if (auto error = checkStore(storePath))
        return *error;
    if (!expireBefore.isValid())
        return Error{ErrorCode::InvalidArgument, QStringLiteral("no expiry time")};
    const QString expire = expireBefore.toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) +
                           QStringLiteral(" +0000");
    const auto pruned = runInStore(
        storePath, {QStringLiteral("prune"), QStringLiteral("--expire=") + expire}, tool);
    if (!pruned)
        return pruned.error();
    return true;
}

Result<bool> snapshotComplete(const QString& storePath, const QString& commit,
                              const GitTool& tool) {
    if (auto error = checkStore(storePath))
        return *error;
    if (!isObjectId(commit))
        return Error{ErrorCode::InvalidArgument, QStringLiteral("malformed snapshot id")};
    const auto listed = runInStore(storePath,
                                   {QStringLiteral("rev-list"), QStringLiteral("--objects"),
                                    QStringLiteral("--missing=print"), commit},
                                   tool);
    if (!listed)
        return listed.error().code == ErrorCode::GitFailed ? Result<bool>(false)
                                                           : Result<bool>(listed.error());
    const QList<QByteArray> lines = listed.value().out.split('\n');
    for (const QByteArray& line : lines)
        if (line.startsWith('?'))
            return false;
    return true;
}

SourceHead readSourceHead(const RepositoryLayout& layout) {
    SourceHead head;
    if (QFileInfo::exists(QDir(layout.commonDir).filePath(QStringLiteral("reftable")))) {
        head.unreadable = true;
        return head;
    }
    bool missing = false;
    auto line = firstLine(QDir(layout.gitDir).filePath(QStringLiteral("HEAD")), &missing);
    for (int depth = 0; depth <= maximumSymrefDepth; ++depth) {
        if (!line) {
            head.unreadable = true;
            return head;
        }
        if (isHexOf(*line, layout.objectFormat)) {
            head.commit = *line;
            return head;
        }
        if (!line->startsWith(QStringLiteral("ref: "))) {
            head.unreadable = true;
            return head;
        }
        const QString ref = line->mid(5).trimmed();
        if (!isRefPath(ref)) {
            head.unreadable = true;
            return head;
        }
        line = firstLine(QDir(layout.commonDir).filePath(ref), &missing);
        if (!line && missing) {
            const auto packed = packedRef(layout.commonDir, ref);
            if (!packed) {
                head.unreadable = true;
            } else if (packed->isEmpty()) {
                head.unborn = true;
            } else if (isHexOf(*packed, layout.objectFormat)) {
                head.commit = *packed;
            } else {
                head.unreadable = true;
            }
            return head;
        }
    }
    head.unreadable = true;
    return head;
}

} // namespace ketplus::runner::snapshot
