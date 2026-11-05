#include "runtime/SessionHost.h"

#include "runtime/ApprovalBroker.h"

#include <limits>

namespace ketplus::daemon {
namespace {

QJsonObject windowError(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

bool integerField(const QJsonObject& object, const QString& key) {
    if (!object.contains(key)) {
        return true;
    }
    const QJsonValue value = object.value(key);
    return value.isDouble() && value.toDouble() >= 0 &&
           value.toDouble() == static_cast<double>(value.toInteger(-1));
}

} // namespace

QJsonObject SessionHost::transcriptWindow(const QJsonObject& request) const {
    return transcriptWindow(request, false);
}

QJsonObject SessionHost::restoreTranscriptWindow(const QJsonObject& request) const {
    return transcriptWindow(request, true);
}

QJsonObject SessionHost::transcriptWindow(const QJsonObject& request,
                                          const bool bindCurrentProjection) const {
    Session* session = nullptr;
    const QJsonObject failure = const_cast<SessionHost*>(this)->checkedSession(request, &session);
    if (!failure.isEmpty()) {
        return failure;
    }
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    if (bindCurrentProjection && payload.contains(QStringLiteral("projection_revision"))) {
        return windowError(QStringLiteral("invalid_page"),
                           QStringLiteral("Restore requests cannot select a local projection."));
    }
    for (const QString& key :
         {QStringLiteral("before_cursor"), QStringLiteral("after_cursor"),
          QStringLiteral("snapshot_cursor"), QStringLiteral("projection_revision"),
          QStringLiteral("turn_limit"), QStringLiteral("event_limit")}) {
        if (!integerField(payload, key)) {
            return windowError(QStringLiteral("invalid_page"),
                               QStringLiteral("Invalid window parameter."));
        }
    }
    for (const QString& key : {QStringLiteral("turn_limit"), QStringLiteral("event_limit")}) {
        if (payload.value(key).toInteger() > std::numeric_limits<int>::max()) {
            return windowError(QStringLiteral("invalid_page"),
                               QStringLiteral("Window limit is too large."));
        }
    }
    const qint64 beforeCursor = payload.value(QStringLiteral("before_cursor")).toInteger(0);
    // after_cursor (exclusive event cursor) pages forward inside the same fenced snapshot. It
    // is mutually exclusive with before_cursor and never valid for restore requests.
    const bool forward = payload.contains(QStringLiteral("after_cursor"));
    const qint64 afterCursor = payload.value(QStringLiteral("after_cursor")).toInteger(0);
    if (forward && bindCurrentProjection) {
        return windowError(
            QStringLiteral("invalid_page"),
            QStringLiteral("Restore requests cannot page forward with after_cursor."));
    }
    if (forward && payload.contains(QStringLiteral("before_cursor"))) {
        return windowError(QStringLiteral("invalid_page"),
                           QStringLiteral("after_cursor cannot be combined with before_cursor."));
    }
    if (forward && afterCursor < 1) {
        return windowError(QStringLiteral("invalid_page"),
                           QStringLiteral("after_cursor must be a positive event cursor."));
    }
    const qint64 snapshotCursor = payload.value(QStringLiteral("snapshot_cursor")).toInteger(-1);
    const int turnLimit = payload.value(QStringLiteral("turn_limit")).toInt(3);
    const int eventLimit = payload.value(QStringLiteral("event_limit")).toInt(128);
    const TranscriptWindow page =
        forward ? journal_->transcriptWindowAfter(
                      session->stored.id, session->stored.generation, afterCursor, snapshotCursor,
                      payload.value(QStringLiteral("projection_revision")).toInteger(-1), turnLimit,
                      eventLimit)
        : bindCurrentProjection
            ? journal_->restoreTranscriptWindow(session->stored.id, session->stored.generation,
                                                beforeCursor, snapshotCursor, turnLimit, eventLimit)
            : journal_->transcriptWindow(
                  session->stored.id, session->stored.generation, beforeCursor, snapshotCursor,
                  payload.value(QStringLiteral("projection_revision")).toInteger(-1), turnLimit,
                  eventLimit);
    if (!page.errorCode.isEmpty()) {
        return windowError(page.errorCode, page.errorMessage);
    }
    QJsonObject result{
        {QStringLiteral("ok"), true},
        {QStringLiteral("transcript_window_version"), 1},
        {QStringLiteral("session_id"), session->stored.id},
        {QStringLiteral("session_generation"), session->stored.generation},
        {QStringLiteral("snapshot_cursor"), page.snapshotCursor},
        {QStringLiteral("projection_revision"), page.projectionRevision},
        {QStringLiteral("before_cursor"), page.beforeCursor},
        {QStringLiteral("has_more_before"), page.hasMoreBefore},
        {QStringLiteral("after_cursor"), page.afterCursor},
        {QStringLiteral("has_more_after"), page.hasMoreAfter},
        {QStringLiteral("partial_first_turn"), page.partialFirstTurn},
        {QStringLiteral("partial_items"), page.partialItems},
        {QStringLiteral("events"), page.events},
        {QStringLiteral("state"), session->stored.state},
        {QStringLiteral("current_turn_sequence"), session->stored.currentTurnSequence},
        {QStringLiteral("active_turn_id"), session->stored.turnId}};
    // Approval state is current, not an instruction to replay historical requests.
    const auto approvals = approvals_->pendingPage(session->stored.id, session->stored.generation);
    if (approvals.contains(QStringLiteral("error_code"))) {
        return windowError(approvals.value(QStringLiteral("error_code")).toString(),
                           QStringLiteral("A pending approval exceeds the local page limit."));
    }
    for (auto it = approvals.begin(); it != approvals.end(); ++it) {
        result.insert(it.key(), it.value());
    }
    return result;
}

QJsonObject SessionHost::activityGroupPage(const QJsonObject& request) const {
    Session* session = nullptr;
    const auto failure = const_cast<SessionHost*>(this)->checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    const auto payload = request.value("payload").toObject();
    for (const auto& key : {QStringLiteral("after_cursor"), QStringLiteral("snapshot_cursor"),
                            QStringLiteral("projection_revision"), QStringLiteral("event_limit")})
        if (!integerField(payload, key))
            return windowError("invalid_page", "Invalid activity group parameter.");
    return journal_->activityGroupPage(session->stored.id, session->stored.generation, payload);
}

QJsonObject SessionHost::transcriptLargeEvent(const QJsonObject& request) const {
    Session* session = nullptr;
    const QJsonObject failure = const_cast<SessionHost*>(this)->checkedSession(request, &session);
    if (!failure.isEmpty()) {
        return failure;
    }
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const QString transferId = payload.value(QStringLiteral("transfer_id")).toString();
    if (!integerField(payload, QStringLiteral("offset")) ||
        !integerField(payload, QStringLiteral("maximum_bytes")) ||
        payload.value(QStringLiteral("maximum_bytes")).toInteger(128 * 1024) > 128 * 1024) {
        return windowError(QStringLiteral("invalid_large_event_fragment"),
                           QStringLiteral("Invalid large-event fragment boundary."));
    }
    const LargeEventFragment fragment = journal_->largeEventFragment(
        session->stored.id, session->stored.generation, transferId,
        payload.value(QStringLiteral("offset")).toInteger(0),
        payload.value(QStringLiteral("maximum_bytes")).toInt(128 * 1024));
    if (!fragment.errorCode.isEmpty()) {
        return windowError(fragment.errorCode, fragment.errorMessage);
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("large_event_fragment_version"), 1},
            {QStringLiteral("session_id"), session->stored.id},
            {QStringLiteral("session_generation"), session->stored.generation},
            {QStringLiteral("transfer_id"), fragment.transferId},
            {QStringLiteral("event_type"), fragment.eventType},
            {QStringLiteral("item_id"), fragment.itemId},
            {QStringLiteral("revision"), fragment.revision},
            {QStringLiteral("digest_sha256"), fragment.digestSha256},
            {QStringLiteral("offset"), fragment.offset},
            {QStringLiteral("total_bytes"), fragment.totalBytes},
            {QStringLiteral("next_offset"), fragment.nextOffset},
            {QStringLiteral("complete"), fragment.complete},
            {QStringLiteral("data"),
             QString::fromLatin1(fragment.data.toBase64(QByteArray::Base64UrlEncoding |
                                                        QByteArray::OmitTrailingEquals))}};
}

QJsonObject SessionHost::pendingApprovals(const QJsonObject& request) const {
    Session* session = nullptr;
    const QJsonObject failure = const_cast<SessionHost*>(this)->checkedSession(request, &session);
    if (!failure.isEmpty()) {
        return failure;
    }
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const auto after = payload.value(QStringLiteral("after_id"));
    if ((!after.isUndefined() && !after.isString()) || after.toString().size() > 128) {
        return windowError(QStringLiteral("invalid_page"),
                           QStringLiteral("Invalid approval boundary."));
    }
    QJsonObject result =
        approvals_->pendingPage(session->stored.id, session->stored.generation, after.toString());
    if (result.contains(QStringLiteral("error_code"))) {
        return windowError(result.value(QStringLiteral("error_code")).toString(),
                           QStringLiteral("A pending approval exceeds the local page limit."));
    }
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("session_id"), session->stored.id);
    result.insert(QStringLiteral("session_generation"), session->stored.generation);
    return result;
}

} // namespace ketplus::daemon
