#include "agents/GitProcess.h"

#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

#include <memory>

namespace ketplus::daemon {
namespace {
constexpr int gitTimeoutMs = 10'000;
} // namespace

void runGit(QObject* owner, const QString& directory, const QStringList& arguments,
            std::function<void(std::optional<QString>)> done, const QString& indexFile) {
    auto* process = new QProcess(owner);
    process->setWorkingDirectory(directory);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    if (!indexFile.isEmpty())
        environment.insert(QStringLiteral("GIT_INDEX_FILE"), indexFile);
    process->setProcessEnvironment(environment);
    auto finish = std::make_shared<std::function<void(std::optional<QString>)>>(std::move(done));
    QObject::connect(process, &QProcess::finished, owner,
                     [process, finish](const int code, const QProcess::ExitStatus status) {
                         const bool ok = status == QProcess::NormalExit && code == 0;
                         (*finish)(ok ? std::optional<QString>(
                                            QString::fromUtf8(process->readAllStandardOutput()))
                                      : std::nullopt);
                         process->deleteLater();
                     });
    QObject::connect(process, &QProcess::errorOccurred, owner,
                     [process, finish](const QProcess::ProcessError error) {
                         if (error != QProcess::FailedToStart)
                             return;
                         (*finish)(std::nullopt);
                         process->deleteLater();
                     });
    QTimer::singleShot(gitTimeoutMs, process, [process] { process->kill(); });
    process->start(QStringLiteral("git"),
                   QStringList{QStringLiteral("--no-optional-locks")} + arguments);
}

} // namespace ketplus::daemon
