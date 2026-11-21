#include "Environment.h"

#include "KetPlusProductVersion.h"
#include "relay/RelayClient.h"

#include <QElapsedTimer>
#include <QFile>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QTextStream>
#include <QThread>

#include <atomic>
#include <csignal>
#include <cstdio>

namespace ketplus::runner::app {
namespace {

std::atomic_bool stopSignal{false};

extern "C" void requestStop(int) { stopSignal.store(true); }

void writeLine(FILE* stream, const QString& line) {
    const QByteArray bytes = line.toUtf8() + '\n';
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stream);
    std::fflush(stream);
}

Platform currentPlatform() {
    // Names as the Relay takes them; anything else is reported as found and refused.
    return {QSysInfo::kernelType(), QSysInfo::currentCpuArchitecture()};
}

} // namespace

Environment systemEnvironment() {
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    Environment environment;
    environment.now = [] { return QDateTime::currentDateTimeUtc(); };
    environment.stopRequested = [] { return stopSignal.load(); };
    environment.sleep = [](const qint64 milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (!stopSignal.load() && timer.elapsed() < milliseconds)
            QThread::msleep(
                static_cast<unsigned long>(std::min<qint64>(100, milliseconds - timer.elapsed())));
    };
    environment.random = [] { return QRandomGenerator::system()->generateDouble(); };
    environment.out = [](const QString& line) { writeLine(stdout, line); };
    environment.err = [](const QString& line) { writeLine(stderr, line); };
    environment.readStdin = [] {
        QFile input;
        return input.open(stdin, QIODevice::ReadOnly) ? input.read(64 * 1024) : QByteArray();
    };
    environment.variable = [](const char* name) { return qEnvironmentVariable(name); };
    environment.platform = currentPlatform();
    environment.hostName = QSysInfo::machineHostName();
    environment.relayUrl = compiledRelayUrl();
    environment.runnerVersion = QStringLiteral(KETPLUS_PRODUCT_VERSION);
    return environment;
}

} // namespace ketplus::runner::app
