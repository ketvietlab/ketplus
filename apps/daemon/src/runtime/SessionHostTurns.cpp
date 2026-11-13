#include "runtime/SessionHost.h"

#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"
#include "runtime/ApprovalBroker.h"
#include "runtime/CompactWindowPolicy.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

namespace ketplus::daemon {
namespace {

// ADR 0067: a review of another conversation's turn is read-only whatever the reviewer's mode.
bool reviewGuard(const QJsonObject& options) {
    return options.value(QStringLiteral("review_guard")).toBool();
}

QString sandboxName(const QJsonObject& options) {
    if (reviewGuard(options))
        return QStringLiteral("read-only");
    if (options.value(QStringLiteral("execution_policy")).toString() == QStringLiteral("full"))
        return QStringLiteral("danger-full-access");
    if (options.value("execution_policy").toString() == "read-only")
        return QStringLiteral("read-only");
    const QString permission = options.value(QStringLiteral("permission")).toString();
    return permission == QStringLiteral("read-only") ||
                   permission == QStringLiteral("danger-full-access")
               ? permission
               : QStringLiteral("workspace-write");
}

QString approvalPolicy(const QJsonObject& options) {
    // Defence in depth: reviews only reach Claude (AgentBroker::enforcesReviewGuard).
    if (reviewGuard(options))
        return QStringLiteral("never");
    return options.value(QStringLiteral("execution_policy")).toString() == QStringLiteral("full")
               ? QStringLiteral("never")
               : QStringLiteral("on-request");
}

QString sandboxType(const QJsonObject& options) {
    const QString permission = sandboxName(options);
    return permission == QStringLiteral("read-only")            ? QStringLiteral("readOnly")
           : permission == QStringLiteral("danger-full-access") ? QStringLiteral("dangerFullAccess")
                                                                : QStringLiteral("workspaceWrite");
}

} // namespace

// Every text item of the turn, in order; attachments arrive as further items.
QString SessionHost::promptText(const QJsonArray& input) {
    QStringList parts;
    for (const QJsonValue& value : input) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
            const QString text = item.value(QStringLiteral("text")).toString();
            if (!text.isEmpty())
                parts.append(text);
        }
    }
    return parts.join(QStringLiteral("\n\n"));
}

QJsonArray SessionHost::imagePaths(const QJsonArray& input) {
    QJsonArray paths;
    for (const QJsonValue& value : input) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("type")).toString() == QStringLiteral("localImage"))
            paths.append(item.value(QStringLiteral("path")).toString());
    }
    return paths;
}

// ADR 0027: Codex receives the agent tools as per-thread config beside the compact window, with
// its routing in developerInstructions (the browser policy is appended after it).
void SessionHost::addCodexAgentTools(Session& session, QJsonObject& params, QJsonObject config) {
    if (const auto launch = agentLaunch(session.stored.id, session.stored.generation)) {
        const QJsonObject agentConfig = agent_tools::codexConfig(*launch);
        for (auto entry = agentConfig.constBegin(); entry != agentConfig.constEnd(); ++entry)
            config.insert(entry.key(), entry.value());
        if (const QString instructions = agentInstructions(session); !instructions.isEmpty())
            params.insert(QStringLiteral("developerInstructions"), instructions);
    }
    if (!config.isEmpty())
        params.insert(QStringLiteral("config"), config);
}

