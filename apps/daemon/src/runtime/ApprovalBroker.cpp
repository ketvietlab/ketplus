#include "runtime/ApprovalBroker.h"

#include <algorithm>

#include "protocol/DaemonProtocol.h"

#include "CodexServerRequests.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>

#include <optional>
#include <utility>

namespace ketplus::daemon {

QJsonObject approvalProviderResult(const QJsonObject& details, const bool accepted) {
    if (details.value(QStringLiteral("request_type")).toString() == QStringLiteral("mcp_tool_call"))
        return codex_requests::mcpElicitationResult(accepted);
    return {{QStringLiteral("decision"),
             accepted ? QStringLiteral("accept") : QStringLiteral("decline")}};
}

namespace {

QString stableOptionId(const QString& questionId, const int index, const QString& label) {
    return QStringLiteral("option_%1")
        .arg(QString::fromLatin1(
            QCryptographicHash::hash(
                QStringLiteral("%1\n%2\n%3").arg(questionId).arg(index).arg(label).toUtf8(),
                QCryptographicHash::Sha256)
                .toHex()
                .left(24)));
}

std::optional<QJsonArray> normalizedQuestions(const QJsonArray& source) {
    // Claude asks up to four questions at once; Codex up to three.
    if (source.isEmpty() || source.size() > 4)
        return std::nullopt;
    QJsonArray normalized;
    QSet<QString> questionIds;
    for (const QJsonValue& value : source) {
        if (!value.isObject())
            return std::nullopt;
        const QJsonObject raw = value.toObject();
        const QString id = raw.value(QStringLiteral("id")).toString().trimmed();
        const QString prompt = raw.value(QStringLiteral("question")).toString().trimmed();
        if (id.isEmpty() || id.size() > 128 || prompt.isEmpty() || prompt.size() > 4096 ||
            questionIds.contains(id)) {
            return std::nullopt;
        }
        const QJsonArray rawOptions = raw.value(QStringLiteral("options")).toArray();
        if ((rawOptions.isEmpty() && !raw.value("isOther").toBool()) || rawOptions.size() > 20)
            return std::nullopt;
        QJsonArray options;
        QSet<QString> optionIds;
        for (int optionIndex = 0; optionIndex < rawOptions.size(); ++optionIndex) {
            const QJsonValue optionValue = rawOptions.at(optionIndex);
            if (!optionValue.isObject())
                return std::nullopt;
            const QJsonObject rawOption = optionValue.toObject();
            const QString label = rawOption.value(QStringLiteral("label")).toString().trimmed();
            QString optionId = rawOption.value(QStringLiteral("id")).toString().trimmed();
            if (optionId.isEmpty())
                optionId = stableOptionId(id, optionIndex, label);
            const QString description = rawOption.value(QStringLiteral("description")).toString();
            if (label.isEmpty() || label.size() > 256 || optionId.size() > 128 ||
                description.size() > 1024 || optionIds.contains(optionId))
                return std::nullopt;
            optionIds.insert(optionId);
            options.append(QJsonObject{{QStringLiteral("id"), optionId},
                                       {QStringLiteral("label"), label},
                                       {QStringLiteral("description"), description}});
        }
        questionIds.insert(id);
        const QString selection = raw.value(QStringLiteral("selection")).toString();
        if (!selection.isEmpty() && selection != QStringLiteral("single") &&
            selection != QStringLiteral("multiple"))
            return std::nullopt;
        const bool multiple =
            selection == QStringLiteral("multiple") ||
            (selection.isEmpty() && raw.value(QStringLiteral("multiSelect")).toBool());
        const QString header = raw.value(QStringLiteral("header")).toString();
        if (header.size() > 80)
            return std::nullopt;
        normalized.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("revision"), 1},
            {QStringLiteral("header"), header},
            {QStringLiteral("prompt"), prompt},
            {QStringLiteral("selection"),
             multiple ? QStringLiteral("multiple") : QStringLiteral("single")},
            {QStringLiteral("allow_custom"), raw.value(QStringLiteral("isOther")).toBool()},
            {QStringLiteral("secret"), raw.value(QStringLiteral("isSecret")).toBool()},
            {QStringLiteral("options"), options}});
    }
    return normalized;
}

} // namespace

ApprovalBroker::ApprovalBroker(QObject* parent) : QObject(parent) {
    // Ticks only while a request can expire, so an idle daemon has no wakeups (#126).
    expiryTimer_.setInterval(250);
    connect(&expiryTimer_, &QTimer::timeout, this, &ApprovalBroker::expireRequests);
}

