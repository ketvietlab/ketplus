#pragma once

#include "RemoteControlFixture.h"

#include <QSet>
#include <QUrlQuery>

#include <functional>

namespace {
QString hex(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QString b64(const QByteArray& bytes) { return QString::fromLatin1(CloudCrypto::base64Url(bytes)); }

// Plays the paired host behind the relay: opens each sealed restore request and answers with a
// fragmented, host-signed page, as CloudControlClient::executeRestoreWindow does.
struct RestoreHost {
    explicit RestoreHost(EncryptedFixture& host) : fixture(host) {}
    EncryptedFixture& fixture;
    // The conversation the host restores; the relay routes only its requests here.
    QString session{"opaque-session"};
    int generation{3};
    // When set, answers a root or older page instead of page() (a large-event read still uses
    // page()).
    std::function<QJsonObject(const QJsonObject& payload)> pages;
    QByteArray clientSigning;
    QByteArray large;
    QHash<QString, QJsonObject> requests;
    QHash<QString, QJsonArray> responses;
    QHash<QString, QString> digests;
    QList<QJsonObject> payloads;
    bool tamper{false};
    int senderKeyVersion{1};
    // The host refuses a root page with more events, as it does one too large for its byte cap.
    int maximumEvents{128};
    QSet<QString> failed;
    // The relay answers a failed restore request 410 restore_window_unavailable
    // (readTranscriptRestoreResponses); false serves the 200 page with status "failed" instead.
    bool failedAs410{true};
    QStringList cancelled;
    // As the relay's commandDto: a missing optional field is null, never absent.
    static QJsonValue orNull(const QJsonObject& command, const char* key) {
        return command.contains(QLatin1String(key)) ? command.value(QLatin1String(key))
                                                    : QJsonValue(QJsonValue::Null);
    }

    QJsonObject event(int cursor, int turn, const QJsonObject& payload,
                      const QString& type = "assistant.message") const {
        return {{"session_id", session}, {"session_generation", generation},
                {"cursor", cursor},      {"turn_sequence", turn},
                {"event_type", type},    {"payload", payload}};
    }
    QJsonObject reference() const {
        return {{"version", 1},
                {"encoding", "json-utf8"},
                {"transfer_id", "t-big"},
                {"event_type", "item.completed"},
                {"item_id", "item-big"},
                {"digest_sha256", hex(large)},
                {"total_bytes", large.size()},
                {"revision", 8}};
    }
    QJsonObject page(const QJsonObject& payload) const {
        if (payload.contains("large_event")) {
            const auto selector = payload.value("large_event").toObject();
            const auto offset = selector.value("offset").toInteger();
            const auto data = large.mid(offset, 100);
            const auto next = offset + data.size();
            return {{"ok", true},
                    {"large_event_fragment_version", 1},
                    {"session_id", session},
                    {"session_generation", generation},
                    {"transfer_id", "t-big"},
                    {"event_type", "item.completed"},
                    {"item_id", "item-big"},
                    {"revision", 8},
                    {"digest_sha256", hex(large)},
                    {"total_bytes", large.size()},
                    {"offset", offset},
                    {"next_offset", next},
                    {"complete", next == large.size()},
                    {"data", b64(data)}};
        }
        if (pages)
            return pages(payload);
        if (payload.value("before_cursor").toInteger() == 0)
            return {
                {"ok", true},
                {"transcript_window_version", 1},
                {"session_id", session},
                {"session_generation", generation},
                {"snapshot_cursor", 9},
                {"projection_revision", 1},
                {"before_cursor", 7},
                {"has_more_before", true},
                {"events", QJsonArray{event(7, 2, {{"text", "newest prompt"}}),
                                      event(8, 2, {{"large_event", reference()}}, "item.completed"),
                                      event(9, 2, {{"text", "newest answer"}})}},
                {"partial_items", QJsonArray{QJsonObject{{"transfer_id", "t-big"}}}}};
        return {{"ok", true},
                {"transcript_window_version", 1},
                {"session_id", session},
                {"session_generation", generation},
                {"snapshot_cursor", 9},
                {"projection_revision", 1},
                {"before_cursor", 3},
                {"has_more_before", false},
                {"events", QJsonArray{event(3, 1, {{"text", "first prompt"}}),
                                      event(5, 1, {{"text", "first answer"}})}}};
    }
    std::optional<std::pair<int, QJsonObject>> answer(const QNetworkRequest& request,
                                                      const QByteArray& body) {
        const auto path = request.url().path();
        if (path == prefix + "/v2/mobile/sessions/" + session + "/commands")
            return accept(QJsonDocument::fromJson(body).object());
        const QString responsesPrefix = prefix + "/v2/mobile/restore-requests/";
        if (path.startsWith(responsesPrefix) && path.endsWith("/cancel")) {
            cancelled.append(
                path.mid(responsesPrefix.size()).chopped(QStringLiteral("/cancel").size()));
            return std::pair{200, QJsonObject{{"ok", true}}};
        }
        if (!path.startsWith(responsesPrefix) || !path.endsWith("/responses"))
            return std::nullopt;
        const auto id =
            path.mid(responsesPrefix.size()).chopped(QStringLiteral("/responses").size());
        const auto command = requests.value(id);
        const auto after = QUrlQuery(request.url()).queryItemValue("afterCursor").toLongLong();
        if (failed.contains(id) && failedAs410)
            return std::pair{410, QJsonObject{{"code", "restore_window_unavailable"},
                                              {"message", "restore_window_unavailable"}}};
        QJsonArray selected;
        for (const auto& value : responses.value(id))
            if (value.toObject().value("responseCursor").toInteger() > after)
                selected.append(value);
        return std::pair{
            200,
            QJsonObject{{"requestId", id},
                        {"ownerSubjectId", "fixture-owner"},
                        {"sessionId", session},
                        {"sessionGeneration", generation},
                        {"snapshotCursor", command.value("snapshotCursor")},
                        {"continuationOfRequestId", orNull(command, "continuationOfRequestId")},
                        {"continuationSequence", command.value("continuationSequence")},
                        {"continuationDigest", orNull(command, "continuationDigest")},
                        {"afterCursor", after},
                        {"nextCursor", responses.value(id).size()},
                        {"status", failed.contains(id) ? "failed" : "completed"},
                        {"responses", selected}}};
    }
    std::pair<int, QJsonObject> accept(QJsonObject command) {
        // The relay adds the routing scope and the owner it authenticated.
        command.insert("workspaceId", "workspace");
        command.insert("machineId", "linux-host");
        command.insert("sessionId", session);
        command.insert("capability", "transcript.restore_window");
        command.insert("ownerSubjectId", "fixture-owner");
        const auto content = command.value("content").toObject();
        QString error;
        const auto plaintext =
            fixture.host.decrypt(content, ControlV2Client::commandAssociatedData(command),
                                 clientSigning, session, generation, &error);
        if (plaintext.isEmpty())
            qFatal("Host could not open the restore request: %s", qPrintable(error));
        const auto payload = QJsonDocument::fromJson(plaintext).object();
        const auto id = command.value("id").toString();
        if (command.value("continuationSequence").toInteger() > 0) {
            const auto parent = command.value("continuationOfRequestId").toString();
            if (!requests.contains(parent) ||
                command.value("continuationDigest").toString() != digests.value(parent))
                qFatal("Continuation is not bound to its parent's response");
        }
        payloads.append(payload);
        requests.insert(id, command);
        QJsonObject receipt;
        for (const auto* key : {"id", "sessionId", "sessionGeneration", "kind", "snapshotCursor",
                                "requestedByteCap", "continuationSequence"})
            receipt.insert(QLatin1String(key), command.value(QLatin1String(key)));
        for (const auto* key : {"continuationOfRequestId", "continuationDigest"})
            receipt.insert(QLatin1String(key), orNull(command, key));
        receipt.insert("status", "queued");
        if (!payload.contains("large_event") &&
            payload.value("event_limit").toInt() > maximumEvents) {
            failed.insert(id);
            responses.insert(id, QJsonArray{});
            return {202, receipt};
        }
        const auto bytes = QJsonDocument(page(payload)).toJson(QJsonDocument::Compact);
        const auto digest = hex(bytes);
        digests.insert(id, digest);
        QJsonArray list;
        constexpr qsizetype fragment = 160;
        for (qsizetype offset = 0; offset < bytes.size(); offset += fragment) {
            const auto data = bytes.mid(offset, fragment);
            const bool terminal = offset + data.size() == bytes.size();
            const int cursor = list.size() + 1;
            const QJsonObject plain{{"restore_response_version", 1},
                                    {"encoding", "json-utf8-fragment"},
                                    {"offset", offset},
                                    {"total_bytes", bytes.size()},
                                    {"data", b64(data)}};
            const auto aad = CloudCrypto::canonical(
                {"ketplus", 2, "transcript-restore-response", id, "fixture-owner", "workspace",
                 "linux-host", session, generation, command.value("credentialEpoch"),
                 command.value("snapshotCursor"), cursor, terminal ? 1 : 0,
                 terminal ? digest : QString{},
                 QStringLiteral("%1:%2:1").arg(session).arg(generation), 1, "linux-host",
                 senderKeyVersion});
            auto sealed =
                fixture.host.encrypt(QJsonDocument(plain).toJson(QJsonDocument::Compact), aad,
                                     session, generation, 1, "linux-host", senderKeyVersion);
            if (tamper && terminal) {
                auto cipher = CloudCrypto::fromBase64Url(sealed.value("ciphertext").toString());
                cipher[0] = char(cipher[0] ^ 0x01);
                sealed.insert("ciphertext", b64(cipher));
            }
            list.append(QJsonObject{
                {"requestId", id},
                {"ownerSubjectId", "fixture-owner"},
                {"sessionId", session},
                {"sessionGeneration", generation},
                {"credentialEpoch", command.value("credentialEpoch")},
                {"snapshotCursor", command.value("snapshotCursor")},
                {"responseCursor", cursor},
                {"attempt", 1},
                {"terminal", terminal},
                {"responseDigest", terminal ? digest : QString{}},
                {"expiresAt",
                 QDateTime::currentDateTimeUtc().addSecs(120).toString(Qt::ISODateWithMs)},
                {"content", sealed}});
        }
        responses.insert(id, list);
        return {202, receipt};
    }
};
} // namespace