void SessionHost::prepareTurn(const QString& sessionId, const qint64 generation,
                              const QJsonArray& input, const QJsonObject& options) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || it->stored.generation != generation) {
        return;
    }
    noteProviderRuntime(sessionId, generation);
    const qint64 compactWindow = options.value(QStringLiteral("compactWindow")).toInteger();
    if (it->stored.adapter == QStringLiteral("claude")) {
        beginClaudeTurn(sessionId, generation, input, options);
        return;
    }
    if (it->stored.adapter == QStringLiteral("cursor") ||
        it->stored.adapter == QStringLiteral("copilot")) {
        beginAcpTurn(sessionId, generation, input, options);
        return;
    }
    if (it->stored.threadId.isEmpty()) {
        QJsonObject params{{QStringLiteral("cwd"), executionDirectory(*it)},
                           {QStringLiteral("sandbox"), sandboxName(options)},
                           {QStringLiteral("approvalPolicy"), approvalPolicy(options)},
                           {QStringLiteral("approvalsReviewer"), QStringLiteral("user")},
                           {QStringLiteral("ephemeral"), false}};
        const QString model = options.value(QStringLiteral("model")).toString();
        if (!model.isEmpty()) {
            params.insert(QStringLiteral("model"), model);
        }
        addCodexAgentTools(*it, params, context_harness::codexCompactConfig(compactWindow));
        adapter_->call(
            QStringLiteral("thread/start"), params,
            [this, sessionId, generation, input, options,
             compactWindow](const QJsonObject& result, const QString& providerError) {
                auto current = sessions_.find(sessionId);
                if (current == sessions_.end() || current->stored.generation != generation) {
                    return;
                }
                if (!providerError.isEmpty()) {
                    failTurnStart(*current, providerError);
                    return;
                }
                current->stored.threadId = result.value(QStringLiteral("thread"))
                                               .toObject()
                                               .value(QStringLiteral("id"))
                                               .toString();
                if (current->stored.threadId.isEmpty()) {
                    failTurnStart(*current, QStringLiteral("Codex returned no thread id."));
                    return;
                }
                current->attachedToProvider = true;
                current->providerCompactWindow = compactWindow;
                sessionsByThread_.insert(current->stored.threadId, sessionId);
                journal_->upsertSession(current->stored);
                syncProviderTitle(*current);
                beginTurn(sessionId, generation, input, options);
            });
        return;
    }
    if (it->attachedToProvider && it->providerCompactWindow == compactWindow) {
        beginTurn(sessionId, generation, input, options);
        return;
    }
    QJsonObject params{{QStringLiteral("threadId"), it->stored.threadId},
                       {QStringLiteral("cwd"), executionDirectory(*it)},
                       {QStringLiteral("sandbox"), sandboxName(options)},
                       {QStringLiteral("approvalPolicy"), approvalPolicy(options)},
                       {QStringLiteral("approvalsReviewer"), QStringLiteral("user")},
                       {QStringLiteral("excludeTurns"), true}};
    addCodexAgentTools(*it, params, context_harness::codexCompactConfig(compactWindow));
    adapter_->call(QStringLiteral("thread/resume"), params,
                   [this, sessionId, generation, input, options,
                    compactWindow](const QJsonObject&, const QString& providerError) {
                       auto current = sessions_.find(sessionId);
                       if (current == sessions_.end() || current->stored.generation != generation) {
                           return;
                       }
                       if (!providerError.isEmpty()) {
                           failTurnStart(*current, providerError);
                           return;
                       }
                       current->attachedToProvider = true;
                       current->providerCompactWindow = compactWindow;
                       syncProviderTitle(*current);
                       beginTurn(sessionId, generation, input, options);
                   });
}

