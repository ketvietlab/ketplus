#include "snapshot/WorkspaceApply.h"

#include "snapshot/WorktreeSnapshot.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace ketplus::runner::snapshot {
namespace {

QString oneLine(const QByteArray& output) { return QString::fromUtf8(output).trimmed(); }

bool inside(const QString& path, const QString& root) {
    return path == root || path.startsWith(root + QLatin1Char('/'));
}

QString mirrorObjectFormat(const QString& mirror) {
    QFile config(QDir(mirror).filePath(QStringLiteral("config")));
    if (!config.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(config.readAll())
                   .toLower()
                   .contains(QStringLiteral("objectformat = sha256"))
               ? QStringLiteral("sha256")
               : QStringLiteral("sha1");
}

std::optional<Error> prepareMirror(const SealedGit& git, const QString& mirror,
                                   const QString& objectFormat) {
    if (QFileInfo::exists(QDir(mirror).filePath(QStringLiteral("HEAD")))) {
        if (mirrorObjectFormat(mirror) != objectFormat)
            return Error{ErrorCode::StoreMismatch,
                         QStringLiteral("the mirror uses another object format")};
        return std::nullopt;
    }
    if (!QDir().mkpath(mirror))
        return Error{ErrorCode::Io, QStringLiteral("cannot create %1").arg(mirror)};
    if (const auto init = git.initBare(mirror, objectFormat); !init)
        return init.error();
    return std::nullopt;
}

struct TreeCheck {
    qint64 entries = 0;
    qint64 bytes = 0;
};

// Walks the snapshot tree before anything is written: every path must stay below the
// workspace and the expanded size must fit the limits.
std::optional<Error> checkTree(const SealedGit& git, const QString& mirror, const QString& commit,
                               const ApplyLimits& limits) {
    GitRun list;
    list.gitDir = mirror;
    list.arguments = {QStringLiteral("ls-tree"),     QStringLiteral("-r"),
                      QStringLiteral("-z"),          QStringLiteral("-l"),
                      QStringLiteral("--full-tree"), commit};
    const auto output = git.run(list);
    if (!output)
        return Error{ErrorCode::SnapshotIncomplete, output.error().message};
    static const QRegularExpression entry(
        QStringLiteral("^(100644|100755|120000|160000) (blob|commit) [0-9a-f]+ +(-|\\d+)$"));
    TreeCheck total;
    for (const QByteArray& raw : output.value().out.split('\0')) {
        if (raw.isEmpty())
            continue;
        const qsizetype tab = raw.indexOf('\t');
        if (tab < 0)
            return Error{ErrorCode::PackInvalid, QStringLiteral("unreadable tree listing")};
        const QString meta = QString::fromUtf8(raw.left(tab));
        const QString path = QString::fromUtf8(raw.mid(tab + 1));
        const auto match = entry.match(meta);
        if (!match.hasMatch())
            return Error{ErrorCode::UnsafePath, QStringLiteral("unsupported entry: %1").arg(meta)};
        if (!isSafeSnapshotPath(path))
            return Error{ErrorCode::UnsafePath, QStringLiteral("unsafe path: %1").arg(path)};
        if (++total.entries > limits.maxEntries)
            return Error{ErrorCode::CheckoutTooLarge, QStringLiteral("too many entries")};
        if (match.captured(3) != QStringLiteral("-"))
            total.bytes += match.captured(3).toLongLong();
        if (total.bytes > limits.maxCheckoutBytes)
            return Error{ErrorCode::CheckoutTooLarge, QStringLiteral("snapshot too large")};
    }
    return std::nullopt;
}

} // namespace

bool isSafeSnapshotPath(const QString& path) {
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\')) ||
        path.contains(QChar(0)))
        return false;
    for (const QString& component : path.split(QLatin1Char('/'))) {
        if (component.isEmpty() || component == QStringLiteral(".") ||
            component == QStringLiteral(".."))
            return false;
        // `.git`, any case, with the trailing dots or spaces some file systems drop.
        QString bare = component.toLower();
        while (bare.endsWith(QLatin1Char('.')) || bare.endsWith(QLatin1Char(' ')))
            bare.chop(1);
        if (bare == QStringLiteral(".git") || bare == QStringLiteral("git~1"))
            return false;
    }
    return true;
}

