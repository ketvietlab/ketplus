#include "control/ControlV2Restore.h"
#include <QJsonDocument>
#include <QUuid>

// Transcript windows over Control v2: one restore request per page (ControlV2Restore.cpp), a
// smaller root when the host cannot seal a page, and a continuation chain for each large event.
namespace ketplus::remote {
using control_v2::sha256;
namespace {
constexpr int maximumContinuations = 4096;
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
} // namespace

void ControlV2Client::beginRoot(const std::shared_ptr<RestoreState>& state) {
    // The relay caps a restore request at 600 s (restoreRequestTtlSeconds), and its
    // continuations cannot outlive it. 540 s leaves a minute for clock skew between this
    // computer and the relay, so a skewed clock never makes the relay reject the deadline.
    state->deadline = QDateTime::currentDateTimeUtc().addSecs(540);
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
