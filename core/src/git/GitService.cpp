#include "git/GitService.h"

#include "git/GitParser.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <memory>
#include <utility>

namespace ketplus {
namespace {

constexpr qsizetype maximumCommandOutput = 8 * 1024 * 1024;

struct CommandState final {
    QByteArray output;
    QByteArray error;
    bool completed{false};
    bool timedOut{false};
    bool outputTooLarge{false};
};

QString normalizedPath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
}

// Resolves the folders but keeps the last component, so a symlink stays the link Git tracks
// instead of becoming its target.
QString entryPath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo file(path);
    const QString folder = normalizedPath(file.absolutePath());
    QString name = file.fileName();
#if defined(Q_OS_DARWIN)
    // Qt and Git both report names precomposed on macOS; match them when a caller passes NFD.
    name = name.normalized(QString::NormalizationForm_C);
#endif
    return QDir::cleanPath(QDir(folder).filePath(name));
}

bool isInside(const QString& path, const QString& root) {
    const QString relative = QDir(root).relativeFilePath(path);
    return relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../")) &&
           !QDir::isAbsolutePath(relative);
}

QString literalPathspec(const QString& path) { return QStringLiteral(":(literal)%1").arg(path); }

QString nullDeviceForGit() {
#if defined(Q_OS_WIN)
    return QStringLiteral("NUL");
#else
    return QProcess::nullDevice();
#endif
}

} // namespace

GitService::GitService(QObject* parent)
    : QObject(parent), gitExecutable_(QStandardPaths::findExecutable(QStringLiteral("git"))),
      refreshTimer_(new QTimer(this)) {
    refreshTimer_->setSingleShot(true);
    connect(refreshTimer_, &QTimer::timeout, this, &GitService::refresh);
    const auto gitStateChanged = [this] {
        watchGitState(); // A replaced file drops out of the watch list.
        scheduleRefresh(300);
    };
    connect(&gitStateWatcher_, &QFileSystemWatcher::directoryChanged, this, gitStateChanged);
    connect(&gitStateWatcher_, &QFileSystemWatcher::fileChanged, this, gitStateChanged);
}

