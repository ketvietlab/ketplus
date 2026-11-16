#include "atlas/ManagedAtlasViewers.h"

#include "KetDialog.h"
#include "atlas/AtlasProcess.h"
#include "atlas/AtlasRendererPermission.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTimer>
#include <memory>

namespace ketplus {

ManagedAtlasViewers::ManagedAtlasViewers(QObject* parent) : QObject(parent) {}

ManagedAtlasViewers* ManagedAtlasViewers::instance() {
    static auto* registry = new ManagedAtlasViewers(QCoreApplication::instance());
    return registry;
}

QList<ManagedAtlasViewers::Viewer> ManagedAtlasViewers::viewers() const {
    QList<Viewer> result;
    for (const auto& entry : processes_)
        result.append(entry.viewer);
    return result;
}

int ManagedAtlasViewers::runningCount() const {
    int count = 0;
    for (const auto& entry : processes_)
        count += entry.viewer.state == State::Running;
    return count;
}

QString ManagedAtlasViewers::errorFor(const QString& path) const { return errors_.value(path); }

void ManagedAtlasViewers::open(const AtlasDescriptor& atlas, QWidget* anchor) {
    if (auto pending = permissions_.value(atlas.path)) {
        pending->raise();
        pending->setFocus();
        return;
    }
    permissions_.remove(atlas.path);
    if (auto existing = processes_.find(atlas.path); existing != processes_.end()) {
        if (existing->viewer.state == State::Running)
            QDesktopServices::openUrl(existing->viewer.url);
        return;
    }

    errors_.remove(atlas.path);
    const QString cli = AtlasProcess::cliPath();
    if (cli.isEmpty() || !QFileInfo::exists(cli)) {
        errors_.insert(atlas.path,
                       tr("KetAtlas 0.6.1 or newer is required. Select or install the CLI first."));
        emit changed();
        return;
    }
    auto portProbe = std::make_shared<QTcpServer>();
    auto rendererProbe = std::make_shared<QTcpServer>();
    if (!portProbe->listen(QHostAddress::LocalHost, 0)) {
        errors_.insert(atlas.path, tr("Could not reserve a local viewer port."));
        emit changed();
        return;
    }
    const bool rendererPortAvailable = rendererProbe->listen(QHostAddress::LocalHost, 0);
    const auto port = portProbe->serverPort();
    const auto rendererPort = rendererProbe->serverPort();
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PATH"), QFileInfo(cli).absolutePath() +
                                                   QDir::listSeparator() +
                                                   environment.value(QStringLiteral("PATH")));
    const auto permission = AtlasRendererPermission::read(atlas, environment, rendererPort);
    if (!permission.present || !permission.error.isEmpty() || !anchor || !rendererPortAvailable) {
        if (!permission.error.isEmpty())
            errors_.insert(atlas.path, permission.error);
        else if (permission.present && !rendererPortAvailable)
            errors_.insert(atlas.path,
                           tr("Could not reserve a renderer port. Opening static content."));
        portProbe->close();
        rendererProbe->close();
        launch(atlas, cli, environment, port, rendererPort, nullptr);
        return;
    }
    auto* dialog = rendererPermissionDialog(anchor, permission);
    permissions_.insert(atlas.path, dialog);
    connect(dialog, &ket::Dialog::finished, this,
            [this, atlas, cli, environment, port, rendererPort, permission, dialog, portProbe,
             rendererProbe](bool accepted) {
                if (permissions_.value(atlas.path) != dialog)
                    return; // Stop/revocation removes the pending request before rejecting it.
                permissions_.remove(atlas.path);
                portProbe->close();
                rendererProbe->close();
                launch(atlas, cli, environment, port, rendererPort,
                       accepted ? &permission : nullptr);
                dialog->deleteLater();
            });
    connect(dialog, &QObject::destroyed, this, [this, path = atlas.path] {
        if (permissions_.contains(path) && permissions_.value(path).isNull())
            permissions_.remove(path);
    });
}

