#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

#include <functional>

// Everything the runner takes from the machine it runs on. Tests pass their own clock, sleep,
// random source, output and platform; the release binary uses systemEnvironment().
namespace ketplus::runner::app {

struct Platform {
    QString os;   // as the Relay names it: "linux"
    QString arch; // "x86_64" or "arm64"
};

struct Environment {
    std::function<QDateTime()> now;
    // Waits up to `milliseconds`, returning early once stopRequested() holds.
    std::function<void(qint64 milliseconds)> sleep;
    std::function<bool()> stopRequested;
    std::function<double()> random; // uniform in [0, 1)
    // One line each, without the newline. Nothing secret is ever passed to either.
    std::function<void(const QString&)> out;
    std::function<void(const QString&)> err;
    std::function<QByteArray()> readStdin;
    std::function<QString(const char* name)> variable; // empty when unset
    Platform platform;
    QString hostName;
    QUrl relayUrl;
    QString runnerVersion;
    int requestTimeoutMs = 30'000;
};

// The machine's clock, signals, terminal, platform and compiled-in Relay.
[[nodiscard]] Environment systemEnvironment();

} // namespace ketplus::runner::app
