#include "git/GitRevisionContent.h"

#include "git/GitProcessEnvironment.h"

#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProcess>
#include <QPromise>
#include <QThreadPool>
#include <QTimer>

#include <utility>

namespace ketplus {
namespace {

constexpr qint64 maximumBytes = 20 * 1024 * 1024;

struct RevisionResult {
    QString content;
    QString error;
};

RevisionResult readWorkingRevision(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists()) {
        return {}; // Deleted right-hand side.
    }
    if (!info.isFile()) {
        return {{}, QObject::tr("The working copy is not a regular file.")};
    }
    if (info.size() > maximumBytes) {
        return {{}, QObject::tr("This file is too large to preview.")};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {{}, file.errorString()};
    }
    const QByteArray bytes = file.read(maximumBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        return {{}, file.errorString()};
    }
    if (bytes.size() > maximumBytes) {
        return {{}, QObject::tr("This file is too large to preview.")};
    }
    return {QString::fromUtf8(bytes), {}};
}

} // namespace

void GitRevisionContent::load(const QString& path, GitDiffMode mode) {
    const auto generation = ++generation_;
    if (process_) {
        process_->kill();
        process_->deleteLater();
        process_ = nullptr;
    }
    if (mode != GitDiffMode::Staged) {
        QPromise<RevisionResult> promise;
        auto* watcher = new QFutureWatcher<RevisionResult>(this);
        connect(watcher, &QFutureWatcher<RevisionResult>::finished, this,
                [this, watcher, generation] {
                    watcher->deleteLater();
                    if (generation == generation_) {
                        const auto result = watcher->result();
                        emit ready(result.content, result.error);
                    }
                });
        promise.start();
        watcher->setFuture(promise.future());
        QThreadPool::globalInstance()->start([path, promise = std::move(promise)]() mutable {
            promise.addResult(readWorkingRevision(path));
            promise.finish();
        });
        return;
    }

    auto* process = new QProcess(this);
    process_ = process;
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, [process] { process->kill(); });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, generation](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart) {
                    return;
                }
                process->deleteLater();
                if (generation == generation_) {
                    process_ = nullptr;
                    emit ready({}, process->errorString());
                }
            });
    connect(process, &QProcess::readyReadStandardOutput, this, [process] {
        if (process->bytesAvailable() > maximumBytes) {
            process->kill();
        }
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, timeout, generation](int exitCode, QProcess::ExitStatus status) {
                const bool timedOut = !timeout->isActive();
                timeout->stop();
                process->deleteLater();
                if (generation != generation_) {
                    return;
                }
                process_ = nullptr;
                if (timedOut) {
                    emit ready({}, tr("Git timed out reading the staged file."));
                } else if (process->bytesAvailable() > maximumBytes) {
                    emit ready({}, tr("This file is too large to preview."));
                } else if (status == QProcess::NormalExit && exitCode == 0) {
                    emit ready(QString::fromUtf8(process->readAllStandardOutput()), {});
                } else {
                    const auto error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                    emit ready({}, error.isEmpty() ? tr("Unable to read the staged file.") : error);
                }
            });
    // Git resolves :./path relative to the working directory, including linked worktrees.
    const QFileInfo file(path);
    process->setWorkingDirectory(file.absolutePath());
    process->setProcessEnvironment(gitProcessEnvironment());
    timeout->start(10000);
    process->start(QStringLiteral("git"),
                   {QStringLiteral("show"), QStringLiteral(":./") + file.fileName()});
}

} // namespace ketplus
