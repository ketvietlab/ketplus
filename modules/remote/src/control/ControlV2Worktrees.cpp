#include "control/ControlV2Client.h"
#include "control/ControlV2Content.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>

// Commands scoped to a catalog worktree before a conversation exists there: sealed to the
// machine's pinned agreement key, answered with a result sealed to this device and signed by the
// pinned host key (CloudControlClient::executeWorktreeCommand is the host side).
namespace ketplus::remote {
using control_v2::segment;
namespace {
constexpr qsizetype maximumPlaintext = 64 * 1024;
constexpr qsizetype maximumResult = 512 * 1024;
QString keyIdFor(const QString& commandId) { return "session-create:" + commandId; }
// The fields the relay keeps for a worktree-scoped command and echoes in its command DTO.
constexpr const char* scopeFields[] = {"id",
                                       "workspaceId",
                                       "machineId",
                                       "idempotencyKey",
                                       "kind",
                                       "ownerSubjectId",
                                       "repositoryId",
                                       "worktreeId",
                                       "expectedCatalogRevision",
                                       "expectedLifecycleRevision"};
} // namespace

void ControlV2Client::prepareWorktreeCommand(const QJsonObject& intent,
                                             const QJsonObject& plaintext, Callback done) {
    const auto kind = intent.value("kind").toString();
    const auto machine = intent.value("machineId").toString();
    const int version = pinnedVersion(machine);
    if (!control_v2::worktreeScoped(kind) || machine.isEmpty() ||
        intent.value("workspaceId").toString().isEmpty() ||
        intent.value("repositoryId").toString().isEmpty() ||
        intent.value("worktreeId").toString().isEmpty() || subjectId().isEmpty() ||
        deviceId_.isEmpty() ||
        QJsonDocument(plaintext).toJson(QJsonDocument::Compact).size() > maximumPlaintext) {
        done({}, "Unsupported or oversized worktree command.");
        return;
    }
    if (!hostPinsStored_ || version < 1) {
        done({}, QString::fromLatin1(control_v2::hostNotPaired));
        return;
    }
    const auto epoch = epoch_;
    hostKeys(
        machine, version,
        [this, epoch, intent, plaintext, kind, machine, version,
         done = std::move(done)](const QJsonObject& keys, const QString& error) {
            const auto agreement =
                daemon::CloudCrypto::fromBase64Url(keys.value("agreementPublicKey").toString());
            if (!error.isEmpty() || epoch != epoch_ || agreement.size() != 32) {
                done({}, !error.isEmpty()  ? error
                         : epoch != epoch_ ? QStringLiteral("Account changed.")
                                           : QStringLiteral("Invalid host agreement key."));
                return;
            }
            const auto token = QUuid::createUuid().toString(QUuid::Id128);
            auto command = intent;
            if (command.value("id").toString().size() < 16)
                command.insert("id", "create_" + token);
            if (command.value("idempotencyKey").toString().size() < 16)
                command.insert("idempotencyKey", "create_idempotency_" + token);
            command.insert("ownerSubjectId", subjectId());
            command.insert("sessionId", "");
            command.insert("sessionGeneration", 0);
            command.insert("capability", "session.create");
            for (const auto* key : {"expectedCatalogRevision", "expectedLifecycleRevision"})
                command.insert(QLatin1String(key), intent.value(QLatin1String(key)).toInteger(0));
            const auto id = command.value("id").toString();
            QString failure;
            const auto content = crypto_.encryptSealedBound(
                QJsonDocument(plaintext).toJson(QJsonDocument::Compact),
                [&command](const QJsonObject& envelope) {
                    auto bound = command;
                    bound.insert("content", envelope);
                    return commandAssociatedData(bound);
                },
                agreement, id, keyIdFor(id), machine, version, deviceId_, keyVersion_, &failure);
            if (content.isEmpty()) {
                done({}, failure.isEmpty() ? "Unable to seal the worktree command." : failure);
                return;
            }
            command.insert("content", content);
            done(command, {});
        });
}

void ControlV2Client::submitWorktreeCommand(const QJsonObject& envelope, Callback done) {
    const auto content = envelope.value("content").toObject();
    if (deviceId_.isEmpty() || content.value("senderDeviceId").toString() != deviceId_ ||
        content.value("senderKeyVersion").toInt() != keyVersion_ ||
        content.value("keyId").toString() != keyIdFor(envelope.value("id").toString()) ||
        !control_v2::worktreeScoped(envelope.value("kind").toString())) {
        done({}, "Prepared command identity changed.");
        return;
    }
    QJsonObject body;
    for (const auto* key : scopeFields)
        body.insert(QLatin1String(key), envelope.value(QLatin1String(key)));
    body.insert("content", content);
    authorized("POST",
               "/v2/mobile/workspaces/" + segment(envelope.value("workspaceId").toString()) +
                   "/session-creates",
               body, std::move(done));
}

void ControlV2Client::worktreeCommand(const QJsonObject& envelope, Callback done) {
    const auto machine = envelope.value("machineId").toString();
    const auto id = envelope.value("id").toString();
    authorized("GET", "/v2/mobile/commands/" + segment(id), {},
               [this, envelope, machine, id, done = std::move(done)](const QJsonValue& value,
                                                                     const QString& failure) {
                   if (!failure.isEmpty()) {
                       done({}, failure);
                       return;
                   }
                   auto result = value.toObject();
                   for (const auto* key : scopeFields)
                       if (result.value(QLatin1String(key)).toVariant() !=
                           envelope.value(QLatin1String(key)).toVariant()) {
                           done({}, "Command result scope changed.");
                           return;
                       }
                   if (!result.value("sessionId").toString().isEmpty() ||
                       result.value("sessionGeneration").toInteger() != 0) {
                       done({}, "Command result scope changed.");
                       return;
                   }
                   const auto status = result.value("status").toString();
                   if (!QStringList{"completed", "failed", "rejected"}.contains(status)) {
                       done(result, {});
                       return;
                   }
                   const auto content = result.value("encryptedResult").toObject();
                   const auto bytes = [&](const char* field) {
                       return daemon::CloudCrypto::fromBase64Url(
                           content.value(QLatin1String(field)).toString());
                   };
                   const int signer = content.value("senderKeyVersion").toInt();
                   if (content.value("version").toInt() != 1 ||
                       content.value("algorithm").toString() != "A256GCM" ||
                       content.value("keyEpoch").toInt() != 1 ||
                       content.value("keyId").toString() != keyIdFor(id) ||
                       content.value("senderDeviceId").toString() != machine ||
                       content.value("recipientDeviceId").toString() != deviceId_ ||
                       content.value("recipientKeyVersion").toInt() != keyVersion_ || signer < 1 ||
                       bytes("ciphertext").size() > maximumResult ||
                       result.value("resultCode").toString().isEmpty()) {
                       done({}, "Command result is not authenticated.");
                       return;
                   }
                   // Only a pinned host key version signs a result this client accepts.
                   hostKeys(machine, signer,
                            [this, result, content, status, id,
                             done](const QJsonObject& keys, const QString& error) mutable {
                                if (!error.isEmpty()) {
                                    done({}, error);
                                    return;
                                }
                                QJsonArray fields{"ketplus",
                                                  2,
                                                  "command-result",
                                                  id,
                                                  result.value("workspaceId"),
                                                  result.value("machineId"),
                                                  "",
                                                  0,
                                                  result.value("attempt"),
                                                  status,
                                                  result.value("resultCode")};
                                for (const auto& field : control_v2::contentScope(content))
                                    fields.append(field);
                                QString failure;
                                const auto plaintext = crypto_.decryptSealedAs(
                                    content, daemon::CloudCrypto::canonical(fields),
                                    daemon::CloudCrypto::fromBase64Url(
                                        keys.value("signingPublicKey").toString()),
                                    id, keyIdFor(id), &failure);
                                QJsonParseError parse;
                                const auto payload = QJsonDocument::fromJson(plaintext, &parse);
                                if (!failure.isEmpty() || plaintext.isEmpty() ||
                                    parse.error != QJsonParseError::NoError ||
                                    !payload.isObject()) {
                                    done({}, failure.isEmpty() ? "Invalid encrypted command result."
                                                               : failure);
                                    return;
                                }
                                result.remove("encryptedResult");
                                result.insert("result", payload.object());
                                done(result, {});
                            });
               });
}

void ControlV2Client::awaitWorktreeCommand(const QJsonObject& intent, const QJsonObject& plaintext,
                                           int timeoutMilliseconds, Callback done) {
    prepareWorktreeCommand(
        intent, plaintext,
        [this, timeoutMilliseconds, done = std::move(done)](const QJsonValue& prepared,
                                                            const QString& error) mutable {
            if (!error.isEmpty()) {
                done({}, error);
                return;
            }
            const auto envelope = prepared.toObject();
            submitWorktreeCommand(
                envelope, [this, envelope, timeoutMilliseconds, done = std::move(done)](
                              const QJsonValue&, const QString& failure) mutable {
                    if (!failure.isEmpty()) {
                        done({}, failure);
                        return;
                    }
                    // A read the host has not answered in time is abandoned.
                    pollWorktreeCommand(envelope,
                                        QDateTime::currentMSecsSinceEpoch() + timeoutMilliseconds,
                                        std::move(done));
                });
        });
}

void ControlV2Client::pollWorktreeCommand(const QJsonObject& envelope, qint64 deadline,
                                          Callback done) {
    const auto epoch = epoch_;
    worktreeCommand(envelope, [this, envelope, deadline, epoch, done = std::move(done)](
                                  const QJsonValue& value, const QString& error) mutable {
        if (!error.isEmpty() || epoch != epoch_) {
            done({}, error.isEmpty() ? QStringLiteral("Account changed.") : error);
            return;
        }
        if (value.toObject().contains("result")) {
            done(value, {});
            return;
        }
        if (QDateTime::currentMSecsSinceEpoch() >= deadline) {
            done({}, "The host did not answer the command.");
            return;
        }
        QTimer::singleShot(restorePollMilliseconds_, this,
                           [this, envelope, deadline, done = std::move(done)]() mutable {
                               pollWorktreeCommand(envelope, deadline, std::move(done));
                           });
    });
}
} // namespace ketplus::remote
