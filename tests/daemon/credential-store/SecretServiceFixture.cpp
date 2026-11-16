#include "cloud/SecretServiceCredentialBackend.h"
#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QTextStream>
using namespace ketplus::daemon;
class Service final : public QDBusVirtualObject {
    QByteArray record_;
    bool exists_{false};
    QString mode_;
    int prompts_{0};
    int dismissed_{0};

  public:
    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& request, const QDBusConnection& connection) override {
        QVariantList out;
        const auto method = request.member();
        if (method == "Configure")
            mode_ = request.arguments().first().toString();
        else if (method == "Counts")
            out = {prompts_, dismissed_};
        else if (method == "Get")
            out = {QVariant::fromValue(QDBusVariant(mode_ == "locked"))};
        else if (method == "SearchItems") {
            QList<QDBusObjectPath> paths;
            if (exists_)
                paths.append(QDBusObjectPath("/org/freedesktop/secrets/item"));
            out = mode_ == "malformed" ? QVariantList{QStringLiteral("not-an-object-path-list")}
                                       : QVariantList{QVariant::fromValue(paths)};
        } else if (method == "OpenSession") {
            if (request.arguments().first() != "plain")
                return false;
            out = {QVariant::fromValue(QDBusVariant(QString{})),
                   QVariant::fromValue(QDBusObjectPath("/org/freedesktop/secrets/session"))};
        } else if (method == "GetSecret") {
            const SecretServiceValue secret{QDBusObjectPath("/org/freedesktop/secrets/session"),
                                            {},
                                            record_,
                                            "application/json"};
            out = {QVariant::fromValue(secret)};
        } else if (method == "CreateItem" || method == "SetSecret") {
            if (mode_ == "denied") {
                connection.send(request.createErrorReply("org.freedesktop.Secret.Error.IsLocked",
                                                         "Synthetic denial"));
                return true;
            }
            if (mode_ == "prompt" && method == "CreateItem") {
                out = {QVariant::fromValue(QDBusObjectPath("/")),
                       QVariant::fromValue(QDBusObjectPath("/org/freedesktop/secrets/prompt"))};
            } else {
                const auto secret = qdbus_cast<SecretServiceValue>(
                    request.arguments().at(method == "CreateItem" ? 1 : 0));
                record_ = secret.value;
                exists_ = true;
                if (method == "CreateItem")
                    out = {QVariant::fromValue(QDBusObjectPath("/org/freedesktop/secrets/item")),
                           QVariant::fromValue(QDBusObjectPath("/"))};
            }
        } else if (method == "Prompt")
            ++prompts_;
        else if (method == "Dismiss")
            ++dismissed_;
        else if (method != "Close")
            return false;
        connection.send(request.createReply(out));
        return true;
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qDBusRegisterMetaType<SecretServiceValue>();
    qDBusRegisterMetaType<QList<QDBusObjectPath>>();
    qDBusRegisterMetaType<QMap<QString, QString>>();
    Service service;
    auto connection = QDBusConnection::sessionBus();
    if (!connection.registerService("org.freedesktop.secrets") ||
        !connection.registerVirtualObject("/org/freedesktop/secrets", &service,
                                          QDBusConnection::SubPath))
        return 2;
    QTextStream(stdout) << "ready\n" << Qt::flush;
    return app.exec();
}