void ApprovalBroker::updateExpiryTimer() {
    const bool expiring =
        std::any_of(pending_.cbegin(), pending_.cend(),
                    [](const Pending& pending) { return pending.expiresAtMs > 0; });
    if (!expiring)
        expiryTimer_.stop();
    else if (!expiryTimer_.isActive())
        expiryTimer_.start();
}

QString ApprovalBroker::request(const QString& sessionId, const qint64 generation,
                                const QJsonValue& providerRequestId, const QString& kind,
                                const QJsonObject& details, const int timeoutMs) {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 expiry = timeoutMs > 0 ? QDateTime::currentMSecsSinceEpoch() + timeoutMs : 0;
    QJsonObject normalizedDetails = details;
    if (kind == QStringLiteral("input")) {
        const std::optional<QJsonArray> questions =
            normalizedQuestions(details.value(QStringLiteral("questions")).toArray());
        if (!questions.has_value()) {
            emit providerRequestRejected(
                providerRequestId, QStringLiteral("invalid_question_request"),
                QStringLiteral("The structured question request is invalid."));
            return {};
        }
        normalizedDetails = {{QStringLiteral("request_id"), id},
                             {QStringLiteral("revision"), 1},
                             {QStringLiteral("is_blocking"),
                              details.value(QStringLiteral("is_blocking")).toBool(true)},
                             {QStringLiteral("questions"), *questions}};
        const QString itemId = details.value(QStringLiteral("item_id")).toString();
        if (!itemId.isEmpty())
            normalizedDetails.insert(QStringLiteral("item_id"), itemId);
    }
    pending_.insert(id,
                    {sessionId, generation, providerRequestId, kind, expiry, normalizedDetails, 1});
    updateExpiryTimer();
    emit requested(
        id, sessionId, generation, kind, normalizedDetails,
        expiry > 0
            ? QDateTime::fromMSecsSinceEpoch(expiry, QTimeZone::UTC).toString(Qt::ISODateWithMs)
            : QString{});
    return id;
}

QJsonObject ApprovalBroker::pendingPage(const QString& sessionId, const qint64 generation,
                                        const QString& afterId) const {
    QStringList ids = pending_.keys();
    ids.sort();
    QJsonArray requests;
    qsizetype bytes = 0;
    QString nextId;
    bool hasMore = false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QString& id : ids) {
        const Pending& pending = pending_[id];
        if (id <= afterId || pending.sessionId != sessionId || pending.generation != generation ||
            (pending.expiresAtMs > 0 && pending.expiresAtMs <= now)) {
            continue;
        }
        QJsonObject request = pending.details;
        request.insert(QStringLiteral("approval_id"), id);
        request.insert(QStringLiteral("kind"), pending.kind);
        request.insert(QStringLiteral("expires_at"),
                       pending.expiresAtMs > 0
                           ? QDateTime::fromMSecsSinceEpoch(pending.expiresAtMs, QTimeZone::UTC)
                                 .toString(Qt::ISODateWithMs)
                           : QString{});
        const qsizetype requestBytes = QJsonDocument(request).toJson(QJsonDocument::Compact).size();
        if (requestBytes > 256 * 1024) {
            return {{QStringLiteral("error_code"), QStringLiteral("approval_too_large")}};
        }
        if (bytes + requestBytes > 256 * 1024 || requests.size() >= 32) {
            hasMore = true;
            break;
        }
        requests.append(request);
        bytes += requestBytes;
        nextId = id;
    }
    return {{QStringLiteral("pending_approvals"), requests},
            {QStringLiteral("pending_approvals_has_more"), hasMore},
            {QStringLiteral("pending_approvals_next_id"), nextId}};
}