void SessionHost::beginClaudeTurn(const QString& sessionId, const qint64 generation,
                                  const QJsonArray& input, const QJsonObject& options) {
    auto it = sessions_.find(sessionId);
    ProviderAdapter* const claude = provider(QStringLiteral("claude"));
    if (it == sessions_.end() || it->stored.generation != generation || claude == nullptr) {
        return;
    }
    const QString prompt = promptText(input);
    if (prompt.isEmpty()) {
        failTurnStart(*it, QStringLiteral("Claude turn has no prompt."));
        return;
    }
    QJsonObject params{{QStringLiteral("sessionId"), sessionId},
                       {QStringLiteral("cwd"), executionDirectory(*it)},
                       {QStringLiteral("prompt"), prompt},
                       // Full access chosen for the conversation (from the desktop or a phone)
                       // reaches the agent as its prompt-free mode, not only Codex's sandbox.
                       {QStringLiteral("permission"), sandboxName(options)},
                       {QStringLiteral("images"), imagePaths(input)}};
    if (reviewGuard(options)) {
        params.insert(QStringLiteral("reviewGuard"), true);
        // ADR 0114 §2: a team's review round verifies; anything else stays read-only.
        const QString scratch = reviewScratchDirectory(sessionId);
        if (options.value(QStringLiteral("review_profile")).toString() ==
                QStringLiteral("verify") &&
            QDir().mkpath(scratch)) {
            params.insert(QStringLiteral("reviewProfile"), QStringLiteral("verify"));
            params.insert(QStringLiteral("scratchDirectory"),
                          QFileInfo(scratch).canonicalFilePath());
        } else {
            params.insert(QStringLiteral("reviewProfile"), QStringLiteral("read"));
        }
    }
    params.insert(QStringLiteral("freshContext"), it->stored.threadId.isEmpty());
    // The handoff transcript lives outside the worktree; Claude may read it without asking.
    const QString handoff = handoffDirectory(sessionId);
    if (QFileInfo(handoff).isDir())
        params.insert(QStringLiteral("readableDirectories"), QJsonArray{handoff});
    for (const QString& key : {QStringLiteral("model"), QStringLiteral("effort")}) {
        const QString value = options.value(key).toString();
        if (!value.isEmpty())
            params.insert(key, value);
    }
    if (const auto window = options.value(QStringLiteral("compactWindow")).toInteger(); window > 0)
        params.insert(QStringLiteral("compactWindow"), window);
    if (!it->stored.threadId.isEmpty())
        params.insert(QStringLiteral("resume"), it->stored.threadId);
    if (const auto launch = agentLaunch(sessionId, generation)) {
        params.insert(QStringLiteral("agentMcpServer"), agent_tools::claudeServer(*launch));
        params.insert(QStringLiteral("agentInstructions"), agentInstructions(*it));
    }
    claude->call(
        QStringLiteral("turn/start"), params,
        [this, sessionId, generation](const QJsonObject& result, const QString& providerError) {
            auto current = sessions_.find(sessionId);
            if (current == sessions_.end() || current->stored.generation != generation) {
                return;
            }
            current->startingTurn = false;
            if (!providerError.isEmpty()) {
                failTurnStart(*current, providerError);
                return;
            }
            // The provider has the turn, and with it the handoff note.
            current->handoffFrom.clear();
            const QString threadId = result.value(QStringLiteral("thread"))
                                         .toObject()
                                         .value(QStringLiteral("id"))
                                         .toString();
            if (!threadId.isEmpty()) {
                if (!current->stored.threadId.isEmpty())
                    sessionsByThread_.remove(current->stored.threadId);
                current->stored.threadId = threadId;
                sessionsByThread_.insert(threadId, sessionId);
            }
            current->attachedToProvider = true;
            if (current->stored.state != QStringLiteral("starting"))
                return;
            current->stored.turnId = result.value(QStringLiteral("turn"))
                                         .toObject()
                                         .value(QStringLiteral("id"))
                                         .toString();
            current->stored.state = QStringLiteral("running");
            journal_->upsertSession(current->stored);
        });
}

void SessionHost::beginAcpTurn(const QString& sessionId, const qint64 generation,
                               const QJsonArray& input, const QJsonObject& options) {
    auto it = sessions_.find(sessionId);
    ProviderAdapter* const target = it == sessions_.end() ? nullptr : provider(it->stored.adapter);
    if (it == sessions_.end() || it->stored.generation != generation || target == nullptr)
        return;
    const QString prompt = promptText(input);
    if (prompt.isEmpty()) {
        failTurnStart(*it, QStringLiteral("%1 turn has no prompt.")
                               .arg(it->stored.adapter == QStringLiteral("copilot")
                                        ? QStringLiteral("GitHub Copilot")
                                        : QStringLiteral("Cursor")));
        return;
    }
    QJsonObject params{{QStringLiteral("sessionId"), sessionId},
                       {QStringLiteral("cwd"), executionDirectory(*it)},
                       {QStringLiteral("prompt"), prompt},
                       // Full access chosen for the conversation (from the desktop or a phone)
                       // reaches the agent as its prompt-free mode, not only Codex's sandbox.
                       {QStringLiteral("permission"), sandboxName(options)},
                       {QStringLiteral("images"), imagePaths(input)}};
    for (const QString& key : {QStringLiteral("model"), QStringLiteral("effort")}) {
        const QString value = options.value(key).toString();
        if (!value.isEmpty())
            params.insert(key, value);
    }
    if (!it->stored.threadId.isEmpty())
        params.insert(QStringLiteral("resume"), it->stored.threadId);
    if (const auto launch = agentLaunch(sessionId, generation)) {
        params.insert(QStringLiteral("agentMcpServer"), agent_tools::acpServer(*launch));
        params.insert(QStringLiteral("agentInstructions"), agentInstructions(*it));
    }
    target->call(
        QStringLiteral("turn/start"), params,
        [this, sessionId, generation](const QJsonObject& result, const QString& providerError) {
            auto current = sessions_.find(sessionId);
            if (current == sessions_.end() || current->stored.generation != generation)
                return;
            current->startingTurn = false;
            if (!providerError.isEmpty()) {
                failTurnStart(*current, providerError);
                return;
            }
            // The provider has the turn, and with it the handoff note.
            current->handoffFrom.clear();
            const QString threadId = result.value(QStringLiteral("thread"))
                                         .toObject()
                                         .value(QStringLiteral("id"))
                                         .toString();
            if (!threadId.isEmpty()) {
                if (!current->stored.threadId.isEmpty())
                    sessionsByThread_.remove(current->stored.threadId);
                current->stored.threadId = threadId;
                sessionsByThread_.insert(threadId, sessionId);
            }
            current->attachedToProvider = true;
            if (current->stored.state != QStringLiteral("starting"))
                return;
            current->stored.turnId = result.value(QStringLiteral("turn"))
                                         .toObject()
                                         .value(QStringLiteral("id"))
                                         .toString();
            current->stored.state = QStringLiteral("running");
            journal_->upsertSession(current->stored);
        });
}

