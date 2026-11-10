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
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty())
        return;
    for (const QString& path : paths) {
        if (running_.contains(path) || !QFileInfo(path).isDir())
            continue;
        if (const auto last = probedAt_.constFind(path);
            last != probedAt_.constEnd() && clock_.elapsed() - *last < minimumIntervalMs)
            continue;
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
        connect(process, &QProcess::finished, this,
                [this, process, path](const int exitCode, const QProcess::ExitStatus status) {
                    running_.remove(path);
                    // A failed or killed status says nothing: the state stays unknown.
                    if (status == QProcess::NormalExit && exitCode == 0)
                        emit stateKnown(path, !process->readAllStandardOutput().isEmpty());
                    process->deleteLater();
                });
        connect(process, &QProcess::errorOccurred, this,
                [this, process, path](const QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart)
                        return; // `finished` follows a crash or a kill
                    running_.remove(path);
                    process->deleteLater();
                });
        process->start();
    }
}

} // namespace ketplus
