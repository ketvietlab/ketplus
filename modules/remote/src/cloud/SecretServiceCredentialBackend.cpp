#include "cloud/SecretServiceCredentialBackend.h"
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusVariant>

namespace ketplus::daemon {
QDBusArgument& operator<<(QDBusArgument& arg, const SecretServiceValue& value) {
    arg.beginStructure();
    arg << value.session << value.parameters << value.value << value.contentType;
    arg.endStructure();
    return arg;
}
const QDBusArgument& operator>>(const QDBusArgument& arg, SecretServiceValue& value) {
    arg.beginStructure();
    arg >> value.session >> value.parameters >> value.value >> value.contentType;
    arg.endStructure();
    return arg;
}
namespace {
const QString busName = QStringLiteral("org.freedesktop.secrets");
const QString rootPath = QStringLiteral("/org/freedesktop/secrets");
const QString collectionPath = rootPath + QStringLiteral("/aliases/default");
const QString serviceInterface = QStringLiteral("org.freedesktop.Secret.Service");
const QString collectionInterface = QStringLiteral("org.freedesktop.Secret.Collection");
const QString itemInterface = QStringLiteral("org.freedesktop.Secret.Item");
class SecretService final : public CredentialBackend {
    QDBusConnection connection_;
    QDBusMessage call(const QString& path, const QString& interface, const QString& method,
                      const QVariantList& args = {}) const {
        auto message = QDBusMessage::createMethodCall(busName, path, interface, method);
        message.setArguments(args);
        // A missing/locked headless store is an error, never an interactive unlock request.
        return connection_.call(message, QDBus::Block, 3000);
    }
    bool valid(const QDBusMessage& reply, const QString& signature, QString* error) const {
        if (reply.type() == QDBusMessage::ReplyMessage && reply.signature() == signature)
            return true;
        if (error)
            *error = QStringLiteral(
                "Secret Service is unavailable or access was denied. Configure an unlocked default "
                "collection for the daemon's OS user and retry; no plaintext fallback is used.");
        return false;
    }
    bool unlocked(QString* error) const {
        const auto locked = call(collectionPath, "org.freedesktop.DBus.Properties", "Get",
                                 {collectionInterface, "Locked"});
        if (!valid(locked, "v", error))
            return false;
        const auto property = qvariant_cast<QDBusVariant>(locked.arguments().first()).variant();
        if (property.metaType().id() != QMetaType::Bool || property.toBool()) {
            if (error)
                *error = QStringLiteral("The default Secret Service collection is locked; unlock "
                                        "it outside KetPlus and retry.");
            return false;
        }
        return true;
    }
    bool find(const QString& service, QString* item, QString* error) const {
        if (!unlocked(error))
            return false;
        const auto reply = call(collectionPath, collectionInterface, "SearchItems",
                                {QVariant::fromValue(attributes(service))});
        if (!valid(reply, "ao", error))
            return false;
        const auto items = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments().first());
        if (items.size() > 1) {
            if (error)
                *error = QStringLiteral(
                    "Conflicting OS credential records require recovery before continuing.");
            return false;
        }
        *item = items.isEmpty() ? QString{} : items.first().path();
        return true;
    }
    static QMap<QString, QString> attributes(const QString& service) {
        return {{"application", "dev.ketplus.control"}, {"service", service}, {"format", "1"}};
    }
    QString openSession(QString* error) const {
        const auto reply = call(rootPath, serviceInterface, "OpenSession",
                                {"plain", QVariant::fromValue(QDBusVariant(QString{}))});
        if (!valid(reply, "vo", error))
            return {};
        const auto path = qvariant_cast<QDBusObjectPath>(reply.arguments().at(1)).path();
        if (path.isEmpty() || path == "/") {
            if (error)
                *error = QStringLiteral("Secret Service did not establish a session.");
            return {};
        }
        return path;
    }
    void closeSession(const QString& session) const {
        auto message = QDBusMessage::createMethodCall(busName, session,
                                                      "org.freedesktop.Secret.Session", "Close");
        connection_.call(message, QDBus::NoBlock);
    }

  public:
    explicit SecretService(const QDBusConnection& connection) : connection_(connection) {
        qDBusRegisterMetaType<SecretServiceValue>();
        qDBusRegisterMetaType<QMap<QString, QString>>();
        qDBusRegisterMetaType<QList<QDBusObjectPath>>();
    }
    bool accessible(const QString&, QString* error) override { return unlocked(error); }
    Result read(const QString& service, QByteArray* value, QString* error) override {
        value->clear();
        QString item;
        if (!find(service, &item, error))
            return Result::Error;
        if (item.isEmpty())
            return Result::Missing;
        const auto session = openSession(error);
        if (session.isEmpty())
            return Result::Error;
        const auto reply =
            call(item, itemInterface, "GetSecret", {QVariant::fromValue(QDBusObjectPath(session))});
        closeSession(session);
        if (!valid(reply, "(oayays)", error))
            return Result::Error;
        const auto secret = qdbus_cast<SecretServiceValue>(reply.arguments().first());
        if (secret.session.path() != session || !secret.parameters.isEmpty() ||
            secret.contentType != "application/json") {
            if (error)
                *error = QStringLiteral("The OS credential record has an invalid secret envelope.");
            return Result::Error;
        }
        *value = secret.value;
        return Result::Found;
    }
    bool write(const QString& service, const QByteArray& value, QString* error) override {
        QString item;
        if (!find(service, &item, error))
            return false;
        const auto session = openSession(error);
        if (session.isEmpty())
            return false;
        const SecretServiceValue secret{QDBusObjectPath(session), {}, value, "application/json"};
        if (!item.isEmpty()) {
            const auto reply =
                call(item, itemInterface, "SetSecret", {QVariant::fromValue(secret)});
            closeSession(session);
            return valid(reply, "", error);
        }
        const QVariantMap properties{
            {itemInterface + ".Label", "KetPlus credentials"},
            {itemInterface + ".Attributes", QVariant::fromValue(attributes(service))}};
        const auto reply = call(collectionPath, collectionInterface, "CreateItem",
                                {properties, QVariant::fromValue(secret), true});
        closeSession(session);
        if (!valid(reply, "oo", error))
            return false;
        const auto created = qvariant_cast<QDBusObjectPath>(reply.arguments().at(0)).path();
        const auto prompt = qvariant_cast<QDBusObjectPath>(reply.arguments().at(1)).path();
        if (prompt != "/" || created.isEmpty() || created == "/") {
            if (!prompt.isEmpty() && prompt != "/")
                call(prompt, "org.freedesktop.Secret.Prompt", "Dismiss");
            if (error)
                *error = QStringLiteral("Secret Service requires interactive authorization; "
                                        "authorize the collection outside KetPlus and retry.");
            return false;
        }
        return true;
    }
};
} // namespace
std::shared_ptr<CredentialBackend>
secretServiceCredentialBackend(const QDBusConnection& connection) {
    return std::make_shared<SecretService>(connection);
}
#if defined(Q_OS_LINUX)
std::shared_ptr<CredentialBackend> platformCredentialBackend() {
    // The plain Secret Service session is confined to a local Unix bus. Never send
    // credentials through a TCP or fallback address supplied by the environment.
    const auto address = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    if (!address.isEmpty() && (!address.startsWith("unix:") || address.contains(';')))
        return secretServiceCredentialBackend(QDBusConnection("ketplus-nonlocal-bus-refused"));
    return secretServiceCredentialBackend(QDBusConnection::sessionBus());
}
#endif
} // namespace ketplus::daemon