void SessionHost::beginTurn(const QString& sessionId, const qint64 generation,
                            const QJsonArray& input, const QJsonObject& options) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || it->stored.generation != generation) {
        return;
    }
    QJsonObject params{{QStringLiteral("threadId"), it->stored.threadId},
                       {QStringLiteral("cwd"), executionDirectory(*it)},
                       {QStringLiteral("approvalPolicy"), approvalPolicy(options)},
                       {QStringLiteral("sandboxPolicy"),
                        QJsonObject{{QStringLiteral("type"), sandboxType(options)}}},
                       {QStringLiteral("input"), input}};
    for (const QString& key : {QStringLiteral("model"), QStringLiteral("effort")}) {
        const QString value = options.value(key).toString();
        if (!value.isEmpty()) {
            params.insert(key, value);
        }
    }
    adapter_->call(
        QStringLiteral("turn/start"), params,
        [this, sessionId, generation](const QJsonObject& result, const QString& providerError) {
            auto current = sessions_.find(sessionId);
            if (current == sessions_.end() || current->stored.generation != generation) {
                return;
            }
            current->startingTurn = false;
            if (!providerError.isEmpty()) {
                failTurnStart(*current, providerError);
                return;
            }
            // The provider has the turn, and with it the handoff note.
            current->handoffFrom.clear();
            // turn/completed or a provider exit can arrive before this reply. Only a turn that is
            // still starting may become running; otherwise the session would stay busy forever.
            if (current->stored.state != QStringLiteral("starting"))
                return;
            current->stored.turnId = result.value(QStringLiteral("turn"))
                                         .toObject()
                                         .value(QStringLiteral("id"))
                                         .toString();
            current->stored.state = QStringLiteral("running");
            journal_->upsertSession(current->stored);
        });
}

void SessionHost::abandonTurnApprovals(const QString& sessionId) {
    // The provider stops waiting for a turn's approvals and questions when the turn ends. An
    // answer given afterwards would go to a request it has forgotten, so they expire before the
    // completion is published and a late answer is refused.
    forgetProviderRoutes(
        approvals_->abandonPending([&sessionId](const QString& id) { return id == sessionId; }));
}

void SessionHost::failTurnStart(Session& session, const QString& message) {
    session.startingTurn = false;
    // A Stop pressed while this turn was starting belongs to it; kept, it would interrupt the
    // next turn as soon as it starts and keep the worktree switch unavailable until then.
    session.interruptPending = false;
    releaseTurnAttachments(session);
    session.stored.state = QStringLiteral("failed");
    journal_->upsertSession(session.stored);
    // The turn never ran and no turn.completed follows: said on the event, with the turn,
    // so the broker ends the requests that turn carried (ADR 0114 §8).
    appendEvent(session, QStringLiteral("runtime.error"),
                {{QStringLiteral("message"), message},
                 {QStringLiteral("turn_start_failed"), true},
                 {QStringLiteral("turn_sequence"), session.stored.currentTurnSequence}});
}

