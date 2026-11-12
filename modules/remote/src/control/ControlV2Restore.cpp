#include "control/ControlV2Restore.h"
#include "control/ControlV2Content.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>

// One transcript.restore_window request over Control v2: the sealed command, its receipt, and
// the host-signed response fragments polled until the terminal digest
// (CloudControlClient::executeRestoreWindow is the host side).
namespace ketplus::remote {
using control_v2::segment;
using control_v2::sha256;
using control_v2::validContent;
namespace {
bool hexDigest(const QString& value) {
    static const QRegularExpression pattern("^[a-f0-9]{64}$");
    return pattern.match(value).hasMatch();
}
QByteArray responseAssociatedData(const QJsonObject& request, const QJsonObject& response,
                                  const QJsonObject& content) {
    QJsonArray fields{"ketplus",
                      2,
                      "transcript-restore-response",
                      request.value("id"),
                      request.value("ownerSubjectId"),
                      request.value("workspaceId"),
                      request.value("machineId"),
                      request.value("sessionId"),
                      request.value("sessionGeneration"),
                      request.value("credentialEpoch"),
                      request.value("snapshotCursor"),
                      response.value("responseCursor"),
                      response.value("terminal").toBool() ? 1 : 0,
                      response.value("responseDigest").toString()};
    for (const auto& field : control_v2::contentScope(content))
        fields.append(field);
    return daemon::CloudCrypto::canonical(fields);
}
} // namespace

void ControlV2Client::failRestore(const std::shared_ptr<RestoreState>& state,
                                  const QString& error) {
    if (std::exchange(state->finished, true))
        return;
    if (state->created && state->epoch == epoch_)
        // Best effort: the host stops serving it now instead of at its deadline.
        authorized("POST",
                   "/v2/mobile/restore-requests/" + segment(state->current.value("id").toString()) +
                       "/cancel",
                   QJsonObject{}, [](const QJsonValue&, const QString&) {});
    const auto done = std::move(state->done);
    done({}, error.isEmpty() ? QStringLiteral("restore_window_unavailable") : error);
}

void ControlV2Client::startRestore(const std::shared_ptr<RestoreState>& state,
                                   const QJsonObject& request, const QJsonObject& payload) {
    if (state->epoch != epoch_) {
        failRestore(state, "Account changed.");
        return;
    }
    state->current = request;
    state->created = false;
    state->responseCursor = 0;
    state->attempt = 0;
    state->responses.clear();
    state->bytes.clear();
    state->totalBytes = -1;
    state->terminalDigest.clear();
    const auto id = request.value("sessionId").toString();
    const auto generation = request.value("sessionGeneration").toInteger();
    auto command = request;
    command.insert("content", QJsonObject{{"keyId", QString("%1:%2:1").arg(id).arg(generation)},
                                          {"keyEpoch", 1},
                                          {"senderDeviceId", deviceId_},
                                          {"senderKeyVersion", keyVersion_}});
    QString error;
    const auto content = crypto_.encrypt(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                                         commandAssociatedData(command), id, generation, 1,
                                         deviceId_, keyVersion_, &error);
    if (content.isEmpty()) {
        failRestore(state, error.isEmpty() ? "Unable to encrypt the restore request." : error);
        return;
    }
    QJsonObject body;
    for (const auto* key : {"id", "sessionGeneration", "idempotencyKey", "kind", "credentialEpoch",
                            "snapshotCursor", "requestedByteCap", "continuationOfRequestId",
                            "continuationSequence", "continuationDigest", "deadlineAt"})
        if (request.contains(QLatin1String(key)))
            body.insert(QLatin1String(key), request.value(QLatin1String(key)));
    body.insert("content", content);
    authorized("POST", "/v2/mobile/sessions/" + segment(id) + "/commands", body,
               [this, state, request](const QJsonValue& value, const QString& failure) {
                   const auto receipt = value.toObject();
                   if (!failure.isEmpty()) {
                       failRestore(state, failure);
                       return;
                   }
                   bool same = true;
                   for (const auto* key : {"id", "sessionId", "sessionGeneration", "kind",
                                           "snapshotCursor", "requestedByteCap"})
                       same &=
                           receipt.value(QLatin1String(key)) == request.value(QLatin1String(key));
                   // The relay sends null continuation fields for a root request.
                   for (const auto* key : {"continuationOfRequestId", "continuationDigest"})
                       same &= receipt.value(QLatin1String(key)).toString() ==
                               request.value(QLatin1String(key)).toString();
                   if (!same ||
                       receipt.value("continuationSequence").toInteger(0) !=
                           request.value("continuationSequence").toInteger(0) ||
                       !QStringList{"queued", "claimed", "completed"}.contains(
                           receipt.value("status").toString())) {
                       failRestore(state, "Restore request scope changed.");
                       return;
                   }
                   state->created = true;
                   pollRestore(state);
               });
}

void ControlV2Client::pollRestore(const std::shared_ptr<RestoreState>& state) {
    if (state->finished)
        return;
    if (state->epoch != epoch_ || QDateTime::currentDateTimeUtc() >= state->deadline) {
        failRestore(state, state->epoch != epoch_ ? "Account changed." : QString{});
        return;
    }
    const auto request = state->current;
    authorized(
        "GET",
        "/v2/mobile/restore-requests/" + segment(request.value("id").toString()) +
            "/responses?afterCursor=" + QString::number(state->responseCursor) + "&limit=128",
        {}, [this, state, request](const QJsonValue& value, const QString& failure) {
            if (state->finished)
                return;
            // The host refuses a page whose sealed fragments exceed the byte cap. The relay
            // reports that refusal as 410 restore_window_unavailable (or, while it still serves
            // the page, as status "failed"); ask again for fewer events at the same snapshot so
            // backward paging never stops there. Halving bounds it to 7 retries from 128.
            const auto retryWithFewerEvents = [this, state, request] {
                if (request.value("continuationSequence").toInteger(0) != 0 ||
                    state->eventLimit <= 1)
                    return false;
                // The relay has already closed the refused request: nothing to cancel.
                state->created = false;
                state->eventLimit = qMax(1, state->eventLimit / 2);
                beginRoot(state);
                return true;
            };
            if (!failure.isEmpty()) {
                if (failure == QLatin1String("restore_window_unavailable") &&
                    retryWithFewerEvents())
                    return;
                failRestore(state, failure);
                return;
            }
            const auto page = value.toObject();
            const auto status = page.value("status").toString();
            const auto responses = page.value("responses").toArray();
            if (page.value("requestId") != request.value("id") ||
                page.value("ownerSubjectId") != request.value("ownerSubjectId") ||
                page.value("sessionId") != request.value("sessionId") ||
                page.value("sessionGeneration") != request.value("sessionGeneration") ||
                page.value("snapshotCursor") != request.value("snapshotCursor") ||
                page.value("continuationOfRequestId").toString() !=
                    request.value("continuationOfRequestId").toString() ||
                page.value("continuationSequence").toInteger(0) !=
                    request.value("continuationSequence").toInteger(0) ||
                page.value("continuationDigest").toString() !=
                    request.value("continuationDigest").toString() ||
                page.value("afterCursor").toInteger(-1) != state->responseCursor ||
                responses.size() > 128 ||
                !QStringList{"queued", "claimed", "completed", "rejected", "failed"}.contains(
                    status)) {
                failRestore(state, "Restore response page scope changed.");
                return;
            }
            const auto now = QDateTime::currentDateTimeUtc();
            const auto id = request.value("sessionId").toString();
            const auto generation = request.value("sessionGeneration").toInteger();
            for (const auto& item : responses) {
                const auto response = item.toObject();
                const auto cursor = response.value("responseCursor").toInteger(-1);
                const auto expires =
                    QDateTime::fromString(response.value("expiresAt").toString(), Qt::ISODate);
                const auto content = response.value("content").toObject();
                if (response.value("requestId") != request.value("id") ||
                    response.value("ownerSubjectId") != request.value("ownerSubjectId") ||
                    response.value("sessionId") != request.value("sessionId") ||
                    response.value("sessionGeneration") != request.value("sessionGeneration") ||
                    response.value("credentialEpoch") != request.value("credentialEpoch") ||
                    response.value("snapshotCursor") != request.value("snapshotCursor") ||
                    response.value("attempt").toInt() < 1 || !expires.isValid() || expires <= now ||
                    expires > state->deadline) {
                    failRestore(state, "Restore response scope changed.");
                    return;
                }
                if (const auto seen = state->responses.constFind(cursor);
                    seen != state->responses.cend()) {
                    if (*seen != response) {
                        failRestore(state, "Conflicting restore response.");
                        return;
                    }
                    continue;
                }
                const bool terminal = response.value("terminal").toBool();
                const auto digest = response.value("responseDigest").toString();
                if (cursor != state->responseCursor + 1 || !state->terminalDigest.isEmpty() ||
                    (state->attempt != 0 && state->attempt != response.value("attempt").toInt()) ||
                    (terminal ? !hexDigest(digest) : !digest.isEmpty()) ||
                    content.value("senderDeviceId") != request.value("machineId") ||
                    !validContent(content, id, generation, 512 * 1024)) {
                    failRestore(state, "Restore response is not authenticated.");
                    return;
                }
                if (!signedByVerifiedKey(id, generation, content)) {
                    failRestore(state, "The host signing key changed; pair this host again.");
                    return;
                }
                QString error;
                const auto plaintext =
                    crypto_.decrypt(content, responseAssociatedData(request, response, content),
                                    state->signing, id, generation, &error);
                const auto fragment = QJsonDocument::fromJson(plaintext).object();
                const auto data =
                    daemon::CloudCrypto::fromBase64Url(fragment.value("data").toString());
                const auto total = fragment.value("total_bytes").toInteger(-1);
                if (!error.isEmpty() || plaintext.isEmpty() ||
                    fragment.value("restore_response_version").toInt() != 1 ||
                    fragment.value("encoding").toString() != "json-utf8-fragment" ||
                    fragment.value("offset").toInteger(-1) != state->bytes.size() ||
                    data.isEmpty() || total < 1 ||
                    total > request.value("requestedByteCap").toInteger() ||
                    (state->totalBytes >= 0 && total != state->totalBytes) ||
                    state->bytes.size() + data.size() > total ||
                    terminal != (state->bytes.size() + data.size() == total)) {
                    failRestore(state,
                                error.isEmpty() ? "Restore response is not authenticated." : error);
                    return;
                }
                state->bytes.append(data);
                state->totalBytes = total;
                state->responseCursor = cursor;
                state->attempt = response.value("attempt").toInt();
                state->responses.insert(cursor, response);
                if (terminal)
                    state->terminalDigest = digest;
            }
            if (page.value("nextCursor").toInteger(-1) != state->responseCursor) {
                failRestore(state, "Restore response page scope changed.");
                return;
            }
            if (!state->terminalDigest.isEmpty()) {
                if (sha256(state->bytes) != state->terminalDigest) {
                    failRestore(state, "Restore response digest mismatch.");
                    return;
                }
                restoreCompleted(state, state->bytes);
                return;
            }
            if (status != "queued" && status != "claimed") {
                if (retryWithFewerEvents())
                    return;
                failRestore(state, {});
                return;
            }
            QTimer::singleShot(restorePollMilliseconds_, this,
                               [this, state] { pollRestore(state); });
        });
}
} // namespace ketplus::remote
