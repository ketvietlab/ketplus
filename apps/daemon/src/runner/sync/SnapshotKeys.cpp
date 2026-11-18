#include "runner/sync/SnapshotKeys.h"

#include "cloud/CredentialStore.h"
#include "cloud/CryptoPrimitives.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>

#include <openssl/rand.h>

namespace ketplus::daemon::runner::sync {

namespace {

constexpr int recordVersion = 1;

enum class Record { Missing, Valid, Malformed };

Record parse(const QByteArray& bytes, QByteArray* secret) {
    if (bytes.isEmpty())
        return Record::Missing;
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
        return Record::Malformed;
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("version")).toInt(-1) != recordVersion)
        return Record::Malformed;
    const QJsonValue value = object.value(QStringLiteral("secret"));
    if (!value.isString())
        return Record::Malformed;
    const QByteArray decoded = control_crypto::fromBase64Url(value.toString());
    if (decoded.size() != runnerSecretBytes ||
        control_crypto::base64Url(decoded) != value.toString().toLatin1())
        return Record::Malformed;
    *secret = decoded;
    return Record::Valid;
}

QByteArray encode(const QByteArray& secret) {
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), recordVersion},
                                     {QStringLiteral("secret"),
                                      QString::fromLatin1(control_crypto::base64Url(secret))}})
        .toJson(QJsonDocument::Compact);
}

QString derive(const QByteArray& secret, const QJsonArray& values) {
    if (secret.size() != runnerSecretBytes)
        return {};
    const QByteArray mac = QMessageAuthenticationCode::hash(control_crypto::canonical(values),
                                                            secret, QCryptographicHash::Sha256);
    return QString::fromLatin1(control_crypto::base64Url(mac).left(workspaceKeyChars));
}

} // namespace

std::optional<QByteArray> RunnerSecret::obtain(QString* error) const {
    const QString account = QString::fromLatin1(runnerSecretAccount);
    QByteArray secret;
    bool generated = true;
    QString storeError;
    const auto failed = [&](const QString& message) -> std::optional<QByteArray> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const QByteArray current = store_.read(account, &storeError);
    if (!storeError.isEmpty())
        return failed(QStringLiteral("the runner secret could not be read"));
    Record record = parse(current, &secret);
    if (record == Record::Valid)
        return secret;
    if (record == Record::Malformed)
        return failed(QStringLiteral("the stored runner secret is malformed"));
    const bool stored = store_.update(
        [&](QHash<QString, QByteArray>& accounts) {
            record = parse(accounts.value(account), &secret);
            if (record != Record::Missing)
                return;
            secret.resize(runnerSecretBytes);
            generated =
                RAND_bytes(reinterpret_cast<unsigned char*>(secret.data()), runnerSecretBytes) == 1;
            if (generated)
                accounts.insert(account, encode(secret));
        },
        &storeError);
    if (!stored)
        return failed(QStringLiteral("the runner secret could not be read or stored"));
    if (record == Record::Malformed)
        return failed(QStringLiteral("the stored runner secret is malformed"));
    if (!generated)
        return failed(QStringLiteral("no random bytes for the runner secret"));
    return secret;
}

QString repositoryKey(const QByteArray& secret, const QString& runnerId,
                      const QString& repositoryId) {
    return derive(secret, QJsonArray{runnerId, repositoryId});
}

QString workspaceKey(const QByteArray& secret, const QString& runnerId, const QString& repositoryId,
                     const QString& worktreeId) {
    return derive(secret, QJsonArray{runnerId, repositoryId, worktreeId});
}

} // namespace ketplus::daemon::runner::sync
