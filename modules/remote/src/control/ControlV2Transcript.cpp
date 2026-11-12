#include "LargeEventReassembler.h"
#include "control/ControlV2Client.h"
#include "control/ControlV2Content.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

// Transcript windows over Control v2: a sealed transcript.restore_window request per page, its
// host-signed response fragments, and a continuation chain for each large event in the page
// (CloudControlClient::executeRestoreWindow is the host side).
namespace ketplus::remote {
using control_v2::segment;
using control_v2::validContent;
namespace {
constexpr int maximumContinuations = 4096;
bool hexDigest(const QString& value) {
    static const QRegularExpression pattern("^[a-f0-9]{64}$");
    return pattern.match(value).hasMatch();
}
QString sha256(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
// The local window rules (CodexDaemonTranscript validWindow), plus the requested bounds.
bool validWindow(const QJsonObject& page, const QString& sessionId, qint64 generation,
                 qint64 snapshotCursor, qint64 beforeCursor, int eventLimit) {
    if (page.value("transcript_window_version").toInt() != 1 ||
        page.value("session_id").toString() != sessionId ||
        page.value("session_generation").toInteger() != generation ||
        !page.value("events").isArray() || !page.value("has_more_before").isBool() ||
        page.value("snapshot_cursor").toInteger(-1) < 0 ||
        page.value("snapshot_cursor").toInteger(-1) > snapshotCursor)
        return false;
    const auto events = page.value("events").toArray();
    if (events.size() > eventLimit)
        return false;
    qint64 previous = 0;
    for (const auto& value : events) {
        const auto event = value.toObject();
        const auto cursor = event.value("cursor").toInteger(-1);
        if (event.value("session_id").toString() != sessionId ||
            event.value("session_generation").toInteger() != generation || cursor <= previous ||
            cursor > snapshotCursor || (beforeCursor > 0 && cursor >= beforeCursor) ||
            !event.value("payload").isObject() || event.value("turn_sequence").toInteger(-1) < 0)
            return false;
        previous = cursor;
    }
    if (!events.isEmpty() && page.value("before_cursor").toInteger() !=
                                 events.first().toObject().value("cursor").toInteger())
        return false;
    return !page.value("has_more_before").toBool() || !events.isEmpty();
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

struct ControlV2Client::RestoreState {
    QJsonObject session;
    QByteArray signing;
    QJsonObject root;
    QJsonObject current;
    QDateTime deadline;
    qint64 beforeCursor{0};
    qint64 snapshotCursor{0};
    int eventLimit{128};
    int turnLimit{3};
    QString owner;
    int byteCap{512 * 1024};
    // The current request reached the relay, so abandoning it must cancel it there.
    bool created{false};
    // Response fragments of the current request.
    qint64 responseCursor{0};
    int attempt{0};
    QHash<qint64, QJsonObject> responses;
    QByteArray bytes;
    qint64 totalBytes{-1};
    QString terminalDigest;
    // The root page and the large event being reassembled.
    QJsonObject page;
    QJsonArray events;
    qsizetype nextEvent{0};
    qsizetype largeEvent{-1};
    std::unique_ptr<LargeEventReassembler> assembler;
    quint64 epoch{0};
    bool finished{false};
    Callback done;
};

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

void ControlV2Client::beginRoot(const std::shared_ptr<RestoreState>& state) {
    // The relay allows a restore request (and its continuations, which cannot outlive it)
    // 10 minutes; keep a margin for clock skew.
    state->deadline = QDateTime::currentDateTimeUtc().addSecs(590);
    state->deadline.setTime(QTime(state->deadline.time().hour(), state->deadline.time().minute(),
                                  state->deadline.time().second()));
    const auto token = QUuid::createUuid().toString(QUuid::Id128);
    const auto& session = state->session;
    state->root = {{"id", "restore_" + token},
                   {"idempotencyKey", "restore_idempotency_" + token},
                   {"workspaceId", session.value("workspaceId")},
                   {"machineId", session.value("machineId")},
                   {"sessionId", session.value("id")},
                   {"sessionGeneration", session.value("generation")},
                   {"capability", "transcript.restore_window"},
                   {"kind", "transcript.restore_window"},
                   {"ownerSubjectId", state->owner},
                   {"credentialEpoch", keyVersion_},
                   {"snapshotCursor", state->snapshotCursor},
                   {"requestedByteCap", state->byteCap},
                   {"continuationSequence", 0},
                   {"deadlineAt", state->deadline.toString(Qt::ISODate)}};
    startRestore(state, state->root,
                 {{"before_cursor", state->beforeCursor},
                  {"event_limit", state->eventLimit},
                  {"turn_limit", state->turnLimit}});
}

void ControlV2Client::transcriptWindow(const QJsonObject& session, const QJsonObject& window,
                                       Callback done) {
    const auto snapshot =
        window.value("snapshot_cursor").toInteger(session.value("lastEventCursor").toInteger(-1));
    const auto before = window.value("before_cursor").toInteger(0);
    const int eventLimit = window.value("event_limit").toInt(128);
    const int turnLimit = window.value("turn_limit").toInt(3);
    const int byteCap = window.value("byte_cap").toInt(512 * 1024);
    const auto owner = credential_.value("subject").toObject().value("id").toString();
    if (snapshot < 0 || before < 0 || (before > 0 && before > snapshot + 1) || eventLimit < 1 ||
        eventLimit > 128 || turnLimit < 1 || turnLimit > 3 || byteCap < 4096 ||
        byteCap > 512 * 1024 || owner.isEmpty()) {
        done({}, "Invalid transcript window request.");
        return;
    }
    auto state = std::make_shared<RestoreState>();
    state->session = session;
    state->beforeCursor = before;
    state->snapshotCursor = snapshot;
    state->eventLimit = eventLimit;
    state->turnLimit = turnLimit;
    state->owner = owner;
    state->byteCap = byteCap;
    state->epoch = epoch_;
    state->done = std::move(done);
    sessionKeys(session, [this, state](const QByteArray& signing, const QString& error) {
        if (!error.isEmpty()) {
            failRestore(state, error);
            return;
        }
        state->signing = signing;
        beginRoot(state);
    });
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
            if (!failure.isEmpty()) {
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
                // The host refuses a page whose sealed fragments exceed the byte cap; ask for
                // fewer events at the same snapshot so backward paging never stops there.
                if (request.value("continuationSequence").toInteger(0) == 0 &&
                    state->eventLimit > 1) {
                    state->created = false;
                    state->eventLimit = qMax(1, state->eventLimit / 2);
                    beginRoot(state);
                    return;
                }
                failRestore(state, {});
                return;
            }
            QTimer::singleShot(restorePollMilliseconds_, this,
                               [this, state] { pollRestore(state); });
        });
}

void ControlV2Client::restoreCompleted(const std::shared_ptr<RestoreState>& state,
                                       const QByteArray& bytes) {
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    const auto id = state->root.value("sessionId").toString();
    const auto generation = state->root.value("sessionGeneration").toInteger();
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        failRestore(state, "Invalid restored transcript page.");
        return;
    }
    const auto result = document.object();
    if (state->largeEvent < 0) {
        if (!validWindow(result, id, generation, state->root.value("snapshotCursor").toInteger(),
                         state->beforeCursor, state->eventLimit)) {
            failRestore(state, "Invalid remote transcript window.");
            return;
        }
        state->page = result;
        state->events = result.value("events").toArray();
        nextLargeEvent(state);
        return;
    }
    QString error;
    if (result.value("large_event_fragment_version").toInt() != 1 ||
        result.value("session_id").toString() != id ||
        result.value("session_generation").toInteger() != generation ||
        result.value("offset").toInteger(-1) != state->assembler->nextOffset() ||
        !state->assembler->accept(result, &error)) {
        failRestore(state, error.isEmpty() ? "Conflicting or incomplete large transcript fragment."
                                           : error);
        return;
    }
    if (!state->assembler->isComplete()) {
        requestLargeFragment(state);
        return;
    }
    const auto payload = state->assembler->payload(&error);
    if (payload.isEmpty()) {
        failRestore(state, error);
        return;
    }
    auto event = state->events.at(state->largeEvent).toObject();
    event.insert("payload", payload);
    state->events.replace(state->largeEvent, event);
    state->largeEvent = -1;
    state->assembler.reset();
    nextLargeEvent(state);
}

void ControlV2Client::nextLargeEvent(const std::shared_ptr<RestoreState>& state) {
    while (state->nextEvent < state->events.size()) {
        const auto index = state->nextEvent++;
        const auto event = state->events.at(index).toObject();
        const auto reference = event.value("payload").toObject().value("large_event").toObject();
        if (reference.isEmpty())
            continue;
        auto assembler = std::make_unique<LargeEventReassembler>(reference);
        if (!assembler->isValid() ||
            assembler->eventType() != event.value("event_type").toString() ||
            assembler->revision() > state->root.value("snapshotCursor").toInteger()) {
            failRestore(state, "Invalid large transcript reference.");
            return;
        }
        state->largeEvent = index;
        state->assembler = std::move(assembler);
        requestLargeFragment(state);
        return;
    }
    if (std::exchange(state->finished, true))
        return;
    auto page = state->page;
    page.insert("events", state->events);
    QJsonArray retained;
    for (const auto& value : page.value("partial_items").toArray())
        if (value.toObject().value("transfer_id").toString().isEmpty())
            retained.append(value);
    page.insert("partial_items", retained);
    page.remove("ok");
    const auto done = std::move(state->done);
    done(page, {});
}

void ControlV2Client::requestLargeFragment(const std::shared_ptr<RestoreState>& state) {
    // Each fragment is a continuation of the previous request, bound to its response digest.
    const auto parent = state->current;
    const auto sequence = parent.value("continuationSequence").toInteger(0) + 1;
    if (sequence > maximumContinuations) {
        failRestore(state, "Large transcript item has too many fragments.");
        return;
    }
    const auto hash = sha256(
        QStringLiteral("%1:%2").arg(state->root.value("id").toString()).arg(sequence).toUtf8());
    auto child = state->root;
    child.insert("id", "restore_" + hash);
    child.insert("idempotencyKey", "restore_idempotency_" + hash);
    child.insert("continuationOfRequestId", parent.value("id"));
    child.insert("continuationSequence", sequence);
    child.insert("continuationDigest", state->terminalDigest);
    const auto& assembler = *state->assembler;
    startRestore(state, child,
                 {{"large_event", QJsonObject{{"version", 1},
                                              {"transfer_id", assembler.transferId()},
                                              {"item_id", assembler.itemId()},
                                              {"revision", assembler.revision()},
                                              {"digest_sha256", assembler.digestSha256()},
                                              {"total_bytes", assembler.totalBytes()},
                                              {"offset", assembler.nextOffset()}}}});
}
} // namespace ketplus::remote