ApprovalResolution ApprovalBroker::resolve(const QString& approvalId, const QString& sessionId,
                                           const qint64 generation, const QString& expectedKind,
                                           const QJsonObject& resolution) {
    auto it = pending_.find(approvalId);
    if (it == pending_.end()) {
        if (resolved_.contains(approvalId)) {
            const bool input = resolvedKinds_.value(approvalId) == QStringLiteral("input");
            return {false,
                    input ? QStringLiteral("already_answered") : QStringLiteral("already_resolved"),
                    input ? QStringLiteral("The request has already been answered.")
                          : QStringLiteral("The approval has already been resolved."),
                    {}};
        }
        return {false,
                QStringLiteral("expired"),
                QStringLiteral("The request does not exist or has expired."),
                {}};
    }
    if (it->sessionId != sessionId || it->generation != generation) {
        return {false,
                QStringLiteral("stale_generation"),
                QStringLiteral("The request belongs to a different session generation."),
                {}};
    }
    if (it->kind != expectedKind) {
        return {false,
                QStringLiteral("request_kind_conflict"),
                QStringLiteral("The command kind does not match the pending request."),
                {}};
    }
    if (it->kind == QStringLiteral("input")) {
        if (resolution.value(QStringLiteral("request_id")).toString() != approvalId ||
            resolution.value(QStringLiteral("revision")).toInt(-1) != it->revision) {
            return {false,
                    QStringLiteral("revision_conflict"),
                    QStringLiteral("The question changed before this answer arrived."),
                    {}};
        }
        const QString action = resolution.value(QStringLiteral("action")).toString();
        if (action == QStringLiteral("defer")) {
            const QString deferredKey = QStringLiteral("%1\n%2").arg(approvalId).arg(it->revision);
            if (deferred_.contains(deferredKey))
                return {true, {}, {}, QStringLiteral("already_deferred")};
            deferred_.insert(deferredKey);
            return {true, {}, {}, QStringLiteral("deferred")};
        }
        if (action != QStringLiteral("answer"))
            return {false,
                    QStringLiteral("invalid_answer"),
                    QStringLiteral("The answer action is invalid."),
                    {}};
        const QJsonArray submitted = resolution.value(QStringLiteral("answers")).toArray();
        const QJsonArray questions = it->details.value(QStringLiteral("questions")).toArray();
        if (submitted.size() != questions.size())
            return {false,
                    QStringLiteral("invalid_answer"),
                    QStringLiteral("Every question must be answered exactly once."),
                    {}};
        QHash<QString, QJsonObject> submittedById;
        for (const QJsonValue& value : submitted) {
            const QJsonObject answer = value.toObject();
            const QString questionId = answer.value(QStringLiteral("question_id")).toString();
            if (questionId.isEmpty() || submittedById.contains(questionId))
                return {false,
                        QStringLiteral("invalid_answer"),
                        QStringLiteral("Question answers must have unique identifiers."),
                        {}};
            submittedById.insert(questionId, answer);
        }
        QJsonObject providerAnswers;
        QJsonObject transcriptAnswers;
        for (const QJsonValue& value : questions) {
            const QJsonObject question = value.toObject();
            const QString questionId = question.value(QStringLiteral("id")).toString();
            const QJsonObject answer = submittedById.value(questionId);
            if (answer.value(QStringLiteral("question_revision")).toInt(-1) !=
                question.value(QStringLiteral("revision")).toInt()) {
                return {false,
                        QStringLiteral("revision_conflict"),
                        QStringLiteral("A question changed before this answer arrived."),
                        {}};
            }
            QHash<QString, QString> labels;
            for (const QJsonValue& optionValue :
                 question.value(QStringLiteral("options")).toArray()) {
                const QJsonObject option = optionValue.toObject();
                labels.insert(option.value(QStringLiteral("id")).toString(),
                              option.value(QStringLiteral("label")).toString());
            }
            QJsonArray providerValues;
            QSet<QString> selected;
            for (const QJsonValue& selectedValue :
                 answer.value(QStringLiteral("option_ids")).toArray()) {
                const QString optionId = selectedValue.toString();
                if (!labels.contains(optionId) || selected.contains(optionId))
                    return {false,
                            QStringLiteral("invalid_answer"),
                            QStringLiteral("An answer contains an invalid option."),
                            {}};
                selected.insert(optionId);
                providerValues.append(labels.value(optionId));
            }
            const QString custom =
                answer.value(QStringLiteral("custom_answer")).toString().trimmed();
            if (!custom.isEmpty()) {
                if (!question.value(QStringLiteral("allow_custom")).toBool() ||
                    custom.size() > 4096)
                    return {false,
                            QStringLiteral("invalid_answer"),
                            QStringLiteral("A custom answer is not allowed."),
                            {}};
                providerValues.append(custom);
            }
            if (providerValues.isEmpty() ||
                (question.value(QStringLiteral("selection")) == QStringLiteral("single") &&
                 providerValues.size() != 1)) {
                return {false,
                        QStringLiteral("invalid_answer"),
                        QStringLiteral("The answer selection does not match the question."),
                        {}};
            }
            QStringList displayed;
            for (const auto& answer : providerValues)
                displayed.append(answer.toString());
            transcriptAnswers.insert(questionId, question.value("secret").toBool()
                                                     ? QStringLiteral("[redacted]")
                                                     : displayed.join(QStringLiteral(", ")));
            providerAnswers.insert(questionId,
                                   QJsonObject{{QStringLiteral("answers"), providerValues}});
        }
        const Pending pending = *it;
        pending_.erase(it);
        deferred_.remove(QStringLiteral("%1\n%2").arg(approvalId).arg(pending.revision));
        rememberResolved(approvalId, pending.kind);
        emit providerResolutionReady(pending.providerRequestId,
                                     {{QStringLiteral("answers"), providerAnswers}});
        return {true, {}, {}, QStringLiteral("accepted"), transcriptAnswers, questions};
    }
    const Pending pending = *it;
    pending_.erase(it);
    rememberResolved(approvalId, pending.kind);

    QJsonObject providerResult;
    if (pending.kind == QStringLiteral("input")) {
        providerResult.insert(QStringLiteral("answers"),
                              resolution.value(QStringLiteral("answers")).toObject());
    } else {
        providerResult = approvalProviderResult(
            pending.details,
            resolution.value(QStringLiteral("decision")).toString() == QStringLiteral("accept"));
    }
    emit providerResolutionReady(pending.providerRequestId, providerResult);
    return {true, {}, {}, QStringLiteral("accepted")};
}