GitService::~GitService() {
    refreshTimer_->stop();
    gitStateWatcher_.disconnect(this);
    // QProcess emits finished from its destructor. Its callbacks use snapshot/worktree
    // members, so disconnect and stop producers before those derived members are freed.
    for (auto* process : findChildren<QProcess*>()) {
        process->disconnect();
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
}

bool GitService::isAvailable() const noexcept { return !gitExecutable_.isEmpty(); }

const GitSnapshot& GitService::snapshot() const noexcept { return snapshot_; }

const QVector<GitWorktree>& GitService::worktrees() const noexcept { return worktrees_; }

void GitService::setWorkspacePath(const QString& path) {
    refreshTimer_->stop();
    workspacePath_ = normalizedPath(path);
    repositoryRoot_.clear();
    gitDirectory_.clear();
    watchGitState();
    snapshot_ = {};
    worktrees_.clear();
    ++workspaceGeneration_;
    ++diffGeneration_;
    ++refreshGeneration_;
    emit snapshotChanged(snapshot_);

    if (!isAvailable() || workspacePath_.isEmpty()) {
        emit worktreesChanged(worktrees_);
        return;
    }

    const quint64 generation = workspaceGeneration_;
    runGit({QStringLiteral("-C"), workspacePath_, QStringLiteral("rev-parse"),
            QStringLiteral("--show-toplevel"), QStringLiteral("--absolute-git-dir")},
           workspacePath_,
           [this, generation](const int exitCode, const QByteArray& output, const QByteArray&,
                              const bool completedNormally) {
               if (generation != workspaceGeneration_ || !completedNormally || exitCode != 0) {
                   if (generation == workspaceGeneration_) {
                       emit worktreesChanged(worktrees_);
                   }
                   return;
               }
               const QStringList lines =
                   QString::fromUtf8(output).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
               const QString root = lines.value(0).trimmed();
               if (root.isEmpty()) {
                   emit worktreesChanged(worktrees_);
                   return;
               }
               repositoryRoot_ = normalizedPath(root);
               gitDirectory_ = normalizedPath(lines.value(1).trimmed());
               watchGitState();
               refresh();
           });
}

void GitService::refresh() {
    if (!isAvailable() || repositoryRoot_.isEmpty()) {
        return;
    }

    const QString root = repositoryRoot_;
    const quint64 workspaceGeneration = workspaceGeneration_;
    const quint64 refreshGeneration = ++refreshGeneration_;
    runGit({QStringLiteral("-C"), root, QStringLiteral("status"),
            QStringLiteral("--porcelain=v2"), QStringLiteral("-z"),
            QStringLiteral("--branch"), QStringLiteral("--untracked-files=all")},
           root,
           [this, root, workspaceGeneration, refreshGeneration](
               const int exitCode, const QByteArray& output, const QByteArray& error,
               const bool completedNormally) {
               if (workspaceGeneration != workspaceGeneration_ ||
                   refreshGeneration != refreshGeneration_) {
                   return;
               }
               if (!completedNormally || exitCode != 0) {
                   emit errorOccurred(QString::fromUtf8(error).trimmed());
                   return;
               }
               snapshot_ = git_parser::parseStatus(output, root);
               emit snapshotChanged(snapshot_);
           });

    runGit({QStringLiteral("-C"), root, QStringLiteral("worktree"), QStringLiteral("list"),
            QStringLiteral("--porcelain"), QStringLiteral("-z")},
           root,
           [this, root, workspaceGeneration, refreshGeneration](
               const int exitCode, const QByteArray& output, const QByteArray&,
               const bool completedNormally) {
               if (workspaceGeneration != workspaceGeneration_ ||
                   refreshGeneration != refreshGeneration_ || !completedNormally ||
                   exitCode != 0) {
                   return;
               }
               worktrees_ = git_parser::parseWorktrees(output, root);
               emit worktreesChanged(worktrees_);
           });
}

void GitService::scheduleRefresh(const int delayMilliseconds) {
    if (!repositoryRoot_.isEmpty()) {
        refreshTimer_->start(delayMilliseconds);
    }
}

void GitService::requestDiff(const QString& filePath, const GitDiffMode mode) {
    const quint64 diffGeneration = ++diffGeneration_;
    if (repositoryRoot_.isEmpty()) {
        emit diffReady(filePath, mode, {},
                       QStringLiteral("The open folder is not a Git repository."));
        return;
    }

    const QString absolutePath = entryPath(filePath);
    if (!isInside(absolutePath, repositoryRoot_)) {
        emit diffReady(filePath, mode, {},
                       QStringLiteral("The file is outside the active repository."));
        return;
    }

    const QString root = repositoryRoot_;
    const QString relativePath = QDir(root).relativeFilePath(absolutePath);
    QStringList arguments{QStringLiteral("-C"), root, QStringLiteral("diff"),
                          QStringLiteral("--no-ext-diff"), QStringLiteral("--no-color")};
    int acceptedDifferenceExitCode = 0;
    // Before the first commit, the working tree is compared with nothing: an entry on disk is
    // entirely new, and one that is only in the index has no combined change. A symlink counts
    // as on disk even when its target is missing, because Git tracks the link itself.
    const bool unbornHead = snapshot_.head == QStringLiteral("(initial)");
    const bool combinedWithoutHead = unbornHead && mode == GitDiffMode::Combined;
    const QFileInfo entry(absolutePath);
    const bool entryOnDisk = entry.isSymLink() || entry.exists();
    if (combinedWithoutHead && !entryOnDisk) {
        emit diffReady(filePath, mode, {}, {});
        return;
    }
    if (isUntracked(relativePath) ||
        (combinedWithoutHead && (entry.isSymLink() || entry.isFile()))) {
        arguments.append({QStringLiteral("--no-index"), QStringLiteral("--"),
                          nullDeviceForGit(), absolutePath});
        acceptedDifferenceExitCode = 1;
    } else {
        arguments.append(QStringLiteral("--find-renames"));
        if (mode == GitDiffMode::Staged || combinedWithoutHead) {
            // Without HEAD, the staged copy is the closest combined view of a folder entry such
            // as a submodule.
            arguments.append(QStringLiteral("--cached"));
        } else if (mode == GitDiffMode::Combined) {
            arguments.append(QStringLiteral("HEAD"));
        }
        arguments.append(QStringLiteral("--"));

        for (const auto& file : snapshot_.files) {
            if (file.path == relativePath && !file.originalPath.isEmpty()) {
                arguments.append(literalPathspec(file.originalPath));
                break;
            }
        }
        arguments.append(literalPathspec(relativePath));
    }

    runGit(
        arguments, root,
        [this, filePath, mode, acceptedDifferenceExitCode,
         diffGeneration](const int exitCode, const QByteArray& output, const QByteArray& error,
                         const bool completedNormally) {
            // A newer request or a workspace switch makes this reply stale.
            if (diffGeneration != diffGeneration_) {
                return;
            }
            if (!completedNormally || (exitCode != 0 && exitCode != acceptedDifferenceExitCode)) {
                emit diffReady(filePath, mode, {}, QString::fromUtf8(error).trimmed());
                return;
            }
            emit diffReady(filePath, mode, QString::fromUtf8(output), {});
        },
        20000);
}

void GitService::runGit(const QStringList& arguments, const QString& workingDirectory,
                        CommandCallback callback, const int timeoutMilliseconds) {
    if (!isAvailable()) {
        callback(-1, {}, QByteArray("Git executable was not found."), false);
        return;
    }

    auto* process = new QProcess(this);
    auto* timeout = new QTimer(process);
    auto state = std::make_shared<CommandState>();
    timeout->setSingleShot(true);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(workingDirectory);
    process->setProgram(gitExecutable_);
    process->setArguments(arguments);
    process->setProcessChannelMode(QProcess::SeparateChannels);

    const auto collectOutput = [process, state] {
        state->output.append(process->readAllStandardOutput());
        state->error.append(process->readAllStandardError());
        if (state->output.size() + state->error.size() > maximumCommandOutput) {
            state->outputTooLarge = true;
            process->kill();
        }
    };
    connect(process, &QProcess::readyReadStandardOutput, process, collectOutput);
    connect(process, &QProcess::readyReadStandardError, process, collectOutput);
    connect(timeout, &QTimer::timeout, process, [process, state] {
        state->timedOut = true;
        process->kill();
    });

    auto finish = [process, timeout, state, callback = std::move(callback),
                   collectOutput](const int exitCode, const bool completedNormally) mutable {
        if (state->completed) {
            return;
        }
        state->completed = true;
        timeout->stop();
        collectOutput();
        if (state->timedOut) {
            state->error = QByteArray("Git command timed out.");
        } else if (state->outputTooLarge) {
            state->error = QByteArray("Git output exceeded the 8 MiB safety limit.");
        }
        callback(exitCode, state->output, state->error,
                 completedNormally && !state->timedOut && !state->outputTooLarge);
        process->deleteLater();
    };

    connect(process, &QProcess::finished, this,
            [finish](const int exitCode, const QProcess::ExitStatus status) mutable {
                finish(exitCode, status == QProcess::NormalExit);
            });
    connect(process, &QProcess::errorOccurred, this,
            [finish](const QProcess::ProcessError error) mutable {
                if (error == QProcess::FailedToStart) {
                    finish(-1, false);
                }
            });

    process->start();
    timeout->start(timeoutMilliseconds);
}

void GitService::watchGitState() {
    QStringList desired;
    if (!gitDirectory_.isEmpty()) {
        // The folder sees index and HEAD being replaced; logs/HEAD is appended for every
        // commit, checkout and reset, including branch moves that leave HEAD itself unchanged.
        desired.append(gitDirectory_);
        const QString reflog = QDir(gitDirectory_).filePath(QStringLiteral("logs/HEAD"));
        if (QFileInfo::exists(reflog)) {
            desired.append(reflog);
        }
    }
    const QStringList watched = gitStateWatcher_.files() + gitStateWatcher_.directories();
    QStringList removed;
    for (const QString& path : watched) {
        if (!desired.contains(path)) {
            removed.append(path);
        }
    }
    if (!removed.isEmpty()) {
        gitStateWatcher_.removePaths(removed);
    }
    QStringList added;
    for (const QString& path : desired) {
        if (!watched.contains(path)) {
            added.append(path);
        }
    }
    if (!added.isEmpty()) {
        gitStateWatcher_.addPaths(added);
    }
}

bool GitService::isUntracked(const QString& relativePath) const {
    for (const auto& file : snapshot_.files) {
        if (file.untracked && file.path == relativePath) {
            return true;
        }
    }
    return false;
}

} // namespace ketplus
