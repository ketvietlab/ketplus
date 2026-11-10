#include "app/WorktreeStateProbe.h"

#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

namespace ketplus {
namespace {

constexpr qint64 minimumIntervalMs = 3000;
constexpr int timeoutMs = 10000;

} // namespace

WorktreeStateProbe::WorktreeStateProbe(QObject* parent) : QObject(parent) { clock_.start(); }

void WorktreeStateProbe::probe(const QStringList& paths) {
    for (const QString& path : paths)
        request(path);
}

void WorktreeStateProbe::request(const QString& path) {
    // A request that cannot run now is owed a read afterwards: the worktree may have changed
    // since the result on screen, and nothing else would ask again.
    if (running_.contains(path)) {
        pending_.insert(path);
        return;
    }
    if (pending_.contains(path))
        return;
    const auto last = probedAt_.constFind(path);
    const qint64 wait =
        last == probedAt_.constEnd() ? 0 : minimumIntervalMs - (clock_.elapsed() - *last);
    if (wait <= 0) {
        start(path);
        return;
    }
    pending_.insert(path);
    QTimer::singleShot(wait, this, [this, path] {
        if (pending_.remove(path))
            start(path);
    });
}

void WorktreeStateProbe::start(const QString& path) {
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty() || !QFileInfo(path).isDir()) {
        emit stateUnknown(path);
        return;
    }
    running_.insert(path);
    probedAt_.insert(path, clock_.elapsed());

    auto* process = new QProcess(this);
    auto environment = QProcessEnvironment::systemEnvironment();
    // Never take the index lock: an Agent may be committing in this worktree.
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(path);
    process->setProgram(git);
    process->setArguments({QStringLiteral("status"), QStringLiteral("--porcelain"),
                           QStringLiteral("--untracked-files=normal")});
    process->setStandardErrorFile(QProcess::nullDevice());
    QTimer::singleShot(timeoutMs, process, &QProcess::kill);
    const auto finish = [this, process, path](const bool read, const bool dirty) {
        running_.remove(path);
        // A failed or killed status says nothing, so an earlier answer no longer stands.
        if (read)
            emit stateKnown(path, dirty);
        else
            emit stateUnknown(path);
        process->deleteLater();
        if (pending_.remove(path))
            request(path);
    };
    connect(process, &QProcess::finished, this,
            [process, finish](const int exitCode, const QProcess::ExitStatus status) {
                const bool read = status == QProcess::NormalExit && exitCode == 0;
                finish(read, read && !process->readAllStandardOutput().isEmpty());
            });
    connect(process, &QProcess::errorOccurred, this, [finish](const QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) // `finished` follows a crash or a kill
            finish(false, false);
    });
    process->start();
}

} // namespace ketplus
