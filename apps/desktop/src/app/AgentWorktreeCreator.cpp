#include "app/AgentWorktreeCreator.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

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
    if (baseBranch.trimmed().isEmpty() || baseBranch.startsWith('-')) {
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

    targetPath_ = worktreePath(primaryWorktreePath, branchName_);
    if (QFileInfo::exists(targetPath_)) {
        emit failed(QStringLiteral("The inferred worktree path already exists:\n%1")
                        .arg(QDir::toNativeSeparators(targetPath_)));
        return;
    }

    output_.clear();
    removing_ = false;
    pruning_ = false;
    timedOut_ = false;
    active_ = true;

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    process_.setProcessEnvironment(environment);
    process_.setWorkingDirectory(repositoryPath);
    process_.setProgram(git);
    process_.setArguments({QStringLiteral("-c"), QStringLiteral("color.ui=false"),
                           QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"),
                           branchName_, QStringLiteral("--"), targetPath_, baseBranch});
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
        else if (removing_)
            emit removed(targetPath_);
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
                                  const bool force) {
    if (active_) {
        emit failed(QStringLiteral("A worktree operation is already running."));
        return;
    }
    const auto target = QFileInfo(worktreePath).canonicalFilePath();
    if (target.isEmpty() || target == QFileInfo(repositoryPath).canonicalFilePath() ||
        !QFileInfo(QDir(target).filePath(".git")).isFile()) {
        emit failed(QStringLiteral("Only an inactive linked worktree can be removed."));
        return;
    }
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty()) {
        emit failed(QStringLiteral("Git was not found on PATH."));
        return;
    }
    output_.clear();
    targetPath_ = target;
    timedOut_ = false;
    removing_ = true;
    pruning_ = false;
    active_ = true;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    process_.setProcessEnvironment(environment);
    process_.setWorkingDirectory(repositoryPath);
    process_.setProgram(git);
    QStringList arguments{QStringLiteral("-c"), QStringLiteral("color.ui=false"),
                          QStringLiteral("worktree"), QStringLiteral("remove")};
    if (force)
        arguments.append(QStringLiteral("--force"));
    arguments << QStringLiteral("--") << target;
    process_.setArguments(arguments);
    process_.start();
    timeout_.start(creationTimeoutMs);
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
    removing_ = false;
    pruning_ = true;
    active_ = true;
    auto environment = QProcessEnvironment::systemEnvironment();
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
