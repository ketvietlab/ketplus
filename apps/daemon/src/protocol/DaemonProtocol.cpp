#include "protocol/DaemonProtocol.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>

#include <algorithm>

namespace ketplus::daemon::protocol {

ValidationResult validateEnvelope(const QJsonObject& envelope) {
    const QJsonValue version = envelope.value(QStringLiteral("protocol_version"));
    if (!version.isDouble() || version.toInt(-1) != currentVersion) {
        return {false, QStringLiteral("unsupported_protocol"),
                QStringLiteral("The protocol version is not supported.")};
    }
    const QString messageId = envelope.value(QStringLiteral("message_id")).toString();
    if (messageId.isEmpty() || messageId.size() > 128) {
        return {false, QStringLiteral("invalid_message_id"),
                QStringLiteral("message_id must contain between 1 and 128 characters.")};
    }
    const QString messageType = envelope.value(QStringLiteral("message_type")).toString();
    if (messageType.isEmpty() || messageType.size() > 128) {
        return {false, QStringLiteral("invalid_message_type"),
                QStringLiteral("message_type must contain between 1 and 128 characters.")};
    }
    if (!envelope.value(QStringLiteral("sent_at")).isString()) {
        return {false, QStringLiteral("invalid_sent_at"),
                QStringLiteral("sent_at must be an ISO-8601 string.")};
    }
    if (!envelope.value(QStringLiteral("payload")).isObject()) {
        return {false, QStringLiteral("invalid_payload"),
                QStringLiteral("payload must be a JSON object.")};
    }
    return {true, {}, {}};
}

QByteArray encodeFrame(const QJsonObject& envelope) {
    const QByteArray payload = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    QByteArray frame(4, Qt::Uninitialized);
    const quint32 size = static_cast<quint32>(payload.size());
    frame[0] = static_cast<char>((size >> 24U) & 0xffU);
    frame[1] = static_cast<char>((size >> 16U) & 0xffU);
    frame[2] = static_cast<char>((size >> 8U) & 0xffU);
    frame[3] = static_cast<char>(size & 0xffU);
    frame.append(payload);
    return frame;
}

QJsonObject response(const QJsonObject& request, const QJsonObject& payload) {
    return {
        {QStringLiteral("protocol_version"), currentVersion},
        {QStringLiteral("message_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("message_type"),
         request.value(QStringLiteral("message_type")).toString() + QStringLiteral(".result")},
        {QStringLiteral("correlation_id"), request.value(QStringLiteral("message_id")).toString()},
        {QStringLiteral("sent_at"), timestamp()},
        {QStringLiteral("payload"), payload}};
}

QJsonObject failure(const QJsonObject& request, const QString& code, const QString& message) {
    return response(request, {{QStringLiteral("ok"), false},
                              {QStringLiteral("error_code"), code},
                              {QStringLiteral("error_message"), message}});
}

QJsonObject oversizedReplacement(const QJsonObject& envelope, const qsizetype bytes) {
    QJsonObject replacement = envelope;
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    if (envelope.contains(QStringLiteral("correlation_id"))) {
        replacement.insert(
            QStringLiteral("payload"),
            QJsonObject{{QStringLiteral("ok"), false},
                        {QStringLiteral("error_code"), QStringLiteral("response_too_large")},
                        {QStringLiteral("error_message"),
                         QStringLiteral("The answer is too large for one message.")},
                        {QStringLiteral("bytes"), bytes}});
        return replacement;
    }
    QJsonObject slim{{QStringLiteral("truncated"), true}, {QStringLiteral("bytes"), bytes}};
    for (auto it = payload.begin(); it != payload.end(); ++it)
        if (!it->isObject() && !it->isArray() && (!it->isString() || it->toString().size() <= 256))
            slim.insert(it.key(), *it);
    // team.changed carries the whole team; its id and revision are what a client refetches.
    const QJsonObject team = payload.value(QStringLiteral("team")).toObject();
    if (!team.isEmpty()) {
        slim.insert(QStringLiteral("team_id"), team.value(QStringLiteral("id")));
        slim.insert(QStringLiteral("revision"), team.value(QStringLiteral("revision")));
    }
    replacement.insert(QStringLiteral("payload"), slim);
    return replacement;
}

int negotiatedMinor(const QJsonObject& helloPayload) {
    const int client = helloPayload.value(QStringLiteral("protocol_minor")).toInt(0);
    return std::clamp(client, 0, currentMinor);
}

QJsonArray features(const int minor) {
    // Minor 0 predates negotiation; its clients read the older per-response version fields.
    if (minor < 1)
        return {};
    return {QStringLiteral("provider.claude.credentials/1"),
            // daemon.hello carries host_key_fingerprint, the ADR 0119 machine id.
            QStringLiteral("daemon.host_identity/1"), QStringLiteral("provider.runtime_status"),
            QStringLiteral("session.fork"), QStringLiteral("transcript.large_event/1"),
            QStringLiteral("transcript.window/1"), QStringLiteral("transcript.activity_groups/1"),
            QStringLiteral("session.tasks/1"), QStringLiteral("session.tombstones/1"),
            QStringLiteral("session.catalog_live/1"), QStringLiteral("session.compact_window/1"),
            QStringLiteral("agents.messaging/1"), QStringLiteral("session.archive/1"),
            QStringLiteral("session.tags/1"), QStringLiteral("tags.catalog/1"),
            QStringLiteral("session.work_report/1"), QStringLiteral("session.awaiting_reply/1"),
            QStringLiteral("teams/1"), QStringLiteral("teams.tasks/1"),
            QStringLiteral("teams.summary/1"), QStringLiteral("teams.settings/1"),
            QStringLiteral("teams.storage/1"), QStringLiteral("transport.oversized/1")};
}

QString timestamp() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

} // namespace ketplus::daemon::protocol