void SessionHost::providerStopped(const QString& adapterName, const QString& reason) {
    // Pending approvals carry the exited process's request ids, which the next process reuses.
    // Only that provider's approvals go: another provider's process is still waiting on its own.
    approvals_->abandonPending([this, &adapterName](const QString& sessionId) {
        const auto it = sessions_.constFind(sessionId);
        return it == sessions_.cend() || it->stored.adapter == adapterName;
    });
    for (auto it = pendingProviderByRequest_.begin(); it != pendingProviderByRequest_.end();) {
        if (it.value() == adapterName)
            it = pendingProviderByRequest_.erase(it);
        else
            ++it;
    }
    for (Session& session : sessions_) {
        if (session.stored.adapter != adapterName)
            continue;
        // Turn ids are per provider process; the next process may reuse them.
        session.completedTurnId.clear();
        session.failedTurnId.clear();
        session.completedItems.clear();
        session.completedItemOrder.clear();
        endBackgroundTasks(session, QStringLiteral("worker_restart"));
        if (!session.attachedToProvider && !session.startingTurn &&
            session.stored.turnId.isEmpty()) {
            continue;
        }
        session.attachedToProvider = false;
        session.startingTurn = false;
        session.interruptPending = false;
        // The provider thread and its turn are gone: clear the turn and release its attachments,
        // as turn/completed would, so interrupts and attachment quotas do not stay stuck.
        session.stored.turnId.clear();
        session.stored.state = QStringLiteral("offline");
        releaseTurnAttachments(session);
        journal_->upsertSession(session.stored);
        appendEvent(session, QStringLiteral("runtime.stopped"),
                    {{QStringLiteral("message"), reason}});
    }
}

void SessionHost::providerReloaded(const QString& reason) {
    Q_UNUSED(reason)
    // A planned reload (browser settings) only happens while no turn, call or approval is
    // outstanding. Sessions keep their state and journal: nothing failed, the thread is simply
    // resumed in the new process on the next turn.
    for (Session& session : sessions_) {
        if (session.stored.adapter != QStringLiteral("codex"))
            continue;
        if (session.stored.state == QStringLiteral("running") || session.startingTurn) {
            // Not idle after all (fail-safe): treat as a provider stop for this session.
            providerStopped(QStringLiteral("codex"), QStringLiteral("Codex app-server restarted."));
            return;
        }
    }
    for (Session& session : sessions_) {
        if (session.stored.adapter == QStringLiteral("codex")) {
            session.attachedToProvider = false;
            session.completedTurnId.clear();
            session.failedTurnId.clear();
            session.completedItems.clear();
            session.completedItemOrder.clear();
        }
    }
}

QJsonObject SessionHost::turnCompletion(Session& session, const QString& status) {
    QJsonObject completed{{QStringLiteral("status"), status}};
    // Only an interrupted end is the Stop's; a turn that finished first keeps its own status.
    if (std::exchange(session.stopRequested, false) &&
        (status == QStringLiteral("interrupted") || status == QStringLiteral("stopped")))
        completed.insert(QStringLiteral("reason"), QStringLiteral("user"));
    // The end row shows the turn's report even when a history page splits it from the turn's
    // work_report.updated event.
    const QJsonObject report = journal_->workReport(session.stored.id, session.stored.generation);
    if (!report.isEmpty() &&
        report.value(QStringLiteral("turn")).toInteger() == session.stored.currentTurnSequence)
        completed.insert(QStringLiteral("work_report"), report);
    // #125: a finished turn whose answer asks the user to decide ends as Waiting for you.
    const bool finished = status.isEmpty() || status == QStringLiteral("completed") ||
                          status == QStringLiteral("ready");
    const bool awaiting = finished && endsWaitingForUser(session);
    setAwaitingReply(session, awaiting);
    if (awaiting)
        completed.insert(QStringLiteral("awaiting_reply"), true);
    session.lastAnswerText.clear();
    session.decisionRequested = false;
    return completed;
}

QString SessionHost::reviewScratchDirectory(const QString& sessionId) const {
    return QFileInfo(journal_->databasePath())
        .dir()
        .filePath(QStringLiteral("review-scratch/%1").arg(sessionId));
}

} // namespace ketplus::daemon
