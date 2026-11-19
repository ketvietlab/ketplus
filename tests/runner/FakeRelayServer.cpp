// SPDX-License-Identifier: MPL-2.0
// The test Relay as a program, for an installed runner built against a loopback Relay URL:
//   ketplus_runner_fake_relay_server --port N --token-file PATH
// writes one registration token to PATH (mode 0600) and serves until stopped. It prints one
// line per registration, heartbeat and deregistration with the runner id and the state it
// reported, never a token or credential. Test-only: not installed, not part of any package.
#include "FakeRelay.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QTextStream>

namespace {

void say(const QString& line) {
    QTextStream out(stdout);
    out << line << Qt::endl;
}

int usage() {
    QTextStream(stderr) << "Usage: ketplus_runner_fake_relay_server --port N --token-file PATH\n";
    return 64;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() != 5 || arguments.at(1) != QStringLiteral("--port") ||
        arguments.at(3) != QStringLiteral("--token-file"))
        return usage();
    bool number = false;
    const uint port = arguments.at(2).toUInt(&number);
    if (!number || port == 0 || port > 65535)
        return usage();

    runner_app_test::FakeRelay relay([] { return QDateTime::currentDateTimeUtc(); },
                                     static_cast<quint16>(port));
    if (relay.url().port() != static_cast<int>(port)) {
        QTextStream(stderr) << "fake_relay: cannot listen on 127.0.0.1:" << port << '\n';
        return 1;
    }
    QFile token(arguments.at(4));
    if (!token.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        !token.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        token.write(relay.createToken(QStringLiteral("desktop-1"))) < 0 || !token.flush()) {
        QTextStream(stderr) << "fake_relay: cannot write a new token file\n";
        return 1;
    }
    token.close();

    relay.rewrite = [&relay](const runner_app_test::FakeRelay::Request& request,
                             runner_app_test::FakeRelay::Reply& reply) {
        const QString path = request.path;
        if (path == QStringLiteral("/v2/runners:register"))
            say(QStringLiteral("register status=%1").arg(reply.status));
        else if (path == QStringLiteral("/v2/runner/heartbeat"))
            say(QStringLiteral("heartbeat runner=%1 state=%2 status=%3")
                    .arg(relay.lastRunnerId, request.body.value(QStringLiteral("state")).toString(),
                         QString::number(reply.status)));
        else if (path == QStringLiteral("/v2/runner:deregister"))
            say(QStringLiteral("deregister status=%1").arg(reply.status));
    };
    say(QStringLiteral("listening port=%1").arg(port));
    return app.exec();
}
