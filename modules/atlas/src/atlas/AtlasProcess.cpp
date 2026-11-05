#include "atlas/AtlasProcess.h"
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>

namespace ketplus {
namespace {
constexpr int maxStartAttempts = 3;
QMutex claimedPortsMutex;
QSet<quint16> claimedPorts;

bool isAddressInUse(const QByteArray& output) {
    return output.contains("EADDRINUSE") || output.contains("Address already in use") ||
           output.contains("address already in use");
}
} // namespace

AtlasProcess::AtlasProcess(QObject* parent) : QObject(parent) {
    process_.setProcessChannelMode(QProcess::MergedChannels);
    timeout_.setSingleShot(true);
    timeout_.setInterval(20000);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        stop();
        emit failed(tr("KetAtlas did not become ready within 20 seconds."));
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        output_.append(process_.readAllStandardOutput());
        if (output_.size() > 32768)
            output_ = output_.right(32768);
        static const QRegularExpression address(
            QStringLiteral("KetAtlas: (http://127\\.0\\.0\\.1:[0-9]+)"));
        const auto match = address.match(QString::fromUtf8(output_));
        if (!ready_ && match.hasMatch()) {
            ready_ = true;
            timeout_.stop();
            emit ready(QUrl(match.captured(1)));
        }
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!stopping_ && error == QProcess::FailedToStart) {
            timeout_.stop();
            releasePort();
            emit failed(process_.errorString());
        }
    });
    connect(&process_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        timeout_.stop();
        if (stopping_)
            return;
        if (!ready_ && startAttempts_ < maxStartAttempts && isAddressInUse(output_)) {
            const quint64 retryGeneration = generation_;
            QTimer::singleShot(0, this, [this, retryGeneration] {
                if (!stopping_ && generation_ == retryGeneration)
                    launchAttempt();
            });
            return;
        }
        releasePort();
        emit failed(tr("KetAtlas stopped.\n%1").arg(QString::fromUtf8(output_).right(2000)));
    });
}
AtlasProcess::~AtlasProcess() { stop(); }

QString AtlasProcess::cliPath() {
    const auto override =
        qEnvironmentVariable("KETPLUS_KETATLAS_CLI", QSettings().value("atlas/cliPath").toString());
    if (!override.isEmpty())
        return override;
    auto paths =
        QProcessEnvironment::systemEnvironment().value("PATH").split(QDir::listSeparator());
    paths << "/opt/homebrew/bin" << "/usr/local/bin";
    const QDir nvm(QDir::homePath() + "/.nvm/versions/node");
    for (const auto& version :
         nvm.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed))
        paths.append(nvm.filePath(version + "/bin"));
    return QStandardPaths::findExecutable("ketatlas", paths);
}

void AtlasProcess::start(const AtlasDescriptor& atlas) {
    stop();
    atlas_ = atlas;
    startAttempts_ = 0;
    launchAttempt();
}

void AtlasProcess::launchAttempt() {
    const auto cli = cliPath();
    if (cli.isEmpty() || !QFileInfo::exists(cli)) {
        emit failed(tr("KetAtlas CLI was not found. Install ketatlas with Node.js 22+ or select "
                       "its executable."));
        return;
    }
    QString portError;
    if (!claimPort(portError)) {
        emit failed(portError);
        return;
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PATH", QFileInfo(cli).absolutePath() + QDir::listSeparator() +
                                   environment.value("PATH"));
    process_.setProcessEnvironment(environment);
    process_.setWorkingDirectory(atlas_.directory);
    output_.clear();
    ready_ = false;
    stopping_ = false;
    ++startAttempts_;
    process_.start(
        cli, {"serve", atlas_.path, "--root", atlas_.directory, "--port", QString::number(port_)});
    timeout_.start();
}

bool AtlasProcess::claimPort(QString& error) {
    releasePort();
    for (int attempt = 0; attempt < 8; ++attempt) {
        QTcpServer reservation;
        if (!reservation.listen(QHostAddress::LocalHost, 0)) {
            error = reservation.errorString();
            return false;
        }
        const quint16 candidate = reservation.serverPort();
        QMutexLocker locker(&claimedPortsMutex);
        if (claimedPorts.contains(candidate))
            continue;
        claimedPorts.insert(candidate);
        port_ = candidate;
        return true;
    }
    error = tr("Could not reserve a unique local port for KetAtlas.");
    return false;
}

void AtlasProcess::releasePort() {
    if (port_ == 0)
        return;
    const QMutexLocker locker(&claimedPortsMutex);
    claimedPorts.remove(port_);
    port_ = 0;
}

void AtlasProcess::stop() {
    ++generation_;
    stopping_ = true;
    timeout_.stop();
    if (process_.state() != QProcess::NotRunning) {
        process_.terminate();
        if (!process_.waitForFinished(1000)) {
            process_.kill();
            process_.waitForFinished(1000);
        }
    }
    releasePort();
    ready_ = false;
}
} // namespace ketplus