void ManagedAtlasViewers::launch(const AtlasDescriptor& atlas, const QString& cli,
                                 const QProcessEnvironment& environment, quint16 port,
                                 quint16 rendererPort, const AtlasRendererPermission* permission) {
    const auto current = AtlasIndex::readManifest(atlas.path, atlas.workspaceRoot);
    if (current.path.isEmpty() || current.path != atlas.path ||
        current.directory != atlas.directory) {
        errors_.insert(atlas.path, tr("The Atlas path changed. Open it again from its worktree."));
        emit changed();
        return;
    }
    auto* process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(atlas.directory);
    process->setProcessEnvironment(environment);
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    timeout->setInterval(20000);
    processes_.insert(atlas.path,
                      ProcessEntry{Viewer{atlas, State::Starting, {}}, process, timeout, {}, {}});
    connect(timeout, &QTimer::timeout, this, [this, path = atlas.path, process] {
        finish(path, process, tr("KetAtlas did not become ready within 20 seconds."));
        process->kill();
    });
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, path = atlas.path] { consumeOutput(path); });
    connect(process, &QProcess::errorOccurred, this,
            [this, path = atlas.path, process](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    finish(path, process, process->errorString());
            });
    connect(process, &QProcess::finished, this,
            [this, path = atlas.path, process](int exitCode, QProcess::ExitStatus) {
                if (!processes_.contains(path))
                    return;
                const auto entry = processes_.value(path);
                const auto output =
                    QString::fromUtf8(entry.diagnosticTail + entry.output).right(500);
                const auto error = entry.viewer.state == State::Starting
                                       ? output.contains(QStringLiteral("--managed"))
                                             ? tr("KetAtlas 0.6.1 or newer is required. Open Edit "
                                                  "to install or select it.")
                                             : tr("KetAtlas stopped before opening. %1").arg(output)
                                   : exitCode == 0 ? QString{}
                                                   : tr("KetAtlas viewer exited unexpectedly.");
                finish(path, process, error);
            });
    QStringList arguments{QStringLiteral("serve"),     atlas.path,
                          QStringLiteral("--managed"), QStringLiteral("--port"),
                          QString::number(port),       QStringLiteral("--no-preload")};
    // No event-loop boundary may intervene between this recheck and starting the CLI.
    if (permission) {
        const auto latest = AtlasRendererPermission::read(atlas, environment, rendererPort);
        if (latest == *permission && latest.error.isEmpty()) {
            arguments.append({QStringLiteral("--renderer"), QStringLiteral("--html-port"),
                              QString::number(rendererPort)});
        } else {
            errors_.insert(atlas.path, tr("The renderer contract or path changed. Opening static "
                                          "content; open again to review a new renderer launch."));
        }
    }
    process->start(cli, arguments);
    timeout->start();
    emit changed();
}

void ManagedAtlasViewers::consumeOutput(const QString& path) {
    auto it = processes_.find(path);
    if (it == processes_.end())
        return;
    it->output.append(it->process->readAllStandardOutput());
    if (it->output.size() > 32768)
        it->output = it->output.right(32768);
    while (true) {
        const auto end = it->output.indexOf('\n');
        if (end < 0)
            break;
        const QByteArray line = it->output.left(end);
        it->output.remove(0, end + 1);
        it->diagnosticTail.append(line + '\n');
        if (it->diagnosticTail.size() > 2048)
            it->diagnosticTail = it->diagnosticTail.right(2048);
        const auto message = QJsonDocument::fromJson(line).object();
        if (message.value(QStringLiteral("type")) == QStringLiteral("ready")) {
            const QUrl url(message.value(QStringLiteral("url")).toString());
            if (url.scheme() != QStringLiteral("http") ||
                url.host() != QStringLiteral("127.0.0.1")) {
                auto* process = it->process;
                finish(path, process, tr("KetAtlas returned an invalid local viewer URL."));
                process->kill();
                return;
            }
            it->timeout->stop();
            it->viewer.state = State::Running;
            it->viewer.url = url;
            if (!QDesktopServices::openUrl(url)) {
                auto* process = it->process;
                finish(path, process, tr("Could not open the viewer in your browser."));
                process->kill();
                return;
            }
            emit changed();
            return; // A listener may stop this process and invalidate the iterator.
        }
    }
}

void ManagedAtlasViewers::stop(const QString& path) {
    if (auto dialog = permissions_.take(path)) {
        dialog->reject();
        dialog->deleteLater();
    }
    auto it = processes_.find(path);
    if (it == processes_.end() || it->viewer.state == State::Stopping)
        return;
    it->viewer.state = State::Stopping;
    it->process->write("{\"type\":\"stop\"}\n");
    it->process->waitForBytesWritten(100);
    auto* process = it->process;
    QTimer::singleShot(3000, process, [process] {
        if (process->state() != QProcess::NotRunning)
            process->kill();
    });
    emit changed();
}

void ManagedAtlasViewers::finish(const QString& path, QProcess* process, const QString& error) {
    auto it = processes_.find(path);
    if (it == processes_.end() || it->process != process)
        return;
    it->timeout->stop();
    processes_.erase(it);
    if (!error.isEmpty())
        errors_.insert(path, error);
    process->deleteLater();
    emit changed();
}

} // namespace ketplus