Result<Applied> applySnapshot(const ApplyRequest& request, const GitTool& tool) {
    static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
    if (!isObjectId(request.commit) ||
        !digest.match(QString::fromLatin1(request.sha256)).hasMatch())
        return Error{ErrorCode::InvalidArgument, QStringLiteral("malformed snapshot id or digest")};
    if (!QDir::isAbsolutePath(request.mirrorPath) || !QDir::isAbsolutePath(request.workspacePath))
        return Error{ErrorCode::InvalidArgument, QStringLiteral("paths must be absolute")};
    const QString mirror = QDir::cleanPath(request.mirrorPath);
    const QString workspace = QDir::cleanPath(request.workspacePath);
    if (inside(mirror, workspace) || inside(workspace, mirror))
        return Error{ErrorCode::InvalidArgument, QStringLiteral("mirror and workspace overlap")};

    // The pack itself: size, then digest, before git reads a byte of it.
    const QFileInfo pack(request.packPath);
    if (!pack.isFile())
        return Error{ErrorCode::InvalidArgument,
                     QStringLiteral("no pack at %1").arg(request.packPath)};
    if (pack.size() > request.limits.maxPackBytes)
        return Error{ErrorCode::PackTooLarge, QStringLiteral("pack is %1 bytes").arg(pack.size())};
    if (fileSha256(request.packPath) != request.sha256)
        return Error{ErrorCode::PackDigestMismatch, QStringLiteral("pack digest differs")};

    const SealedGit git(tool);
    if (const auto version = git.checkVersion(); !version)
        return version.error();
    const QString format =
        request.commit.size() == 64 ? QStringLiteral("sha256") : QStringLiteral("sha1");
    if (auto error = prepareMirror(git, mirror, format))
        return *error;

    // Strict indexing runs git's fsck checks on every object (malformed trees, `..` or
    // `.git` entries, bad .gitmodules) and bounds the input once more.
    GitRun index;
    index.gitDir = mirror;
    index.inputFile = request.packPath;
    index.arguments = {QStringLiteral("index-pack"), QStringLiteral("--stdin"),
                       QStringLiteral("--strict"),
                       QStringLiteral("--max-input-size=%1").arg(request.limits.maxPackBytes)};
    if (const auto indexed = git.run(index); !indexed)
        return Error{ErrorCode::PackInvalid, indexed.error().message};

    GitRun check;
    check.gitDir = mirror;
    check.arguments = {QStringLiteral("cat-file"), QStringLiteral("-t"), request.commit};
    const auto type = git.run(check);
    if (!type || oneLine(type.value().out) != QStringLiteral("commit"))
        return Error{ErrorCode::SnapshotIncomplete, QStringLiteral("the pack lacks the snapshot")};
    check.arguments = {QStringLiteral("rev-list"), QStringLiteral("--objects"),
                       QStringLiteral("--missing=error"), request.commit};
    check.outputFile = QProcess::nullDevice();
    if (const auto connected = git.run(check); !connected)
        return Error{ErrorCode::SnapshotIncomplete, connected.error().message};
    if (auto error = checkTree(git, mirror, request.commit, request.limits))
        return *error;

    // Only now is the workspace touched.
    if (!QDir().mkpath(workspace))
        return Error{ErrorCode::Io, QStringLiteral("cannot create %1").arg(workspace)};
    const QString canonical = QFileInfo(workspace).canonicalFilePath();
    if (inside(QFileInfo(mirror).canonicalFilePath(), canonical))
        return Error{ErrorCode::InvalidArgument, QStringLiteral("mirror and workspace overlap")};
    const QByteArray key =
        QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    const QString ref = QStringLiteral("refs/ketplus/workspaces/") + QString::fromLatin1(key);
    QDir().mkpath(QDir(mirror).filePath(QStringLiteral("ketplus-workspaces")));

    Applied applied;
    applied.commit = request.commit;
    GitRun previous;
    previous.gitDir = mirror;
    previous.arguments = {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                          QStringLiteral("--quiet"), ref};
    if (const auto held = git.run(previous))
        applied.previousCommit = oneLine(held.value().out);

    // Settle the stat cache first: a file whose mtime moved but whose content did not is
    // marked clean here, so the checkout below leaves it (and its mtime) alone. Exit status 1
    // only means some entries differ, which the checkout then rewrites.
    GitRun refresh;
    refresh.gitDir = mirror;
    refresh.workTree = canonical;
    refresh.indexFile = QDir(mirror).filePath(QStringLiteral("ketplus-workspaces/") +
                                              QString::fromLatin1(key) + QStringLiteral(".index"));
    if (QFileInfo::exists(refresh.indexFile)) {
        refresh.arguments = {QStringLiteral("update-index"), QStringLiteral("-q"),
                             QStringLiteral("--refresh"), QStringLiteral("--ignore-missing"),
                             QStringLiteral("--unmerged")};
        if (const auto refreshed = git.run(refresh);
            !refreshed && refreshed.error().code != ErrorCode::GitFailed)
            return refreshed.error();
    }

    GitRun checkout;
    checkout.gitDir = mirror;
    checkout.workTree = canonical;
    checkout.indexFile = refresh.indexFile;
    checkout.arguments = {QStringLiteral("read-tree"), QStringLiteral("--reset"),
                          QStringLiteral("-u"), request.commit};
    if (const auto done = git.run(checkout); !done)
        return done.error();

    GitRun record;
    record.gitDir = mirror;
    record.arguments = {QStringLiteral("update-ref"), ref, request.commit};
    if (const auto done = git.run(record); !done)
        return done.error();
    return applied;
}

} // namespace ketplus::runner::snapshot