void ApprovalBroker::expireRequests() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->expiresAtMs == 0 || it->expiresAtMs > now) {
            ++it;
            continue;
        }
        const QString approvalId = it.key();
        const Pending pending = *it;
        it = pending_.erase(it);
        deferred_.remove(QStringLiteral("%1\n%2").arg(approvalId).arg(pending.revision));
        rememberResolved(approvalId, pending.kind);
        const QJsonObject result = pending.kind == QStringLiteral("input")
                                       ? QJsonObject{{QStringLiteral("answers"), QJsonObject{}}}
                                       : approvalProviderResult(pending.details, false);
        emit providerResolutionReady(pending.providerRequestId, result);
        emit expired(approvalId, pending.sessionId, pending.generation, pending.kind,
                     pending.revision);
    }
    // Requests answered or cancelled since the last tick are gone too.
    updateExpiryTimer();
}

void ApprovalBroker::cancelPending(const QString& sessionId, const qint64 generation) {
    // Remove first: answering a provider may synchronously finish the turn.
    QHash<QString, Pending> cancelled;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->sessionId == sessionId && it->generation == generation) {
            cancelled.insert(it.key(), *it);
            it = pending_.erase(it);
        } else
            ++it;
    }
    for (auto it = cancelled.cbegin(); it != cancelled.cend(); ++it) {
        deferred_.remove(QStringLiteral("%1\n%2").arg(it.key()).arg(it->revision));
        rememberResolved(it.key(), it->kind);
        emit providerResolutionReady(it->providerRequestId,
                                     it->kind == QStringLiteral("input")
                                         ? QJsonObject{{"answers", QJsonObject{}}}
                                         : approvalProviderResult(it->details, false));
        emit expired(it.key(), it->sessionId, it->generation, it->kind, it->revision);
    }
}

void ApprovalBroker::expireAssistantQuestions(const QString& sessionId, const qint64 generation) {
    QHash<QString, Pending> expiredQuestions;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->sessionId == sessionId && it->generation == generation &&
            it->providerRequestId.toObject().value("ketplus_async_question").toBool()) {
            expiredQuestions.insert(it.key(), *it);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = expiredQuestions.cbegin(); it != expiredQuestions.cend(); ++it) {
        deferred_.remove(QStringLiteral("%1\n%2").arg(it.key()).arg(it->revision));
        rememberResolved(it.key(), it->kind);
        emit expired(it.key(), it->sessionId, it->generation, it->kind, it->revision);
    }
}

void ApprovalBroker::abandonPending(const std::function<bool(const QString&)>& ofSession) {
    QHash<QString, Pending> abandoned;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (ofSession && !ofSession(it->sessionId)) {
            ++it;
            continue;
        }
        deferred_.remove(QStringLiteral("%1\n%2").arg(it.key()).arg(it->revision));
        abandoned.insert(it.key(), *it);
        it = pending_.erase(it);
    }
    for (auto it = abandoned.cbegin(); it != abandoned.cend(); ++it) {
        rememberResolved(it.key(), it->kind);
        emit expired(it.key(), it->sessionId, it->generation, it->kind, it->revision);
    }
}

void ApprovalBroker::rememberResolved(const QString& approvalId, const QString& kind) {
    resolved_.insert(approvalId);
    resolvedKinds_.insert(approvalId, kind);
    resolvedOrder_.append(approvalId);
    constexpr qsizetype maximumRememberedApprovals = 1024;
    while (resolvedOrder_.size() > maximumRememberedApprovals) {
        const QString forgotten = resolvedOrder_.takeFirst();
        resolved_.remove(forgotten);
        resolvedKinds_.remove(forgotten);
    }
}

} // namespace ketplus::daemon
