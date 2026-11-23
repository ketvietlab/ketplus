#include "app/AgentWorktreeCreator.h"

#include "LinkedWorktreeRemoval.h"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QtConcurrentRun>

namespace ketplus {
namespace {
constexpr qsizetype maximumOutputBytes = 64 * 1024;
constexpr int creationTimeoutMs = 30'000;
} // namespace

AgentWorktreeCreator::AgentWorktreeCreator(QObject* parent) : QObject(parent) {
    setObjectName(QStringLiteral("agentWorktreeCreator"));
    process_.setProcessChannelMode(QProcess::MergedChannels);
    timeout_.setSingleShot(true);

    connect(&process_, &QProcess::readyReadStandardOutput, this,
            &AgentWorktreeCreator::appendOutput);
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](const int exitCode, const QProcess::ExitStatus exitStatus) {
                appendOutput();
                finish(exitStatus == QProcess::NormalExit && exitCode == 0);
            });
    connect(&process_, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish(false, QStringLiteral("Could not start Git."));
        }
    });
    connect(&timeout_, &QTimer::timeout, this, [this] {
        if (!active_) {
            return;
        }
        timedOut_ = true;
        process_.kill();
    });
}

QString AgentWorktreeCreator::directoryNameForBranch(const QString& branchName) {
    QString name = branchName.trimmed();
    name.replace(QLatin1Char('/'), QLatin1Char('-'));
    name.replace(QLatin1Char('\\'), QLatin1Char('-'));
    return name;
}

QString AgentWorktreeCreator::worktreePath(const QString& primaryWorktreePath,
                                           const QString& branchName) {
    const QFileInfo primary(QDir::cleanPath(primaryWorktreePath));
    const QString directoryName =
        primary.fileName() + QLatin1Char('-') + directoryNameForBranch(branchName);
    return QDir::cleanPath(primary.dir().filePath(directoryName));
}

bool AgentWorktreeCreator::isRunning() const noexcept { return active_; }

void AgentWorktreeCreator::create(const QString& repositoryPath, const QString& primaryWorktreePath,
                                  const QString& branchName, const QString& baseBranch) {
    if (active_) {
        emit failed(QStringLiteral("A worktree is already being created."));
        return;
    }

    branchName_ = branchName.trimmed();
    const QString base = baseBranch.trimmed();
    if (base.isEmpty() || base.startsWith('-')) {
        emit failed(QStringLiteral("Select a valid base branch."));
        return;
    }
    if (branchName_.isEmpty()) {
        emit failed(QStringLiteral("Enter a branch name."));
        return;
    }

    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty()) {
        emit failed(QStringLiteral("Git was not found on PATH."));
        return;
    }

    const QFileInfo primary(primaryWorktreePath);
    const QFileInfo repository(repositoryPath);
    if (!primary.isAbsolute() || !primary.isDir() || !repository.isAbsolute() ||
        !repository.isDir()) {
        emit failed(QStringLiteral("The repository or primary worktree is unavailable."));
        return;
    }
    targetPath_ = worktreePath(primary.canonicalFilePath(), branchName_);
    const QFileInfo target(targetPath_);
    if (target.absolutePath() != primary.canonicalPath() ||
        branchName_.contains(QLatin1Char('\\')) || branchName_.contains(QChar(u'\0'))) {
        emit failed(QStringLiteral("Enter a valid branch name."));
        return;
    }
    if (target.exists() || target.isSymLink()) {
        emit failed(QStringLiteral("The inferred worktree path already exists:\n%1")
                        .arg(QDir::toNativeSeparators(targetPath_)));
        return;
    }

    output_.clear();
    pruning_ = false;
    timedOut_ = false;
    active_ = true;

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const auto& key : environment.keys())
        if (key.startsWith(QStringLiteral("GIT_"), Qt::CaseInsensitive))
            environment.remove(key);
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    process_.setProcessEnvironment(environment);
    process_.setWorkingDirectory(repositoryPath);
    process_.setProgram(git);
    process_.setArguments({QStringLiteral("-c"), QStringLiteral("color.ui=false"),
                           QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"),
                           branchName_, QStringLiteral("--"), targetPath_, base});
    process_.start();
    timeout_.start(creationTimeoutMs);
}

void AgentWorktreeCreator::appendOutput() {
    const QByteArray chunk = process_.readAllStandardOutput();
    const qsizetype remaining = maximumOutputBytes - output_.size();
    if (remaining > 0) {
        output_.append(chunk.first(qMin(remaining, chunk.size())));
    }
}

void AgentWorktreeCreator::finish(const bool succeeded, const QString& fallbackError) {
    if (!active_) {
        return;
    }

    timeout_.stop();
    active_ = false;
    if (succeeded) {
        if (pruning_)
            emit pruned();
        else
            emit created(targetPath_, branchName_);
        return;
    }

    QString error = QString::fromLocal8Bit(output_).trimmed();
    if (timedOut_) {
        error = QStringLiteral("Git timed out while updating the worktree.");
    } else if (error.isEmpty()) {
        error = fallbackError.isEmpty() ? QStringLiteral("Git could not update the worktree.")
                                        : fallbackError;
    }
    emit failed(error);
}

void AgentWorktreeCreator::remove(const QString& repositoryPath, const QString& worktreePath,
                                  const QString& expectedIdentity) {
    if (active_) {
        emit failed(QStringLiteral("A worktree operation is already running."));
        return;
    }
    active_ = true;
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, worktreePath] {
        const QString error = watcher->result();
        watcher->deleteLater();
        active_ = false;
        if (error.isEmpty())
            emit removed(worktreePath);
        else
            emit failed(error);
    });
    watcher->setFuture(QtConcurrent::run(git::removeLinkedWorktree, repositoryPath, worktreePath,
                                         expectedIdentity));
}

void AgentWorktreeCreator::prune(const QString& repositoryPath) {
    if (active_) {
        emit failed(QStringLiteral("A worktree operation is already running."));
        return;
    }
    if (!QFileInfo(repositoryPath).isDir()) {
        emit failed(QStringLiteral("The repository folder is unavailable."));
        return;
    }
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty()) {
        emit failed(QStringLiteral("Git was not found on PATH."));
        return;
    }

    output_.clear();
    targetPath_.clear();
    timedOut_ = false;
    pruning_ = true;
    active_ = true;
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& key : environment.keys())
        if (key.startsWith(QStringLiteral("GIT_"), Qt::CaseInsensitive))
            environment.remove(key);
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    process_.setProcessEnvironment(environment);
    process_.setWorkingDirectory(repositoryPath);
    process_.setProgram(git);
    process_.setArguments({QStringLiteral("-c"), QStringLiteral("color.ui=false"),
                           QStringLiteral("worktree"), QStringLiteral("prune"),
                           QStringLiteral("--verbose")});
    process_.start();
    timeout_.start(creationTimeoutMs);
}

} // namespace ketplus
