#include "git/GitRevisionContent.h"
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>
namespace ketplus {
void GitRevisionContent::load(const QString& path, GitDiffMode mode) {
    const auto generation = ++generation_;
    if (process_) {
        process_->kill();
        process_->deleteLater();
        process_ = nullptr;
    }
    constexpr qint64 maximumBytes = 20 * 1024 * 1024;
    if (mode != GitDiffMode::Staged) {
        QTimer::singleShot(0, this, [this, path, generation] {
            if (generation != generation_)
                return;
            QFile file(path);
            if (!file.exists()) {
                emit ready({}, {});
                return;
            } // Deleted right-hand side.
            if (file.size() > maximumBytes) {
                emit ready({}, "This file is too large to preview.");
                return;
            }
            if (!file.open(QIODevice::ReadOnly)) {
                emit ready({}, file.errorString());
                return;
            }
            emit ready(QString::fromUtf8(file.readAll()), {});
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
                if (error == QProcess::FailedToStart && generation == generation_)
                    emit ready({}, "Unable to read the staged Markdown file.");
                if (error == QProcess::FailedToStart)
                    process->deleteLater();
            });
    connect(process, &QProcess::readyReadStandardOutput, this, [process] {
        if (process->bytesAvailable() > maximumBytes)
            process->kill();
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, timeout, generation](int exitCode, QProcess::ExitStatus status) {
                timeout->stop();
                if (generation == generation_) {
                    if (status == QProcess::NormalExit && exitCode == 0)
                        emit ready(QString::fromUtf8(process->readAllStandardOutput()), {});
                    else
                        emit ready(
                            {},
                            "Staged content is unavailable (the file may be deleted or unmerged).");
                }
                process->deleteLater();
            });
    // Git resolves :./path relative to -C, including linked worktrees.
    process->start(
        "git", {"-C", QFileInfo(path).absolutePath(), "show", ":./" + QFileInfo(path).fileName()});
    timeout->start(10000);
}
} // namespace ketplus
