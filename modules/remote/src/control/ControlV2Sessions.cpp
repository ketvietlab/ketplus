#include "control/ControlV2Client.h"
#include "control/ControlV2Content.h"
#include <QJsonDocument>
#include <QSet>
namespace ketplus::remote {
using control_v2::contentScope;
using control_v2::segment;
using control_v2::validContent;
namespace {
QJsonObject sessionScope(const QJsonObject& intent) {
    return {{"id", intent.value("sessionId")},
            {"generation", intent.value("sessionGeneration")},
            {"workspaceId", intent.value("workspaceId")},
            {"machineId", intent.value("machineId")}};
}
} // namespace
QByteArray ControlV2Client::commandAssociatedData(const QJsonObject& command) {
    const auto content = command.value("content").toObject();
    QJsonArray fields{"ketplus",
                      2,
                      "command",
                      command.value("id"),
                      command.value("workspaceId"),
                      command.value("machineId"),
                      command.value("sessionId"),
                      command.value("sessionGeneration"),
                      command.value("idempotencyKey"),
                      command.value("capability"),
                      command.value("kind")};
    for (const auto& value : contentScope(content))
        fields.append(value);
    // As the host reads a restore request (CloudControlClient::commandAssociatedData).
    if (command.value("kind").toString() == "transcript.restore_window")
        for (const auto& value : QJsonArray{
                 command.value("credentialEpoch"), command.value("ownerSubjectId"),
                 command.value("snapshotCursor"), command.value("requestedByteCap"),
                 command.value("continuationOfRequestId").toString(),
                 command.value("continuationSequence").toInteger(0),
                 command.value("continuationDigest").toString(), command.value("deadlineAt")})
            fields.append(value);
    if (command.value("kind").toString().startsWith("workspace.files."))
        for (const auto* key : {"repositoryId", "worktreeId", "workspaceRevision"})
            fields.append(command.value(QLatin1String(key)).toString());
    // Sealed to the machine before a conversation exists (CloudControlClient isWorktreeCommand).
    if (control_v2::worktreeScoped(command.value("kind").toString())) {
        for (const auto* key : {"ownerSubjectId", "repositoryId", "worktreeId",
                                "expectedCatalogRevision", "expectedLifecycleRevision"})
            fields.append(command.value(QLatin1String(key)));
        for (const auto* key : {"recipientDeviceId", "recipientKeyVersion", "ephemeralPublicKey"})
            fields.append(content.value(QLatin1String(key)));
    }
    return daemon::CloudCrypto::canonical(fields);
}
void ControlV2Client::senderKey(const QString& id, int version,
                                std::function<void(const QByteArray&, const QString&)> done) {
    hostKeys(id, version, [done = std::move(done)](const QJsonObject& keys, const QString& error) {
        done(error.isEmpty()
                 ? daemon::CloudCrypto::fromBase64Url(keys.value("signingPublicKey").toString())
                 : QByteArray{},
             error);
    });
}
void ControlV2Client::hostKeys(const QString& id, int version,
                               std::function<void(const QJsonObject&, const QString&)> done) {
    authorized(
        "GET", "/v2/mobile/device-keys/" + segment(id) + "/" + QString::number(version), {},
        [this, id, version, done = std::move(done)](const QJsonValue& value, const QString& error) {
            const auto object = value.toObject();
            const auto keys = object.value("keys").toObject();
            const auto signing =
                daemon::CloudCrypto::fromBase64Url(keys.value("signingPublicKey").toString());
            const auto fingerprint = daemon::CloudCrypto::keyFingerprint(keys);
            if (!error.isEmpty() || object.value("deviceId").toString() != id ||
                object.value("keyVersion").toInt() != version ||
                keys.value("signingAlgorithm").toString() != "Ed25519" || signing.size() != 32 ||
                fingerprint.isEmpty()) {
                done({}, error.isEmpty() ? "Invalid host signing identity." : error);
                return;
            }
            // The relay serves these keys; only ones pinned when this client paired with the
            // host are accepted. A new host key version needs a new pairing (#88).
            const auto pin = QStringLiteral("%1/%2").arg(id).arg(version);
            if (!hostPinsStored_ && credential_.contains("deviceId")) {
                // Paired before pins: the first host seen is adopted once, then pins apply.
                QString storageError;
                if (!storeHostPins({{pin, fingerprint}}, &storageError)) {
                    done({},
                         storageError.isEmpty() ? "Unable to store the host pin." : storageError);
                    return;
                }
            } else if (hostPins_.value(pin).toString() != fingerprint) {
                done({}, QString::fromLatin1(control_v2::hostNotPaired));
                return;
            }
            done(keys, {});
        });
}
bool ControlV2Client::signedByVerifiedKey(const QString& sessionId, qint64 generation,
                                          const QJsonObject& content) const {
    const auto version =
        signerVersions_.value(QStringLiteral("%1:%2").arg(sessionId).arg(generation));
    return version > 0 && content.value("senderKeyVersion").toInt() == version;
}
void ControlV2Client::sessionKeys(const QJsonObject& session,
                                  std::function<void(const QByteArray&, const QString&)> done) {
    const auto id = session.value("id").toString();
    const auto generation = session.value("generation").toInteger();
    const auto machine = session.value("machineId").toString();
    if (deviceId_.isEmpty() || id.isEmpty() || generation < 1 || machine.isEmpty() ||
        session.value("workspaceId").toString().isEmpty()) {
        done({}, "A paired identity and complete remote session scope are required.");
        return;
    }
    authorized(
        "GET",
        "/v2/mobile/sessions/" + segment(id) + "/keys?generation=" + QString::number(generation),
        {},
        [this, id, generation, machine, done = std::move(done)](const QJsonValue& value,
                                                                const QString& error) {
            if (!error.isEmpty()) {
                done({}, error);
                return;
            }
            for (const auto& entry : value.toArray()) {
                const auto envelope = entry.toObject();
                if (envelope.value("sessionId").toString() != id ||
                    envelope.value("sessionGeneration").toInteger() != generation ||
                    envelope.value("recipientDeviceId").toString() != deviceId_ ||
                    envelope.value("recipientKeyVersion").toInt() != keyVersion_ ||
                    envelope.value("senderDeviceId").toString() != machine ||
                    envelope.value("keyEpoch").toInt() != 1)
                    continue;
                senderKey(
                    machine, envelope.value("senderKeyVersion").toInt(),
                    [this, id, generation, envelope,
                     done = std::move(done)](const QByteArray& signing, const QString& failure) {
                        QString error = failure;
                        if (error.isEmpty() &&
                            !crypto_.importSessionKey(envelope, signing, deviceId_, keyVersion_,
                                                      &error))
                            error = error.isEmpty() ? "Unable to import session key." : error;
                        if (error.isEmpty())
                            signerVersions_.insert(QStringLiteral("%1:%2").arg(id).arg(generation),
                                                   envelope.value("senderKeyVersion").toInt());
                        done(error.isEmpty() ? signing : QByteArray{}, error);
                    });
                return;
            }
            done({}, QString::fromLatin1(control_v2::noSessionKeyGrant));
        });
}
void ControlV2Client::events(const QJsonObject& session, qint64 after, Callback done) {
    if (after < 0) {
        done({}, "Invalid event cursor.");
        return;
    }
    sessionKeys(session, [this, session, after, done = std::move(done)](const QByteArray& signing,
                                                                        const QString& error) {
        if (!error.isEmpty()) {
            done({}, error);
            return;
        }
        const auto id = session.value("id").toString();
        const auto generation = session.value("generation").toInteger();
        authorized(
            "GET",
            "/v2/mobile/sessions/" + segment(id) +
                "/events?generation=" + QString::number(generation) +
                "&afterCursor=" + QString::number(after) + "&limit=100&waitSeconds=0",
            {},
            [this, session, id, generation, after, signing,
             done = std::move(done)](const QJsonValue& value, const QString& failure) {
                if (!failure.isEmpty()) {
                    done({}, failure);
                    return;
                }
                auto page = value.toObject();
                if (page.value("sessionId").toString() != id ||
                    page.value("sessionGeneration").toInteger() != generation ||
                    page.value("afterCursor").toInteger() != after ||
                    page.value("nextCursor").toInteger() < after) {
                    done({}, "Event page scope changed.");
                    return;
                }
                QJsonArray decoded;
                auto cursor = after;
                for (const auto& item : page.value("events").toArray()) {
                    auto event = item.toObject();
                    const auto content = event.value("content").toObject();
                    if (event.value("workspaceId") != session.value("workspaceId") ||
                        event.value("sessionId").toString() != id ||
                        event.value("sessionGeneration").toInteger() != generation ||
                        event.value("cursor").toInteger() <= cursor ||
                        event.value("cursor").toInteger() > page.value("nextCursor").toInteger() ||
                        content.value("senderDeviceId") != session.value("machineId") ||
                        !validContent(content, id, generation, 256 * 1024)) {
                        done({}, "Invalid event routing metadata.");
                        return;
                    }
                    QJsonArray fields{"ketplus",
                                      2,
                                      "event",
                                      event.value("workspaceId"),
                                      id,
                                      generation,
                                      event.value("cursor"),
                                      event.value("turnSequence"),
                                      event.value("eventType")};
                    if (!signedByVerifiedKey(id, generation, content)) {
                        done({}, "The host signing key changed; pair this host again.");
                        return;
                    }
                    for (const auto& field : contentScope(content))
                        fields.append(field);
                    QString error;
                    const auto plaintext =
                        crypto_.decrypt(content, daemon::CloudCrypto::canonical(fields), signing,
                                        id, generation, &error);
                    QJsonParseError parse;
                    const auto json = QJsonDocument::fromJson(plaintext, &parse);
                    if (!error.isEmpty() || parse.error != QJsonParseError::NoError ||
                        !json.isObject()) {
                        done({}, error.isEmpty() ? "Invalid encrypted event payload." : error);
                        return;
                    }
                    event.remove("content");
                    event.insert("payload", json.object());
                    decoded.append(event);
                    cursor = event.value("cursor").toInteger();
                }
                page.insert("events", decoded);
                done(page, {});
            });
    });
}
void ControlV2Client::prepareCommand(const QJsonObject& intent, const QJsonObject& plaintext,
                                     Callback done) {
    static const QSet<QString> supported{"attachment.stage.begin",
                                         "attachment.stage.chunk",
                                         "attachment.stage.commit",
                                         "attachment.stage.cancel",
                                         "session.start_turn",
                                         "session.steer",
                                         "session.interrupt",
                                         "session.rename",
                                         "session.fork",
                                         "approval.resolve",
                                         "question.answer",
                                         "session.configuration.read",
                                         "session.configuration.apply",
                                         "session.pending_approvals",
                                         "session.execution_policy.apply",
                                         "session.mode.apply",
                                         "workspace.files.tree",
                                         "workspace.files.search",
                                         "workspace.files.file",
                                         "workspace.files.diff"};
    if (!supported.contains(intent.value("kind").toString()) ||
        intent.value("id").toString().isEmpty() ||
        intent.value("idempotencyKey").toString().isEmpty() ||
        intent.value("capability").toString().isEmpty() ||
        QJsonDocument(plaintext).toJson(QJsonDocument::Compact).size() > 64 * 1024) {
        done({}, "Unsupported or oversized command intent.");
        return;
    }
    sessionKeys(sessionScope(intent), [this, intent, plaintext, done = std::move(done)](
                                          const QByteArray&, const QString& failure) {
        if (!failure.isEmpty()) {
            done({}, failure);
            return;
        }
        auto command = intent;
        const auto id = intent.value("sessionId").toString();
        const auto generation = intent.value("sessionGeneration").toInteger();
        command.insert("content", QJsonObject{{"keyId", QString("%1:%2:1").arg(id).arg(generation)},
                                              {"keyEpoch", 1},
                                              {"senderDeviceId", deviceId_},
                                              {"senderKeyVersion", keyVersion_}});
        QString error;
        const auto encrypted = crypto_.encrypt(
            QJsonDocument(plaintext).toJson(QJsonDocument::Compact), commandAssociatedData(command),
            id, generation, 1, deviceId_, keyVersion_, &error);
        if (encrypted.isEmpty()) {
            done({}, error.isEmpty() ? "Unable to encrypt command." : error);
            return;
        }
        command.insert("content", encrypted);
        done(command, {});
    });
}
void ControlV2Client::submit(const QJsonObject& intent, Callback done) {
    const auto content = intent.value("content").toObject();
    if (deviceId_.isEmpty() || content.value("senderDeviceId").toString() != deviceId_ ||
        content.value("senderKeyVersion").toInt() != keyVersion_ ||
        !validContent(content, intent.value("sessionId").toString(),
                      intent.value("sessionGeneration").toInteger(), 64 * 1024)) {
        done({}, "Prepared command identity changed.");
        return;
    }
    authorized("POST",
               "/v2/mobile/sessions/" + segment(intent.value("sessionId").toString()) + "/commands",
               intent, std::move(done));
}
void ControlV2Client::command(const QJsonObject& intent, Callback done) {
    sessionKeys(sessionScope(intent), [this, intent, done = std::move(done)](
                                          const QByteArray& signing, const QString& error) {
        if (!error.isEmpty()) {
            done({}, error);
            return;
        }
        authorized(
            "GET", "/v2/mobile/commands/" + segment(intent.value("id").toString()), {},
            [this, intent, signing, done = std::move(done)](const QJsonValue& value,
                                                            const QString& failure) {
                if (!failure.isEmpty()) {
                    done({}, failure);
                    return;
                }
                auto result = value.toObject();
                for (const auto* key : {"id", "workspaceId", "machineId", "sessionId",
                                        "sessionGeneration", "idempotencyKey", "kind"})
                    if (result.value(QLatin1String(key)) != intent.value(QLatin1String(key))) {
                        done({}, "Command result scope changed.");
                        return;
                    }
                const auto status = result.value("status").toString();
                if (!QStringList{"completed", "failed", "rejected"}.contains(status)) {
                    done(result, {});
                    return;
                }
                const auto content = result.value("encryptedResult").toObject();
                const auto id = intent.value("sessionId").toString();
                const auto generation = intent.value("sessionGeneration").toInteger();
                if (!validContent(content, id, generation, 512 * 1024) ||
                    content.value("senderDeviceId") != intent.value("machineId") ||
                    result.value("resultCode").toString().isEmpty()) {
                    done({}, "Command result is not authenticated.");
                    return;
                }
                QJsonArray fields{"ketplus",
                                  2,
                                  "command-result",
                                  intent.value("id"),
                                  intent.value("workspaceId"),
                                  intent.value("machineId"),
                                  id,
                                  generation,
                                  result.value("attempt"),
                                  status,
                                  result.value("resultCode")};
                for (const auto& field : contentScope(content))
                    fields.append(field);
                if (intent.value("kind").toString().startsWith("workspace.files."))
                    for (const auto* key : {"repositoryId", "worktreeId", "workspaceRevision"})
                        fields.append(intent.value(QLatin1String(key)).toString());
                QString error;
                const auto bytes = crypto_.decrypt(content, daemon::CloudCrypto::canonical(fields),
                                                   signing, id, generation, &error);
                QJsonParseError parse;
                const auto payload = QJsonDocument::fromJson(bytes, &parse);
                if (!error.isEmpty() || parse.error != QJsonParseError::NoError ||
                    !payload.isObject()) {
                    done({}, error.isEmpty() ? "Invalid encrypted command result." : error);
                    return;
                }
                result.remove("encryptedResult");
                result.insert("result", payload.object());
                done(result, {});
            });
    });
}
} // namespace ketplus::remote
