#include "DaemonClient.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>
#include <functional>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("ketplusctl");
    QCommandLineParser parser;
    parser.setApplicationDescription("Manage a running KetPlus daemon without a Desktop UI.");
    parser.addHelpOption();
    parser.addOptions({{"socket", "Local socket path.", "path"},
                       {"token-file", "Owner-only local token file.", "path"},
                       {"timeout", "Deadline in seconds.", "seconds", "30"}});
    parser.addPositionalArgument(
        "command",
        "status | login | logout | doctor | workspace | pairing | device | sessions | call");
    parser.addPositionalArgument("arguments",
                                 "workspace add/list/remove; pairing create/status/revoke TICKET, "
                                 "pairing approve TICKET CLAIM_DIGEST; "
                                 "device revoke; call OPERATION [JSON|-]",
                                 "[arguments...]");
    parser.process(app);
    auto args = parser.positionalArguments();
    if (args.isEmpty())
        parser.showHelp(2);
    if (parser.isSet("socket"))
        qputenv("KETPLUS_DAEMON_SOCKET", parser.value("socket").toUtf8());
    if (parser.isSet("token-file"))
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", parser.value("token-file").toUtf8());
    qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
    const QString command = args.takeFirst();
    QString operation;
    QJsonObject payload;
    if (command == "status")
        operation = "remote.status";
    else if (command == "login")
        operation = "remote.sign_in.device";
    else if (command == "logout")
        operation = "remote.sign_out";
    else if (command == "doctor")
        operation = "provider.runtime_status";
    else if (command == "sessions")
        operation = "session.list";
    else if (command == "workspace" && !args.isEmpty()) {
        const auto action = args.takeFirst();
        if (action == "list")
            operation = "workspace.list";
        else if ((action == "add" || action == "remove") && args.size() == 1) {
            operation = action == "add" ? "workspace.register" : "workspace.unregister";
            payload.insert("path", args.takeFirst());
        }
    } else if ((command == "pairing" || command == "device") && !args.isEmpty()) {
        const auto action = args.takeFirst();
        if (command == "pairing" && action == "create")
            operation = "remote.pairing.create";
        else if (command == "pairing" && action == "approve" && args.size() == 2) {
            // Approval names the claim digest `pairing status` showed (ADR 0034).
            operation = "remote.pairing.approve";
            payload.insert("ticket_id", args.takeFirst());
            payload.insert("claim_digest", args.takeFirst());
        } else if ((action == "status" || action == "revoke") && args.size() == 1) {
            operation = "remote." + command + "." + action;
            payload.insert(command == "pairing" ? "ticket_id" : "device_id", args.takeFirst());
        }
    } else if (command == "call" && !args.isEmpty()) {
        operation = args.takeFirst();
        if (args.size() == 1) {
            QByteArray bytes;
            if (args.first() == "-") {
                QFile input;
                if (!input.open(stdin, QIODevice::ReadOnly))
                    return 2;
                bytes = input.read(1024 * 1024 + 1);
            } else
                bytes = args.first().toUtf8();
            args.clear();
            const auto document = QJsonDocument::fromJson(bytes);
            if (bytes.size() > 1024 * 1024 || !document.isObject()) {
                QTextStream(stderr) << "Payload must be a JSON object below 1 MiB.\n";
                return 2;
            }
            payload = document.object();
        }
    }
    bool validTimeout = false;
    const int seconds = parser.value("timeout").toInt(&validTimeout);
    if (operation.isEmpty() || !args.isEmpty() || !validTimeout || seconds < 1 || seconds > 3600) {
        QTextStream(stderr) << "Invalid command or deadline. Use --help.\n";
        return 2;
    }
    int result = 1;
    ketplus::remote::DaemonClient client;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
        QTextStream(stderr) << "Daemon operation timed out. Check the service and socket paths.\n";
        app.quit();
    });
    deadline.start(seconds * 1000);
    QTimer poll;
    poll.setInterval(1000);
    bool statusInFlight = false;
    const auto finish = [&](const QJsonObject& value, const QString& error) {
        if (!error.isEmpty())
            QTextStream(stderr) << error << '\n';
        else {
            QTextStream(stdout) << QJsonDocument(value).toJson(QJsonDocument::Indented);
            result = 0;
        }
        app.quit();
    };
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (statusInFlight)
            return;
        statusInFlight = true;
        client.call(
            "remote.status", {}, {}, 0, [&](const QJsonObject& value, const QString& error) {
                statusInFlight = false;
                if (!error.isEmpty()) {
                    finish(value, error);
                    return;
                }
                if (!value.value("sign_in_pending").toBool()) {
                    const auto failure = value.value("sign_in_error").toString();
                    finish(value,
                           failure.isEmpty() && value.value("machine_id").toString().isEmpty()
                               ? QStringLiteral("Sign-in did not enroll this machine.")
                               : failure);
                }
            });
    });
    client.call(operation, payload, {}, 0, [&](const QJsonObject& value, const QString& error) {
        if (command != "login" || !error.isEmpty()) {
            finish(value, error);
            return;
        }
        QTextStream(stdout) << QJsonDocument(value).toJson(QJsonDocument::Indented);
        deadline.start(qMin(3600, qMax(seconds, 610)) * 1000);
        poll.start();
    });
    app.exec();
    return result;
}
