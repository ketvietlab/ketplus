#include "runtime/SessionHost.h"

#include <QDateTime>
#include <QJsonArray>
#include <QUuid>

namespace ketplus::daemon {
namespace {
QJsonObject failure(const QString& code) {
    return {{"ok", false},
            {"error_code", code},
            {"error_message", "The prepared conversation is not available."}};
}
} // namespace

QJsonObject SessionHost::authorizePreparation(const QJsonObject& request) const {
    const auto id = request.value("session_id").toString();
    const auto preparation = journal_->sessionPreparation(id);
    if (!preparation || (preparation->state == "activated" && sessions_.contains(id)))
        return {};
    if (preparation->owner != request.value("_preparation_owner").toString())
        return failure("preparation_not_found");
    if (preparation->state != "prepared")
        return failure("preparation_consumed");
    if (preparation->expiresAt <= QDateTime::currentMSecsSinceEpoch())
        return failure("preparation_expired");
    if (normalizedWorktree(preparation->worktreePath) != preparation->worktreePath)
        return failure("preparation_scope_changed");
    const auto type = request.value("message_type").toString();
    if (type != "session.start_turn" && type != "session.prepare.cancel" &&
        type != "attachment.stage.begin" && type != "attachment.stage.chunk" &&
        type != "attachment.stage.commit" && type != "attachment.stage.cancel")
        return failure("preparation_inactive");
    if (request.value("session_generation").toInteger(-1) != 1)
        return failure("stale_generation");
    return {};
}

QJsonObject SessionHost::prepareSession(const QJsonObject& request) {
    const auto owner = request.value("_preparation_owner").toString();
    if (owner.isEmpty())
        return failure("preparation_not_found");
    const auto payload = request.value("payload").toObject();
    const auto worktree = normalizedWorktree(payload.value("worktree_path").toString());
    if (worktree.isEmpty())
        return failure("invalid_worktree");
    auto options = payload.value("options").toObject();
    const auto provider = options.value("provider").toString("codex");
    if (provider != "codex" && provider != "claude" && provider != "cursor")
        return failure("provider_unavailable");
    options.insert("provider", provider);
    SessionPreparation preparation{QUuid::createUuid().toString(QUuid::WithoutBraces),
                                   owner,
                                   payload.value("client_request_id").toString(),
                                   worktree,
                                   options,
                                   QDateTime::currentMSecsSinceEpoch() + sessionPreparationTtlMs};
    QString error;
    if (!journal_->prepareSession(preparation, &error))
        return failure(error);
    if (preparation.state != "prepared")
        return failure("preparation_consumed");
    if (preparation.expiresAt <= QDateTime::currentMSecsSinceEpoch())
        return failure("preparation_expired");
    return {{"ok", true},
            {"session_id", preparation.sessionId},
            {"session_generation", 1},
            {"expires_at_ms", preparation.expiresAt},
            {"state", "prepared"}};
}

QJsonObject SessionHost::cancelPreparedSession(const QJsonObject& request) {
    if (request.value("session_generation").toInteger(-1) != 1)
        return failure("stale_generation");
    QString error;
    const auto id = request.value("session_id").toString();
    if (!journal_->discardPreparedSession(id, request.value("_preparation_owner").toString(),
                                          QDateTime::currentMSecsSinceEpoch(), &error))
        return failure(error);
    preparedSessions_.remove(id);
    return {{"ok", true}, {"discarded", true}};
}

QJsonObject SessionHost::checkedAttachmentSession(const QJsonObject& request, Session** session) {
    const auto id = request.value("session_id").toString();
    if (sessions_.contains(id))
        return checkedSession(request, session);
    const auto preparation = journal_->sessionPreparation(id);
    if (!preparation)
        return failure("session_not_found");
    if (const auto result = authorizePreparation(request); !result.isEmpty())
        return result;
    auto& context = preparedSessions_[id];
    context.stored.id = id;
    context.stored.worktreePath = preparation->worktreePath;
    context.stored.adapter = preparation->options.value("provider").toString("codex");
    context.stored.state = "prepared";
    context.stored.generation = 1;
    *session = &context;
    return {};
}

void SessionHost::expirePreparedSessions() {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    journal_->pruneSessionPreparations(now);
    int remaining = 128;
    for (const auto& preparation : journal_->sessionPreparations()) {
        if (preparation.expiresAt > now || remaining-- <= 0)
            break;
        if (journal_->discardPreparedSession(preparation.sessionId, preparation.owner, now))
            preparedSessions_.remove(preparation.sessionId);
    }
}
} // namespace ketplus::daemon
