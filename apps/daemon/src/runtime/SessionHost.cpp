#include "runtime/SessionHost.h"

#include "agents/AgentInstructions.h"
#include "runtime/CodexItems.h"
#include "runtime/CompactWindowPolicy.h"

#include "agent/ConversationTitle.h"
#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"
#include "providers/compat/ProviderCapabilities.h"
#include "runtime/ApprovalBroker.h"
#include "runtime/ProviderHandoff.h"

#include "CodexBrowserTools.h"
#include "CodexServerRequests.h"
#include "ComputerToolLaunch.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace ketplus::daemon {
namespace {

QString modelCatalogRevision(const QJsonArray& models) {
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(models).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256)
            .toHex());
}

constexpr qsizetype maximumPromptBytes = 256 * 1024;
constexpr qsizetype maximumTitleBytes = 4 * 1024;

QJsonObject ok(QJsonObject values = {}) {
    values.insert(QStringLiteral("ok"), true);
    return values;
}

QJsonObject error(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

QString threadIdFrom(const QJsonObject& params) {
    QString threadId = params.value(QStringLiteral("threadId")).toString();
    if (threadId.isEmpty()) {
        threadId = params.value(QStringLiteral("thread"))
                       .toObject()
                       .value(QStringLiteral("id"))
                       .toString();
    }
    return threadId;
}

QString requestKey(const QJsonValue& requestId) {
    if (requestId.isString())
        return requestId.toString();
    return QString::number(requestId.toVariant().toLongLong());
}

QJsonObject tokenUsageFromNotification(const QString& method, const QJsonObject& params) {
    if (method == QStringLiteral("thread/tokenUsage/updated")) {
        QJsonObject usage = params.value(QStringLiteral("tokenUsage")).toObject();
        return usage.isEmpty() ? params.value(QStringLiteral("token_usage")).toObject() : usage;
    }
    if (method != QStringLiteral("codex/event/token_count"))
        return {};
    QJsonObject info = params.value(QStringLiteral("msg")).toObject().value("info").toObject();
    if (info.isEmpty())
        info = params.value(QStringLiteral("info")).toObject();
    const QJsonObject last = info.value(QStringLiteral("last_token_usage")).toObject();
    if (last.isEmpty() && !info.contains(QStringLiteral("model_context_window")))
        return {};
    return {
        {QStringLiteral("last"),
         QJsonObject{
             {QStringLiteral("totalTokens"), last.value(QStringLiteral("total_tokens"))},
             {QStringLiteral("inputTokens"), last.value(QStringLiteral("input_tokens"))},
             {QStringLiteral("outputTokens"), last.value(QStringLiteral("output_tokens"))}}},
        {QStringLiteral("modelContextWindow"), info.value(QStringLiteral("model_context_window"))}};
}

QJsonObject occupancyTokenUsage(const QJsonObject& payload) {
    QJsonObject usage = payload.value(QStringLiteral("tokenUsage")).toObject();
    if (usage.isEmpty())
        usage = payload.value(QStringLiteral("token_usage")).toObject();
    if (usage.isEmpty())
        usage = payload.value(QStringLiteral("usage")).toObject();
    const QJsonObject last = usage.value(QStringLiteral("last")).toObject();
    qint64 used = last.value(QStringLiteral("totalTokens"))
                      .toInteger(last.value(QStringLiteral("total_tokens")).toInteger(-1));
    qint64 limit = usage.value(QStringLiteral("modelContextWindow"))
                       .toInteger(usage.value(QStringLiteral("model_context_window")).toInteger(0));
    if (used < 0) {
        used = payload.value(QStringLiteral("used"))
                   .toInteger(usage.value(QStringLiteral("used")).toInteger(-1));
    }
    if (limit <= 0) {
        limit = payload.value(QStringLiteral("size"))
                    .toInteger(usage.value(QStringLiteral("size")).toInteger(0));
    }
    if (used < 0 || limit <= 0)
        return {};
    return {{QStringLiteral("last"), QJsonObject{{QStringLiteral("totalTokens"), used}}},
            {QStringLiteral("modelContextWindow"), limit}};
}

// Plan rows arrive from a provider sidecar, so the journal stores only the fields
// the desktop renders, each one range-checked here rather than in the view.
QJsonArray planLimitRows(const QJsonObject& payload) {
    QJsonArray rows;
    const QJsonArray incoming = payload.value(QStringLiteral("limits")).toArray();
    for (const QJsonValue& value : incoming) {
        const QJsonObject row = value.toObject();
        const QString id = row.value(QStringLiteral("id")).toString();
        const QString label = row.value(QStringLiteral("label")).toString();
        const QJsonValue used = row.value(QStringLiteral("usedPercent"));
        if (id.isEmpty() || label.isEmpty() || !used.isDouble())
            continue;
        const double percent = used.toDouble();
        if (!std::isfinite(percent) || percent < 0)
            continue;
        rows.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("label"), label},
            {QStringLiteral("usedPercent"), qMin(100.0, percent)},
            {QStringLiteral("resetsAt"), row.value(QStringLiteral("resetsAt")).toInteger(0)},
            {QStringLiteral("available"), true}});
    }
    return rows;
}

// Every provider numbers its JSON-RPC requests independently, so a bare id is not unique across
// adapters. Approvals travel through the broker under an adapter-qualified id and are untagged
// again before the answer goes back to the provider that asked.
QJsonValue taggedRequestId(const QString& adapterName, const QJsonValue& requestId) {
    return QStringLiteral("%1\x1f%2\x1f%3")
        .arg(adapterName, requestId.isString() ? QStringLiteral("s") : QStringLiteral("n"),
             requestKey(requestId));
}

bool untagRequestId(const QJsonValue& tagged, QString& adapterName, QJsonValue& requestId) {
    if (!tagged.isString())
        return false;
    const QStringList parts = tagged.toString().split(QLatin1Char('\x1f'));
    if (parts.size() != 3)
        return false;
    adapterName = parts.at(0);
    requestId = parts.at(1) == QStringLiteral("s") ? QJsonValue(parts.at(2))
                                                   : QJsonValue(parts.at(2).toLongLong());
    return true;
}

QString requestedAdapter(const QJsonObject& options, const QString& fallback) {
    const QString provider = options.value(QStringLiteral("provider")).toString().trimmed();
    if (provider == QStringLiteral("claude"))
        return QStringLiteral("claude");
    if (provider == QStringLiteral("cursor"))
        return QStringLiteral("cursor");
    if (provider.isEmpty())
        return fallback.isEmpty() ? QStringLiteral("codex") : fallback;
    if (provider == QStringLiteral("codex"))
        return QStringLiteral("codex");
    return provider;
}

// Commands relayed from the phone arrive under this client id; every other client runs on the Mac.
bool localClient(const QString& clientId) { return clientId != QStringLiteral("cloud-control-v2"); }

// Appends each provider's model list to `merged` in order; a provider that fails adds nothing.
void appendModelLists(QList<ProviderAdapter*> providers, QJsonArray merged,
                      std::function<void(const QJsonArray&)> done) {
    if (providers.isEmpty()) {
        done(merged);
        return;
    }
    ProviderAdapter* const next = providers.takeFirst();
    next->call(QStringLiteral("model/list"), {},
               [providers = std::move(providers), merged = std::move(merged),
                done = std::move(done)](const QJsonObject& result, const QString&) mutable {
                   for (const QJsonValue& value : result.value(QStringLiteral("data")).toArray())
                       merged.append(value);
                   appendModelLists(std::move(providers), std::move(merged), std::move(done));
               });
}

bool isCanonicalAdapter(const QString& adapter) {
    return adapter == QStringLiteral("codex") || adapter == QStringLiteral("claude") ||
           adapter == QStringLiteral("cursor") || adapter == QStringLiteral("copilot");
}

// What a tool call will run or touch, for its approval card.
QString toolCallSummary(const QString& tool, const QJsonObject& input) {
    constexpr qsizetype maximumSummaryCharacters = 4000;
    QString summary;
    for (const QString& key :
         {QStringLiteral("command"), QStringLiteral("file_path"), QStringLiteral("notebook_path"),
          QStringLiteral("url"), QStringLiteral("pattern")}) {
        const QString value = input.value(key).toString();
        if (!value.isEmpty()) {
            summary = value;
            break;
        }
    }
    const QString details = QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Indented));
    if (summary.isEmpty())
        summary = details;
    else if (input.size() > 1)
        summary = QStringLiteral("%1\n\n%2").arg(summary, details);
    summary = QStringLiteral("%1: %2").arg(tool, summary);
    if (summary.size() > maximumSummaryCharacters)
        summary = summary.left(maximumSummaryCharacters) + QStringLiteral("…");
    return summary;
}

// Each agent's catalog entries name that agent, so a merged catalog can be narrowed again.
QJsonArray taggedCatalog(const QJsonArray& catalog, const QString& provider) {
    QJsonArray tagged;
    for (const QJsonValue& value : catalog) {
        QJsonObject entry = value.toObject();
        entry.insert(QStringLiteral("provider"), provider);
        tagged.append(entry);
    }
    return tagged;
}

// A conversation's settings offer only its own agent's models: turns run on that agent, and
// two agents can list the same id (a Cursor family named after a Codex model).
QJsonArray catalogForAdapter(const QJsonArray& catalog, const QString& adapter) {
    const QString provider = adapter.isEmpty() ? QStringLiteral("codex") : adapter;
    QJsonArray own;
    for (const QJsonValue& value : catalog) {
        if (value.toObject().value(QStringLiteral("provider")).toString(QStringLiteral("codex")) ==
            provider)
            own.append(value);
    }
    return own;
}

// Every agent's models a conversation can run on: a new one picks its agent, and a started one
// can move to another between turns, as the desktop's picker tabs offer.
QJsonArray agentCatalog(const QJsonArray& catalog) {
    QJsonArray models;
    for (const QJsonValue& value : catalog) {
        QJsonObject model = value.toObject();
        if (model.value(QStringLiteral("hidden")).toBool())
            continue;
        if (!model.contains(QStringLiteral("provider")))
            model.insert(QStringLiteral("provider"), QStringLiteral("codex"));
        models.append(model);
    }
    return models;
}

void tagCodexModels(QJsonObject& result) {
    QJsonArray data;
    for (const QJsonValue& value : result.value(QStringLiteral("data")).toArray()) {
        QJsonObject model = value.toObject();
        if (!model.contains(QStringLiteral("provider")))
            model.insert(QStringLiteral("provider"), QStringLiteral("codex"));
        data.append(model);
    }
    result.insert(QStringLiteral("data"), data);
}

// The settings a session.configuration_changed event changed, so readers can word it.
QJsonArray changedSettings(const StoredSession& before, const StoredSession& after) {
    QJsonArray changed;
    const auto note = [&changed](const bool differs, const QString& name) {
        if (differs)
            changed.append(name);
    };
    note(before.model != after.model, QStringLiteral("model"));
    note(before.effort != after.effort, QStringLiteral("effort"));
    note(before.executionPolicy != after.executionPolicy, QStringLiteral("execution_policy"));
    note(before.mode != after.mode, QStringLiteral("mode"));
    note(before.compactWindow != after.compactWindow, QStringLiteral("compact_window"));
    return changed;
}

} // namespace

SessionHost::SessionHost(LocalJournal* journal, CodexAdapter* adapter, QObject* parent)
    : SessionHost(journal, adapter, nullptr, nullptr, parent) {}

SessionHost::SessionHost(LocalJournal* journal, CodexAdapter* adapter, ClaudeAdapter* claude,
                         QObject* parent)
    : SessionHost(journal, adapter, claude, nullptr, parent) {}

SessionHost::SessionHost(LocalJournal* journal, CodexAdapter* adapter, ClaudeAdapter* claude,
                         CursorAdapter* cursor, QObject* parent)
    : SessionHost(journal, adapter, QList<ProviderAdapter*>{claude, cursor}, parent) {}

SessionHost::SessionHost(LocalJournal* journal, CodexAdapter* adapter,
                         QList<ProviderAdapter*> additionalProviders, QObject* parent)
    : QObject(parent), catalogCache_(this), journal_(journal), adapter_(adapter),
      approvals_(new ApprovalBroker(this)),
      mediaStore_(McpMediaStore::directoryForJournal(journal->databasePath())),
      readiness_(ProviderReadiness::pathForJournal(journal->databasePath())) {
    mediaReads_.setMaxThreadCount(mediaReadThreads);
    teamGit_.setMaxThreadCount(1);
    teamRemote_.setMaxThreadCount(1);
    // A generated image whose inline pixels history retention drops keeps a 512 px preview.
    journal_->setImagePreviewWriter([this](const QByteArray& image, const QString& sessionId) {
        QBuffer source;
        source.setData(image);
        if (!source.open(QIODevice::ReadOnly))
            return QJsonObject();
        const auto preview = McpMediaStore::encodePreview(&source, 512);
        if (!preview.value("ok").toBool())
            return QJsonObject();
        const auto jpeg =
            QByteArray::fromBase64(preview.value("data").toString().toLatin1(),
                                   QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        return mediaStore_.storeImage(jpeg, "image/jpeg", sessionId);
    });
    deltaFlush_.setSingleShot(true);
    deltaFlush_.setInterval(deltaCoalesceMs);
    connect(&deltaFlush_, &QTimer::timeout, this, &SessionHost::flushDeltas);
    providers_.append(adapter);
    for (ProviderAdapter* provider : additionalProviders)
        if (provider != nullptr && !providers_.contains(provider))
            providers_.append(provider);
    QSet<QString> liveSessions;
    for (ProviderAdapter* provider : std::as_const(providers_))
        connect(provider, &ProviderAdapter::stopped, this,
                [this, name = provider->name()] { catalogCache_.invalidate(name); });
    connect(adapter_, &CodexAdapter::notificationReceived, this,
            [this](const QString& method, const QJsonObject&) {
                if (method == "account/updated") {
                    catalogCache_.invalidate("codex");
                    // The CLI signed in, out or changed account: a sign-in, key or limit
                    // failure reported for the old account no longer applies.
                    readiness_.forget(QStringLiteral("codex"));
                }
            });
    connect(adapter_, &CodexAdapter::reloaded, this, [this] { catalogCache_.invalidate("codex"); });
    for (ProviderAdapter* provider : additionalProviders)
        if (auto* claude = qobject_cast<ClaudeAdapter*>(provider))
            connect(claude, &ClaudeAdapter::authenticationObserved, this,
                    [this](const QString& source) { readiness_.observeClaudeSource(source); });
    for (const StoredSession& stored : journal_->sessions()) {
        liveSessions.insert(stored.id);
        Session session;
        session.stored = stored;
        // Older daemons let a lingering run of the previous provider overwrite the thread id.
        // Resuming that id fails every turn, so start a fresh thread with a handoff instead.
        const QJsonObject threads =
            QJsonDocument::fromJson(stored.providerThreads.toUtf8()).object();
        for (auto thread = threads.begin(); thread != threads.end(); ++thread) {
            if (thread.key() != stored.adapter && !stored.threadId.isEmpty() &&
                thread.value().toObject().value("thread_id").toString() == stored.threadId) {
                session.stored.threadId.clear();
                session.handoffFrom = thread.key();
                if (!journal_->upsertSession(session.stored))
                    qWarning("Unable to clear the stale thread of %s", qUtf8Printable(stored.id));
            }
        }
        // A restarted daemon has no provider process for turns that were in flight, and
        // nothing ever resumes them. Record them as interrupted instead of an open-ended
        // "recovering" state that clients render as running forever.
        if (session.stored.state == QStringLiteral("running") ||
            session.stored.state == QStringLiteral("starting") ||
            session.stored.state == QStringLiteral("recovering") ||
            session.stored.state == QStringLiteral("offline")) {
            session.stored.state = QStringLiteral("interrupted");
            session.stored.turnId.clear();
            if (!journal_->upsertSession(session.stored))
                qWarning("Unable to record interrupted session %s: %s",
                         qUtf8Printable(session.stored.id),
                         qUtf8Printable(journal_->errorString()));
        }
        // Clients read a turn as running until its turn.completed arrives, so an interrupted
        // turn without one kept the phone's composer locked.
        auto inserted = sessions_.insert(stored.id, session);
        if (inserted->stored.state == QStringLiteral("interrupted") &&
            journal_->hasOpenTurn(stored.id, stored.generation))
            (void)appendEvent(*inserted, QStringLiteral("turn.completed"),
                              {{QStringLiteral("status"), QStringLiteral("interrupted")}});
        // Older builds could persist an entire pasted handoff as a provider title.
        // Repair only that recognizable shape; ordinary and manual titles stay intact.
        if (stored.title.contains(QStringLiteral("<ketplus_response_preferences>")) &&
            stored.title.contains(QStringLiteral("User request:"), Qt::CaseInsensitive))
            (void)updateSessionTitle(*inserted, conversation_title::suggest(stored.title), false);
        if (!stored.threadId.isEmpty()) {
            sessionsByThread_.insert(stored.threadId, stored.id);
        }
    }
    // Background tasks run inside the provider process of the daemon that started them.
    const QHash<QString, QJsonArray> backgroundTasks = journal_->backgroundTaskSnapshots();
    for (auto snapshot = backgroundTasks.cbegin(); snapshot != backgroundTasks.cend(); ++snapshot) {
        const auto session = sessions_.find(snapshot.key());
        if (session == sessions_.end())
            continue;
        session->backgroundTasks = snapshot.value();
        endBackgroundTasks(*session, QStringLiteral("worker_restart"));
    }
    // Media of sessions that no longer exist in the journal is released at startup.
    mediaStore_.retainSessions(liveSessions);
    // ADR 0027: every conversation has a number; older journals are backfilled once.
    agents_ = new AgentBroker(journal_, static_cast<AgentHostPort*>(this), this);
    startTeams();
    for (const Session& session : std::as_const(sessions_)) {
        QString numberError;
        if (journal_
                ->assignConversationNumber(session.stored.id, session.stored.generation,
                                           &numberError)
                .isEmpty())
            qWarning("Unable to number conversation %s: %s", qUtf8Printable(session.stored.id),
                     qUtf8Printable(numberError));
    }
    QTimer::singleShot(0, agents_, &AgentBroker::resume);

    connect(adapter_, &CodexAdapter::notificationReceived, this,
            &SessionHost::handleProviderNotification);
    connect(adapter_, &CodexAdapter::requestReceived, this, &SessionHost::handleProviderRequest);
    connect(adapter_, &CodexAdapter::notificationSkipped, this,
            [this](const QString& method, const QString& threadId, const qsizetype bytes) {
                if (Session* session = sessionForThread(threadId))
                    appendEvent(*session, QStringLiteral("provider.protocol_error"),
                                {{QStringLiteral("provider"), QStringLiteral("codex")},
                                 {QStringLiteral("method"), method},
                                 {QStringLiteral("oversized_bytes"), bytes}});
            });
    connect(adapter_, &CodexAdapter::reloaded, this, &SessionHost::providerReloaded);
    for (ProviderAdapter* provider : std::as_const(providers_)) {
        const QString name = provider->name();
        connect(provider, &ProviderAdapter::stopped, this,
                [this, name](const QString& reason) { providerStopped(name, reason); });
        if (provider == adapter_)
            continue; // Codex requests and notifications are connected above.
        connect(provider, &ProviderAdapter::canonicalEvent, this,
                [this, name](const QString& sessionId, const QString& type,
                             const QJsonObject& payload) {
                    handleCanonicalEvent(name, sessionId, type, payload);
                });
        connect(provider, &ProviderAdapter::requestReceived, this,
                [this, name](const QJsonValue& requestId, const QString& method,
                             const QJsonObject& params) {
                    if (method == QStringLiteral("question.request") ||
                        method == QStringLiteral("cursor/ask_question"))
                        handleCanonicalQuestion(requestId, params, name);
                    else
                        handleCanonicalPermission(name, requestId, params);
                });
    }
    connect(approvals_, &ApprovalBroker::providerResolutionReady, this,
            &SessionHost::routeProviderResponse);
    connect(approvals_, &ApprovalBroker::providerRequestRejected, this,
            &SessionHost::routeProviderError);
    connect(approvals_, &ApprovalBroker::requested, this,
            [this](const QString& approvalId, const QString& sessionId, const qint64 generation,
                   const QString& kind, const QJsonObject& details, const QString& expiresAt) {
                auto it = sessions_.find(sessionId);
                if (it == sessions_.end() || it->stored.generation != generation) {
                    return;
                }
                QJsonObject payload = details;
                payload.insert(QStringLiteral("approval_id"), approvalId);
                payload.insert(QStringLiteral("kind"), kind);
                payload.insert(QStringLiteral("expires_at"), expiresAt);
                appendEvent(*it, QStringLiteral("approval.requested"), payload);
            });
    connect(approvals_, &ApprovalBroker::expired, this,
            [this](const QString& approvalId, const QString& sessionId, const qint64 generation,
                   const QString& kind, const int revision) {
                auto it = sessions_.find(sessionId);
                if (it == sessions_.end() || it->stored.generation != generation) {
                    return;
                }
                appendEvent(*it,
                            kind == QStringLiteral("input") ? QStringLiteral("question.expired")
                                                            : QStringLiteral("approval.expired"),
                            {{QStringLiteral("approval_id"), approvalId},
                             {QStringLiteral("request_id"), approvalId},
                             {QStringLiteral("revision"), revision}});
            });
    auto* cleanup = new QTimer(this);
    cleanup->setInterval(15000);
    connect(cleanup, &QTimer::timeout, this, &SessionHost::processDeletionWork);
    cleanup->start();
    QTimer::singleShot(0, this, &SessionHost::processDeletionWork);
}

QJsonObject SessionHost::handleCommand(const QJsonObject& request, const QString& clientId) {
    const QString type = request.value(QStringLiteral("message_type")).toString();
    const QString messageId = request.value(QStringLiteral("message_id")).toString();
    // Local administration only, checked before the command-result cache.
    if (type.startsWith("session.agent_messages.") && !localClient(clientId))
        return error("forbidden", "Held-message authorization requires the local Desktop UI.");
    if (type.startsWith("session.reviewer.") && !localClient(clientId))
        return error("forbidden", "Choosing a reviewer requires the local Desktop UI.");
    // ADR 0114: teams are the user's; agents use the team_* tools instead.
    if (type.startsWith("team.") && !localClient(clientId))
        return error("forbidden", "Teams are managed from the local Desktop UI.");
    if (type == "session.work_report.read" && !localClient(clientId))
        return error("forbidden", "The work report is read by the local Desktop UI.");
    if (type.startsWith("storage.") && clientId == "cloud-control-v2")
        return error("forbidden", "Storage settings require local administration.");
    if (type.startsWith("session.agent_messages.")) {
        Session* session = nullptr;
        const auto failure = checkedSession(request, &session);
        if (!failure.isEmpty())
            return failure;
    }
    if (isMutating(type)) {
        const QJsonObject cached = journal_->commandResult(messageId);
        if (!cached.isEmpty()) {
            return cached;
        }
    }

    // Until relay resource provenance and clients understand context fences, switched
    // conversations accept only scope-independent remote controls. Local IPC is opt-in.
    const auto remoteSession = sessions_.constFind(request.value("session_id").toString());
    const bool explicitContext =
        remoteSession != sessions_.cend() &&
        request.value("payload").toObject().value("expected_context_revision").toInteger(-1) ==
            remoteSession->stored.contextRevision;
    const auto contextPayload = request.value("payload").toObject();
    const bool resourceContext = remoteSession != sessions_.cend() &&
                                 contextPayload.value("resource_context_revision").toInteger(-1) ==
                                     remoteSession->stored.contextRevision;
    const bool transcriptRead = type == "session.output.read" ||
                                (type == "session.media.read" && !contextPayload.contains("path"));
    const bool settingsOrAttachment =
        QStringList{"session.configuration.read",     "session.configuration.apply",
                    "session.execution_policy.apply", "session.mode.apply",
                    "attachment.stage.begin",         "attachment.stage.chunk",
                    "attachment.stage.commit",        "attachment.stage.cancel"}
            .contains(type);
    const bool scopedSettings =
        settingsOrAttachment && (explicitContext || resourceContext) &&
        (!contextPayload.contains("expected_context_revision") || explicitContext) &&
        (!contextPayload.contains("resource_context_revision") || resourceContext);
    if (clientId == "cloud-control-v2" && remoteSession != sessions_.cend() &&
        remoteSession->stored.contextRevision > 1 && type != "session.interrupt" &&
        type != "session.pending_approvals" && type != "approval.resolve" &&
        type != "question.answer" && type != "session.worktrees" &&
        type != "session.worktree.switch" && type != "session.resource.resolve" &&
        !scopedSettings && !transcriptRead &&
        !((type == "session.start_turn" || type == "session.steer") && explicitContext))
        return error("context_upgrade_required",
                     "Remote worktree switching is not enabled for this protocol version.");
    QJsonObject result;
    if (type.startsWith(QStringLiteral("workspace."))) {
        if (clientId == QStringLiteral("cloud-control-v2"))
            return error("unknown_operation",
                         "Workspace enrollment requires local administration.");
        result = workspaceCatalog(type, request.value("payload").toObject());
    } else if (type.startsWith("storage.size.")) {
        result = storageSize(type, request.value("payload").toObject());
    } else if (type.startsWith("storage.retention.")) {
        result = storageRetention(type, request.value("payload").toObject());
    } else if (type.startsWith("storage.options.")) {
        result = storageOptions(type, request.value("payload").toObject());
    } else if (type.startsWith("storage.compaction.") || type.startsWith("storage.breakdown.") ||
               type.startsWith("storage.export.")) {
        result = storageJobs().handle(type, request.value("payload").toObject());
    } else if (type == QStringLiteral("session.list")) {
        result = listSessions(request);
    } else if (type == QStringLiteral("session.attach") ||
               type == QStringLiteral("session.events_after")) {
        result = eventsAfter(request);
    } else if (type == QStringLiteral("session.transcript_page")) {
        result = transcriptPage(request);
    } else if (type == QStringLiteral("session.activity_group_page")) {
        result = activityGroupPage(request);
    } else if (type == QStringLiteral("session.transcript_window")) {
        result = transcriptWindow(request);
    } else if (type == QStringLiteral("session.transcript_large_event")) {
        result = transcriptLargeEvent(request);
    } else if (type == QStringLiteral("session.pending_approvals")) {
        result = pendingApprovals(request);
    } else if (type == QStringLiteral("session.resource.resolve")) {
        result = resolveResource(request);
    } else if (type == QStringLiteral("session.worktrees")) {
        result = sessionWorktrees(request);
    } else if (type == QStringLiteral("session.worktree.switch")) {
        result = switchWorktree(request);
    } else if (type == QStringLiteral("session.agent_messages.read") ||
               type == QStringLiteral("session.agent_messages.release")) {
        result = heldAgentMessages(request, type.endsWith(".release"));
    } else if (type == QStringLiteral("session.reviewer.get") ||
               type == QStringLiteral("session.reviewer.set")) {
        result = conversationReviewer(request, type.endsWith(".set"));
    } else if (type == QStringLiteral("session.work_report.read")) {
        result = readWorkReport(request);
    } else if (type.startsWith(QStringLiteral("team."))) {
        result = teamCommand(type, request);
    } else if (type == QStringLiteral("session.start_turn")) {
        result = startTurn(request, localClient(clientId));
    } else if (type == QStringLiteral("session.rename")) {
        result = renameSession(request);
    } else if (type == QStringLiteral("session.mark_seen")) {
        result = markSessionSeen(request);
    } else if (type == QStringLiteral("session.set_archived")) {
        result = setSessionArchived(request);
    } else if (type == QStringLiteral("session.tags.set")) {
        result = setSessionTags(request);
    } else if (type == QStringLiteral("tags.catalog")) {
        result = tagCatalog();
    } else if (type == QStringLiteral("tags.catalog.set")) {
        result = setTagCatalog(request);
    } else if (type == QStringLiteral("session.mark_done")) {
        result = markSessionDone(request);
    } else if (type == QStringLiteral("session.delete")) {
        result = deleteSession(request);
    } else if (type == QStringLiteral("session.output.read") ||
               type == QStringLiteral("session.media.read")) {
        result = ephemeralRead(request);
    } else if (type == QStringLiteral("session.steer")) {
        result = steer(request, {}, localClient(clientId));
    } else if (type == QStringLiteral("session.interrupt")) {
        result = interrupt(request);
    } else if (type == QStringLiteral("approval.resolve") ||
               type == QStringLiteral("question.answer")) {
        result = resolveApproval(request);
    } else if (type == QStringLiteral("session.configuration.read")) {
        result = configuration(request, request.value(QStringLiteral("payload"))
                                            .toObject()
                                            .value(QStringLiteral("provider_catalog"))
                                            .toArray());
    } else if (type == QStringLiteral("session.configuration.apply")) {
        result = applyConfiguration(request, clientId != QStringLiteral("cloud-control-v2"));
    } else if (type == QStringLiteral("session.execution_policy.apply")) {
        result = applyExecutionPolicy(request);
    } else if (type == QStringLiteral("session.mode.apply")) {
        result = applyMode(request);
    } else if (type == QStringLiteral("attachment.stage.begin")) {
        result = beginAttachment(request);
    } else if (type == QStringLiteral("attachment.stage.chunk")) {
        result = appendAttachmentChunk(request);
    } else if (type == QStringLiteral("attachment.stage.commit")) {
        result = commitAttachment(request);
    } else if (type == QStringLiteral("attachment.stage.cancel")) {
        result = cancelAttachment(request);
    } else if (type == QStringLiteral("client.ack_cursor")) {
        result = acknowledge(request, clientId);
    } else {
        result = error(QStringLiteral("unknown_operation"),
                       QStringLiteral("The requested daemon operation is not supported."));
    }

    if (isMutating(type)) {
        journal_->storeCommandResult(messageId, result);
    }
    return result;
}

bool SessionHost::hasActiveSessions() const {
    for (const Session& session : sessions_) {
        const QString& state = session.stored.state;
        if (session.startingTurn || state == QStringLiteral("starting") ||
            state == QStringLiteral("running") || state == QStringLiteral("waiting_for_input") ||
            state == QStringLiteral("stopping")) {
            return true;
        }
    }
    return false;
}

void SessionHost::handleProviderQuery(
    const QJsonObject& request, std::function<void(const QJsonObject&, const QString&)> callback) {
    const QString type = request.value(QStringLiteral("message_type")).toString();
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    if (type == QStringLiteral("provider.models")) {
        QJsonObject params{{QStringLiteral("limit"), 100}};
        const QString cursor = payload.value(QStringLiteral("cursor")).toString();
        if (!cursor.isEmpty()) {
            params.insert(QStringLiteral("cursor"), cursor);
        }
        adapter_->call(
            QStringLiteral("model/list"), params,
            [this, callback = std::move(callback), cursor](QJsonObject result,
                                                           const QString& providerError) {
                if (providerError.isEmpty())
                    tagCodexModels(result);
                // Other providers list everything on Codex's first page, in registry order.
                QList<ProviderAdapter*> others;
                if (cursor.isEmpty())
                    for (ProviderAdapter* provider : std::as_const(providers_))
                        if (provider != adapter_ && provider->isAvailable())
                            others.append(provider);
                const bool hasOthers = std::any_of(
                    providers_.cbegin(), providers_.cend(), [this](ProviderAdapter* provider) {
                        return provider != adapter_ && provider->isAvailable();
                    });
                const auto finish = [callback, providerError, hasOthers,
                                     result](const QJsonArray& merged) {
                    if (merged.isEmpty() && !providerError.isEmpty() && !hasOthers) {
                        callback({}, providerError);
                        return;
                    }
                    QJsonObject response = result;
                    response.insert(QStringLiteral("data"), merged);
                    response.insert(QStringLiteral("ok"), true);
                    callback(response, {});
                };
                appendModelLists(others, result.value(QStringLiteral("data")).toArray(), finish);
            });
        return;
    }
    if (type == QStringLiteral("provider.configuration_catalog")) {
        struct Pending {
            QHash<QString, QJsonObject> results;
            QHash<QString, QString> errors;
            QStringList providers;
            QString required;
            bool launching{true};
            bool delivered{false};
        };
        auto pending = std::make_shared<Pending>();
        const auto session = sessions_.constFind(request.value("session_id").toString());
        if (session != sessions_.cend())
            pending->required = session->stored.adapter;
        QList<QPair<QString, ProviderCatalogCache::Fetch>> fetches;
        for (ProviderAdapter* provider : std::as_const(providers_))
            if (provider == adapter_ || provider->isAvailable())
                fetches.append({provider->name(), [provider](auto done) {
                                    provider->configurationCatalog(std::move(done));
                                }});
        for (const auto& fetch : fetches)
            pending->providers.append(fetch.first);
        const auto finish = [pending, callback] {
            if (pending->launching || pending->delivered)
                return;
            if (!pending->required.isEmpty()) {
                if (!pending->results.contains(pending->required))
                    return;
            } else if (pending->results.size() != pending->providers.size())
                return;
            pending->delivered = true;
            QJsonArray merged;
            for (const auto& provider : pending->providers)
                for (const auto& model : taggedCatalog(
                         pending->results.value(provider).value("catalog").toArray(), provider))
                    merged.append(model);
            QString error = pending->errors.value(pending->required);
            if (merged.isEmpty() && error.isEmpty())
                error = "No model catalog is available.";
            callback({{"ok", error.isEmpty()}, {"catalog", merged}}, error);
        };
        if (!pending->required.isEmpty() && !pending->providers.contains(pending->required)) {
            callback({}, "The conversation provider is unavailable.");
            return;
        }
        for (const auto& fetch : fetches)
            catalogCache_.get(
                fetch.first, fetch.second,
                [pending, finish, key = fetch.first](const auto& result, const auto& error) {
                    pending->results.insert(key, result);
                    pending->errors.insert(key, error);
                    finish();
                });
        pending->launching = false;
        finish();
        return;
    }
    if (type == QStringLiteral("provider.rate_limits")) {
        adapter_->call(
            QStringLiteral("account/rateLimits/read"), {},
            [callback = std::move(callback)](QJsonObject result, const QString& providerError) {
                if (providerError.isEmpty()) {
                    result.insert(QStringLiteral("ok"), true);
                }
                callback(result, providerError);
            });
        return;
    }
    if (type == QStringLiteral("provider.runtime_status")) {
        // Check again (#170): forget reported sign-in failures and read every probe afresh.
        if (payload.value(QStringLiteral("recheck")).toBool())
            readiness_.recheck(QDateTime::currentSecsSinceEpoch());
        providerRuntimeStatus(std::move(callback));
        return;
    }
    callback({}, QStringLiteral("Unknown provider query."));
}

QJsonObject SessionHost::configuration(const QJsonObject& request,
                                       const QJsonArray& mergedCatalog) const {
    Session* session = nullptr;
    const QJsonObject failure = const_cast<SessionHost*>(this)->checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    auto result = configurationOf(session->stored, mergedCatalog);
    if (request.value("payload").toObject().value("effective_only").toBool())
        return ok({{"effective_only", true},
                   {"revision", result.value("revision")},
                   {"effective", result.value("effective")},
                   {"adapter", session->stored.adapter}});
    return result;
}

QJsonObject SessionHost::newConversationConfiguration(const QJsonArray& mergedCatalog) const {
    StoredSession fresh;
    fresh.adapter = QStringLiteral("codex");
    QJsonObject configuration = configurationOf(fresh, mergedCatalog);
    // Not started yet, the conversation can run on any agent this Mac has: every agent's
    // models go out, each naming its agent, and the phone sends the agent chosen with the
    // first message.
    const QJsonArray models = agentCatalog(mergedCatalog);
    configuration.insert(QStringLiteral("models"), models);
    configuration.insert(QStringLiteral("catalog_revision"), modelCatalogRevision(models));
    return configuration;
}

QJsonObject SessionHost::configurationOf(const StoredSession& stored,
                                         const QJsonArray& mergedCatalog) const {
    const QJsonArray providerCatalog = catalogForAdapter(mergedCatalog, stored.adapter);
    return ok(
        {{QStringLiteral("configuration_version"), 1},
         {QStringLiteral("catalog_revision"), modelCatalogRevision(providerCatalog)},
         {QStringLiteral("revision"), stored.configurationRevision},
         {QStringLiteral("effective"),
          QJsonObject{{QStringLiteral("model"), stored.model},
                      {QStringLiteral("effort"), stored.effort},
                      {QStringLiteral("execution_policy"), stored.executionPolicy},
                      {QStringLiteral("mode"), stored.mode},
                      {QStringLiteral("compactWindow"),
                       context_harness::compactWindowJson(stored.compactWindow)}}},
         {QStringLiteral("models"), providerCatalog},
         // `models` are what `session.configuration.apply` takes, the running agent's;
         // `agent_models` are every agent's, which a turn can move the conversation to
         // by naming its `provider`, `model` and `effort`.
         {QStringLiteral("adapter"),
          stored.adapter.isEmpty() ? QStringLiteral("codex") : stored.adapter},
         {QStringLiteral("agent_models"), agentCatalog(mergedCatalog)},
         {QStringLiteral("execution_policies"),
          QJsonArray{QStringLiteral("read-only"), QStringLiteral("ask"), QStringLiteral("full")}},
         {QStringLiteral("modes"), QJsonArray{QStringLiteral("default")}},
         {QStringLiteral("attachment_policy"),
          QJsonObject{{QStringLiteral("maximum_count"), 5},
                      {QStringLiteral("maximum_file_bytes"), 20 * 1024 * 1024},
                      {QStringLiteral("maximum_draft_bytes"), 40 * 1024 * 1024},
                      {QStringLiteral("chunk_bytes"), 32 * 1024},
                      {QStringLiteral("allowed_media_types"),
                       QJsonArray{QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
                                  QStringLiteral("text/plain")}},
                      {QStringLiteral("maximum_bytes_by_media_type"),
                       QJsonObject{{QStringLiteral("image/png"), 20 * 1024 * 1024},
                                   {QStringLiteral("image/jpeg"), 20 * 1024 * 1024},
                                   {QStringLiteral("text/plain"), 512 * 1024}}}}}});
}

QJsonObject SessionHost::applyConfiguration(const QJsonObject& request, bool localClient) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    if (payload.value(QStringLiteral("expected_revision")).toInteger(-1) !=
        session->stored.configurationRevision)
        return error(QStringLiteral("stale_configuration"),
                     QStringLiteral("The conversation settings changed."));
    // Defaults apply to the next submission; the active turn already owns its options.
    if (payload.contains("execution_policy") &&
        (session->startingTurn || session->stored.state == QStringLiteral("running")))
        return error(QStringLiteral("session_busy"),
                     QStringLiteral("Wait for the active turn before changing permissions."));
    const QJsonArray mergedCatalog = payload.value(QStringLiteral("provider_catalog")).toArray();
    const QJsonArray providerCatalog = catalogForAdapter(mergedCatalog, session->stored.adapter);
    const bool desktopSelection = localClient && payload.value("desktop_selection").toBool();
    if (!desktopSelection &&
        payload.value(QStringLiteral("expected_catalog_revision")).toString() !=
            modelCatalogRevision(providerCatalog))
        return error(QStringLiteral("stale_model_catalog"),
                     QStringLiteral("The available models changed. Refresh and try again."));
    qint64 compactWindow = session->stored.compactWindow;
    if (const auto failure = context_harness::resolveCompactWindow(
            payload.value(QStringLiteral("compactWindow")), session->stored.adapter,
            session->stored.compactWindow, session->backgroundTasks, compactWindow);
        !failure.isEmpty())
        return failure;
    const QString model = payload.value(QStringLiteral("model")).toString(session->stored.model);
    const QString effort = payload.value(QStringLiteral("effort")).toString(session->stored.effort);
    bool supported = model.isEmpty();
    for (const QJsonValue& value : providerCatalog) {
        const QJsonObject candidate = value.toObject();
        const QJsonArray advertised = candidate.value(QStringLiteral("efforts")).toArray();
        bool matched = candidate.value(QStringLiteral("id")).toString() == model ||
                       candidate.value(QStringLiteral("model")).toString() == model;
        for (const QJsonValue& entry : advertised) {
            if (entry.toObject().value(QStringLiteral("model")).toString() == model)
                matched = true;
        }
        if (!matched)
            continue;
        supported =
            effort.isEmpty() ||
            std::any_of(advertised.begin(), advertised.end(), [&effort](const QJsonValue& entry) {
                return entry.toObject().value(QStringLiteral("id")).toString() == effort;
            });
    }
    if (!supported && !desktopSelection)
        return error(QStringLiteral("unsupported_configuration"),
                     QStringLiteral("The selected settings are not supported."));
    const StoredSession before = session->stored;
    if (desktopSelection && payload.contains("execution_policy")) {
        const QString policy = payload.value("execution_policy").toString();
        if (!QSet<QString>{"ask", "full", "read-only"}.contains(policy) ||
            (policy == "full" && !payload.value("risk_acknowledged").toBool()))
            return error("execution_policy_denied", "The requested permission was not authorized.");
        session->stored.executionPolicy = policy;
    }
    session->stored.model = model;
    session->stored.effort = effort;
    session->stored.compactWindow = compactWindow;
    ++session->stored.configurationRevision;
    if (!journal_->upsertSession(session->stored))
        return error(QStringLiteral("journal_error"), journal_->errorString());
    appendEvent(*session, QStringLiteral("session.configuration_changed"),
                {{"revision", session->stored.configurationRevision},
                 {"effective", configurationOf(session->stored, {}).value("effective")},
                 {"changed", changedSettings(before, session->stored)},
                 {"adapter", session->stored.adapter}});
    return configuration(request, mergedCatalog);
}

QJsonObject SessionHost::applyExecutionPolicy(const QJsonObject& request) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    if (session->startingTurn || session->stored.state == QStringLiteral("running"))
        return error(QStringLiteral("session_busy"),
                     QStringLiteral("Wait for the active turn to finish."));
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const QString value = payload.value(QStringLiteral("execution_policy")).toString();
    if (payload.value(QStringLiteral("expected_revision")).toInteger(-1) !=
        session->stored.configurationRevision)
        return error(QStringLiteral("stale_configuration"),
                     QStringLiteral("The conversation settings changed."));
    if (!QSet<QString>{QStringLiteral("read-only"), QStringLiteral("ask"), QStringLiteral("full")}
             .contains(value) ||
        (value == QStringLiteral("full") &&
         payload.value(QStringLiteral("risk_acknowledged")).toBool() != true))
        return error(QStringLiteral("execution_policy_denied"),
                     QStringLiteral("The requested execution policy was not authorized."));
    const StoredSession before = session->stored;
    session->stored.executionPolicy = value;
    ++session->stored.configurationRevision;
    if (!journal_->upsertSession(session->stored))
        return error(QStringLiteral("journal_error"), journal_->errorString());
    appendEvent(*session, QStringLiteral("session.configuration_changed"),
                {{"revision", session->stored.configurationRevision},
                 {"effective", configurationOf(session->stored, {}).value("effective")},
                 {"changed", changedSettings(before, session->stored)},
                 {"adapter", session->stored.adapter}});
    return ok({{QStringLiteral("revision"), session->stored.configurationRevision},
               {QStringLiteral("execution_policy"), value}});
}

QJsonObject SessionHost::applyMode(const QJsonObject& request) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    if (session->startingTurn || session->stored.state == QStringLiteral("running"))
        return error(QStringLiteral("session_busy"),
                     QStringLiteral("Wait for the active turn to finish."));
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const QString value = payload.value(QStringLiteral("mode")).toString();
    if (payload.value(QStringLiteral("expected_revision")).toInteger(-1) !=
            session->stored.configurationRevision ||
        !QSet<QString>{QStringLiteral("default")}.contains(value))
        return error(QStringLiteral("stale_or_unsupported_mode"),
                     QStringLiteral("The selected mode is no longer available."));
    const StoredSession before = session->stored;
    session->stored.mode = value;
    ++session->stored.configurationRevision;
    if (!journal_->upsertSession(session->stored))
        return error(QStringLiteral("journal_error"), journal_->errorString());
    appendEvent(*session, QStringLiteral("session.configuration_changed"),
                {{"revision", session->stored.configurationRevision},
                 {"effective", configurationOf(session->stored, {}).value("effective")},
                 {"changed", changedSettings(before, session->stored)},
                 {"adapter", session->stored.adapter}});
    return ok({{QStringLiteral("revision"), session->stored.configurationRevision},
               {QStringLiteral("mode"), value}});
}

QJsonObject SessionHost::beginAttachment(const QJsonObject& request) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    QString journalError;
    const QString mediaType = payload.value(QStringLiteral("media_type")).toString();
    const qint64 totalBytes = payload.value(QStringLiteral("total_bytes")).toInteger(-1);
    if ((mediaType == QStringLiteral("text/plain") && totalBytes > 512 * 1024) ||
        (!QSet<QString>{QStringLiteral("text/plain"), QStringLiteral("image/png"),
                        QStringLiteral("image/jpeg")}
              .contains(mediaType)))
        return error(QStringLiteral("attachment_policy_denied"),
                     QStringLiteral("This attachment is outside the supported policy."));
    const bool accepted = journal_->beginAttachment(
        session->stored.id, session->stored.generation,
        payload.value(QStringLiteral("attachment_id")).toString(),
        payload.value(QStringLiteral("expected_revision")).toInteger(-1),
        payload.value(QStringLiteral("name")).toString(), mediaType, totalBytes,
        payload.value(QStringLiteral("chunk_count")).toInt(-1),
        payload.value(QStringLiteral("digest_sha256")).toString(), &journalError);
    return accepted ? ok({{QStringLiteral("revision"), 1}})
                    : error(journalError, QStringLiteral("The attachment could not be staged."));
}

QJsonObject SessionHost::appendAttachmentChunk(const QJsonObject& request) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const QByteArray data = QByteArray::fromBase64(
        payload.value(QStringLiteral("data")).toString().toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    QString journalError;
    const bool accepted = journal_->appendAttachmentChunk(
        session->stored.id, session->stored.generation,
        payload.value(QStringLiteral("attachment_id")).toString(),
        payload.value(QStringLiteral("expected_revision")).toInteger(-1),
        payload.value(QStringLiteral("chunk_index")).toInt(-1), data,
        payload.value(QStringLiteral("digest_sha256")).toString(), &journalError);
    return accepted ? ok({{QStringLiteral("accepted"), true}})
                    : error(journalError, QStringLiteral("The attachment chunk was rejected."));
}

QJsonObject SessionHost::commitAttachment(const QJsonObject& request) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    QString journalError;
    const bool accepted = journal_->commitAttachment(
        session->stored.id, session->stored.generation,
        payload.value(QStringLiteral("attachment_id")).toString(),
        payload.value(QStringLiteral("expected_revision")).toInteger(-1), &journalError);
    return accepted ? ok({{QStringLiteral("status"), QStringLiteral("ready")}})
                    : error(journalError, QStringLiteral("The attachment is incomplete."));
}

QJsonObject SessionHost::cancelAttachment(const QJsonObject& request) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    QString journalError;
    const bool accepted = journal_->cancelAttachment(
        session->stored.id, session->stored.generation,
        payload.value(QStringLiteral("attachment_id")).toString(),
        payload.value(QStringLiteral("expected_revision")).toInteger(-1), &journalError);
    return accepted ? ok({{QStringLiteral("status"), QStringLiteral("cancelled")}})
                    : error(journalError, QStringLiteral("The attachment could not be cancelled."));
}

QJsonObject SessionHost::eventsAfter(const QJsonObject& request) const {
    Session* session = nullptr;
    const QJsonObject failure = const_cast<SessionHost*>(this)->checkedSession(request, &session);
    if (!failure.isEmpty()) {
        return failure;
    }
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const qint64 cursor = payload.value(QStringLiteral("after_cursor")).toInteger(-1);
    if (cursor < 0 || cursor > session->stored.lastCursor) {
        return error(QStringLiteral("invalid_cursor"),
                     QStringLiteral("after_cursor is outside the session event range."));
    }
    const bool compactCompleted = payload.value(QStringLiteral("compact_completed")).toBool();
    const qint64 earliest =
        compactCompleted
            ? journal_->earliestCompactedCursor(session->stored.id, session->stored.generation)
            : journal_->earliestCursor(session->stored.id, session->stored.generation);
    if (earliest < 0) {
        return error(QStringLiteral("journal_error"), journal_->errorString());
    }
    if (cursor < earliest - 1) {
        // Retention pruned events after this cursor (pruning follows the clients that have
        // acknowledged, so a new or silent client can fall behind). Say so explicitly and name
        // the oldest retained cursor instead of returning a stream with a hidden gap.
        QJsonObject expired =
            error(QStringLiteral("cursor_expired"),
                  QStringLiteral("after_cursor precedes the retained event range."));
        expired.insert(QStringLiteral("events_pruned"), true);
        expired.insert(QStringLiteral("earliest_available_cursor"), earliest);
        expired.insert(QStringLiteral("last_event_cursor"), session->stored.lastCursor);
        return expired;
    }
    const int limit = payload.value(QStringLiteral("limit")).toInt(protocol::maximumReplayEvents);
    const EventReplayPage page =
        compactCompleted
            ? journal_->compactedEventsAfter(session->stored.id, session->stored.generation, cursor,
                                             limit)
            : EventReplayPage{journal_->eventsAfter(session->stored.id, session->stored.generation,
                                                    cursor, limit),
                              cursor};
    qint64 nextCursor = page.nextCursor;
    if (!compactCompleted && !page.events.isEmpty()) {
        nextCursor = page.events.last().toObject().value(QStringLiteral("cursor")).toInteger();
    }
    return ok({{QStringLiteral("session_id"), session->stored.id},
               {QStringLiteral("session_generation"), session->stored.generation},
               {QStringLiteral("events"), page.events},
               {QStringLiteral("next_event_cursor"), nextCursor},
               {QStringLiteral("events_pruned"), false},
               {QStringLiteral("earliest_available_cursor"), earliest},
               {QStringLiteral("last_event_cursor"), session->stored.lastCursor}});
}

QJsonObject SessionHost::transcriptPage(const QJsonObject& request) const {
    Session* session = nullptr;
    const QJsonObject failure = const_cast<SessionHost*>(this)->checkedSession(request, &session);
    if (!failure.isEmpty()) {
        return failure;
    }
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const qint64 before = payload.value(QStringLiteral("before_turn_sequence")).toInteger(0);
    const int limit = payload.value(QStringLiteral("limit")).toInt(3);
    // before_cursor (exclusive event cursor) continues a page truncated inside a turn; it is
    // mutually exclusive with before_turn_sequence.
    const QJsonValue beforeCursorValue = payload.value(QStringLiteral("before_cursor"));
    const qint64 beforeCursor = beforeCursorValue.toInteger(-1);
    const bool hasBeforeCursor = !beforeCursorValue.isUndefined() && !beforeCursorValue.isNull();
    if (before < 0 || limit < 1 || limit > 20 ||
        (hasBeforeCursor && (!beforeCursorValue.isDouble() || beforeCursor < 1 || before > 0))) {
        return error(QStringLiteral("invalid_page"),
                     QStringLiteral("Transcript page boundaries are invalid."));
    }
    const TranscriptPage page =
        journal_->transcriptPage(session->stored.id, session->stored.generation, before, limit,
                                 hasBeforeCursor ? beforeCursor : 0);
    if (!journal_->errorString().isEmpty()) {
        return error(QStringLiteral("journal_error"), journal_->errorString());
    }
    return ok({{QStringLiteral("transcript_version"), 1},
               {QStringLiteral("session_id"), session->stored.id},
               {QStringLiteral("session_generation"), session->stored.generation},
               {QStringLiteral("snapshot_cursor"), page.snapshotCursor},
               {QStringLiteral("earliest_turn_sequence"), page.earliestTurnSequence},
               {QStringLiteral("has_more_before"), page.hasMoreBefore},
               {QStringLiteral("truncated"), page.truncated},
               {QStringLiteral("first_event_cursor"), page.firstCursor},
               {QStringLiteral("events"), page.events}});
}

QJsonObject SessionHost::startTurn(const QJsonObject& request, bool localClient,
                                   const QJsonObject& agentOrigin) {
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const QString prompt = payload.value(QStringLiteral("prompt")).toString().trimmed();
    const QJsonArray submittedInput = payload.value(QStringLiteral("input")).toArray();
    const bool hasImage =
        std::any_of(submittedInput.begin(), submittedInput.end(), [](const QJsonValue& value) {
            const QString type = value.toObject().value("type").toString();
            return type == QStringLiteral("localImage") || type == QStringLiteral("image");
        });
    if ((prompt.isEmpty() && !hasImage &&
         payload.value(QStringLiteral("attachment_ids")).toArray().isEmpty()) ||
        prompt.toUtf8().size() > maximumPromptBytes) {
        return error(QStringLiteral("invalid_prompt"),
                     QStringLiteral("prompt must be non-empty and no larger than 256 KiB."));
    }
    // Clients may wrap the request (skills, Atlas instructions, attachment contents) before it
    // reaches the provider. Journal and title what the user typed so restored transcripts match.
    QString displayText = payload.value(QStringLiteral("display_text")).toString().trimmed();
    if (displayText.isEmpty() || displayText.toUtf8().size() > maximumPromptBytes)
        displayText = prompt;
    QString sessionId = request.value(QStringLiteral("session_id")).toString();
    if (sessionId.isEmpty()) {
        sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    auto it = sessions_.find(sessionId);
    if (it != sessions_.end()) {
        const auto expected = payload.value(QStringLiteral("expected_context_revision"));
        if ((it->stored.contextRevision > 1 || !expected.isUndefined()) &&
            expected.toInteger(-1) != it->stored.contextRevision)
            return error(QStringLiteral("stale_context"),
                         QStringLiteral("Refresh the conversation's execution context."));
    }
    // A deleted worktree does not lock the conversation: its turns run in the repository's
    // primary worktree (executionDirectory) until a replacement checkout appears.
    const QString requestedWorktree = payload.value(QStringLiteral("worktree_path")).toString();
    const QString worktree =
        it != sessions_.end() &&
                (requestedWorktree.isEmpty() ||
                 (QDir::cleanPath(requestedWorktree) == QDir::cleanPath(it->stored.worktreePath) &&
                  !QFileInfo(it->stored.worktreePath).isDir()))
            ? it->stored.worktreePath
            : normalizedWorktree(requestedWorktree);
    if (worktree.isEmpty()) {
        return error(
            QStringLiteral("invalid_worktree"),
            QStringLiteral("worktree_path must identify an existing directory for a new session."));
    }
    const QJsonObject options = payload.value(QStringLiteral("options")).toObject();
    const QString adapter = requestedAdapter(
        options, it == sessions_.end() ? QStringLiteral("codex") : it->stored.adapter);
    if (it != sessions_.end() && it->stored.contextRevision > 1 && adapter != "codex" &&
        adapter != it->stored.adapter)
        return error(QStringLiteral("context_switch_unsupported"),
                     QStringLiteral("This provider cannot resume a switched context yet."));
    if (!isCanonicalAdapter(adapter)) {
        return error(QStringLiteral("provider_unavailable"),
                     QStringLiteral("The selected agent provider is not available."));
    }
    qint64 compactWindow = 0;
    if (const auto failure = context_harness::resolveCompactWindow(
            options.value(QStringLiteral("compactWindow")), adapter,
            it == sessions_.end() ? 0 : it->stored.compactWindow,
            it == sessions_.end() ? QJsonArray{} : it->backgroundTasks, compactWindow);
        !failure.isEmpty())
        return failure;
    // Codex is checked when its process starts, as before.
    if (adapter != QStringLiteral("codex") && availableProvider(adapter) == nullptr)
        return error(QStringLiteral("provider_unavailable"), missingProviderMessage(adapter));
    if (it == sessions_.end()) {
        Session session;
        session.stored.id = sessionId;
        session.stored.worktreePath = worktree;
        session.stored.adapter = adapter;
        session.stored.state = QStringLiteral("ready");
        session.stored.generation = 1;
        session.stored.title = conversation_title::suggest(displayText);
        // A permission chosen before the first message, as the phone offers it. Full access
        // needs the same acknowledgement as switching to it later.
        if (options.value(QStringLiteral("execution_policy")).toString() ==
                QStringLiteral("full") &&
            options.value(QStringLiteral("risk_acknowledged")).toBool())
            session.stored.executionPolicy = QStringLiteral("full");
        if (!journal_->upsertSession(session.stored)) {
            return error(QStringLiteral("journal_error"), journal_->errorString());
        }
        it = sessions_.insert(sessionId, session);
        QString numberError;
        if (journal_->assignConversationNumber(sessionId, session.stored.generation, &numberError)
                .isEmpty())
            qWarning("Unable to number conversation %s: %s", qUtf8Printable(sessionId),
                     qUtf8Printable(numberError));
        QString createError;
        if (!appendEvent(
                *it, QStringLiteral("session.created"),
                {{QStringLiteral("worktree_path"), worktree}, {QStringLiteral("adapter"), adapter}},
                &createError)) {
            return error(QStringLiteral("journal_error"), createError);
        }
    } else {
        const qint64 generation =
            request.value(QStringLiteral("session_generation")).toInteger(it->stored.generation);
        if (generation != it->stored.generation) {
            return error(QStringLiteral("stale_generation"),
                         QStringLiteral("The command targets an old session generation."));
        }
        if (it->stored.worktreePath != worktree) {
            return error(QStringLiteral("worktree_mismatch"),
                         QStringLiteral("A session cannot move to another worktree."));
        }
        if (it->startingTurn || it->stored.state == QStringLiteral("running")) {
            return error(QStringLiteral("session_busy"),
                         QStringLiteral("The session already has an active turn."));
        }
        if (it->stored.title.isEmpty() && agentOrigin.isEmpty()) {
            it->stored.title = conversation_title::suggest(displayText);
        }
    }
    // ADR 0030: no desktop control while a remote client's turn runs. Busy sessions were
    // rejected above, so no turn is active here. A later local turn lifts the marker, so a
    // crash or a missed turn end can only leave control off.
    if (!computer_tools::setRemoteTurn(sessionId, !localClient) && !localClient &&
        computer_tools::loadSettings().enabled)
        return error(
            QStringLiteral("computer_policy_error"),
            QStringLiteral("KetPlus could not turn off computer use for this remote turn."));
    // Persist repository identity before a checkout is removed; git runs off this thread.
    rememberRepositoryIdentity(it->stored.worktreePath);
    const bool worktreeMissing = !QFileInfo(it->stored.worktreePath).isDir();
    noteWorktreesBeforeTurn(*it);
    QJsonArray input = payload.value(QStringLiteral("input")).toArray();
    if (input.isEmpty()) {
        input.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                 {QStringLiteral("text"), prompt}});
    }
    QJsonArray messageImages;
    if (const QJsonObject failure =
            attachMessageMedia(*it, payload, localClient, input, messageImages);
        !failure.isEmpty())
        return failure;
    // A provider handoff starts a fresh native context, including on A -> B -> A. Only the
    // current conversation transcript carries context forward; retired threads are cleanup refs.
    // The note that points the new provider at the transcript is delivered only with a turn
    // the provider accepted: handoffFrom stays set until its turn/start succeeds, so a turn
    // rejected by the journal or the provider hands off again on the next attempt instead of
    // starting the new provider blind.
    QString handoffTranscript;
    if (it->stored.adapter != adapter) {
        const QString previousAdapter = it->stored.adapter;
        const QString previousThread = it->stored.threadId;
        const QString previousThreads = it->stored.providerThreads;
        const bool previousAttached = it->attachedToProvider;
        const auto failHandoff = [&](const QString& reason) {
            // Nothing was journaled: the conversation stays with its previous provider.
            it->stored.adapter = previousAdapter;
            it->stored.threadId = previousThread;
            it->stored.providerThreads = previousThreads;
            it->attachedToProvider = previousAttached;
            if (!previousThread.isEmpty())
                sessionsByThread_.insert(previousThread, sessionId);
            for (const QString& heldPath : std::exchange(it->materializedAttachments, {}))
                QFile::remove(heldPath);
            for (const QString& id : std::exchange(it->stagedAttachmentIds, {}))
                (void)journal_->cancelAttachment(sessionId, it->stored.generation, id, 1);
            return error(QStringLiteral("handoff_failed"), reason);
        };
        QJsonObject threads = QJsonDocument::fromJson(it->stored.providerThreads.toUtf8()).object();
        provider_handoff::rememberThread(threads, previousAdapter, it->stored.threadId);
        QString writeError;
        handoffTranscript = provider_handoff::writeTranscript(
            handoffDirectory(sessionId),
            provider_handoff::renderTranscript(
                provider_handoff::journalEvents(*journal_, sessionId, it->stored.generation)),
            &writeError);
        if (handoffTranscript.isEmpty())
            return failHandoff(writeError);
        if (!it->stored.threadId.isEmpty())
            sessionsByThread_.remove(it->stored.threadId);
        it->stored.adapter = adapter;
        it->stored.threadId.clear();
        it->stored.providerThreads =
            QString::fromUtf8(QJsonDocument(threads).toJson(QJsonDocument::Compact));
        it->attachedToProvider = false;
        QString changeError;
        if (!appendEvent(
                *it, QStringLiteral("session.provider_changed"),
                {{QStringLiteral("from"), previousAdapter}, {QStringLiteral("to"), adapter}},
                &changeError))
            return failHandoff(changeError);
        it->handoffFrom = previousAdapter;
    }
    if (!it->handoffFrom.isEmpty() && it->stored.adapter == adapter) {
        if (handoffTranscript.isEmpty()) {
            QString writeError;
            handoffTranscript = provider_handoff::writeTranscript(
                handoffDirectory(sessionId),
                provider_handoff::renderTranscript(
                    provider_handoff::journalEvents(*journal_, sessionId, it->stored.generation)),
                &writeError);
            if (handoffTranscript.isEmpty())
                return error(QStringLiteral("handoff_failed"), writeError);
        }
        input.prepend(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"),
                         provider_handoff::handoffNote(it->handoffFrom, handoffTranscript, 0)}});
    }
    const StoredSession previous = it->stored;
    it->stored.state = QStringLiteral("starting");
    QJsonObject userMessage{{QStringLiteral("text"), displayText},
                            {QStringLiteral("command_id"), request.value("message_id")}};
    if (!messageImages.isEmpty())
        userMessage.insert(QStringLiteral("images"), messageImages);
    // ADR 0027: an agent message opens this turn; clients show it instead of user text.
    for (auto field = agentOrigin.constBegin(); field != agentOrigin.constEnd(); ++field)
        userMessage.insert(field.key(), field.value());
    // Clients pair the prompt they already display with this echo by id instead of guessing.
    const QString clientMessageId = payload.value(QStringLiteral("client_message_id")).toString();
    if (!clientMessageId.isEmpty() && clientMessageId.size() <= 128)
        userMessage.insert(QStringLiteral("client_message_id"), clientMessageId);
    if (localClient && options.contains("permission")) {
        const auto permission = options.value("permission").toString();
        it->stored.executionPolicy = permission == "danger-full-access" ? "full"
                                     : permission == "read-only"        ? "read-only"
                                                                        : "ask";
    }
    if (!options.value("model").toString().isEmpty())
        it->stored.model = options.value("model").toString();
    if (!options.value("effort").toString().isEmpty())
        it->stored.effort = options.value("effort").toString();
    it->stored.compactWindow = compactWindow;
    QJsonObject configuration;
    QString appendError;
    // The clients word the change from what changed (ADR 0027, "Model changed to …").
    const QJsonArray changed = changedSettings(previous, it->stored);
    if (!changed.isEmpty()) {
        if (it->stored.configurationRevision == std::numeric_limits<qint64>::max()) {
            appendError = "Session configuration revision is exhausted.";
        } else {
            ++it->stored.configurationRevision;
            configuration = {{"revision", it->stored.configurationRevision},
                             {"effective", configurationOf(it->stored, {}).value("effective")},
                             {"changed", changed},
                             {"adapter", it->stored.adapter}};
        }
    }
    const QPointer<SessionHost> acceptingHost(this);
    const bool accepted =
        appendError.isEmpty() && journalTurnStart(*it, userMessage, configuration, &appendError);
    if (acceptingHost.isNull())
        return error("session_unavailable", "The session host closed during turn acceptance.");
    it = sessions_.find(sessionId);
    if (it == sessions_.end() || it->stored.generation != previous.generation)
        return error("session_unavailable", "The session changed during turn acceptance.");
    if (!accepted) {
        // Prior stream flushes can advance counters; restore only the draft
        // configuration/state, leaving those already accepted stream events intact.
        it->stored.state = previous.state;
        it->stored.model = previous.model;
        it->stored.effort = previous.effort;
        it->stored.executionPolicy = previous.executionPolicy;
        it->stored.compactWindow = previous.compactWindow;
        it->stored.configurationRevision = previous.configurationRevision;
        releaseTurnAttachments(*it);
        return error("journal_error", appendError);
    }
    it->startingTurn = true;
    it->stopRequested = false;
    QJsonObject turnOptions = options;
    turnOptions.insert(QStringLiteral("provider"), it->stored.adapter);
    turnOptions.insert(QStringLiteral("model"), it->stored.model);
    turnOptions.insert(QStringLiteral("effort"), it->stored.effort);
    turnOptions.insert(QStringLiteral("execution_policy"), it->stored.executionPolicy);
    turnOptions.insert(QStringLiteral("mode"), it->stored.mode);
    // Only the broker's review requests set this; clients cannot (ADR 0067).
    turnOptions.remove(QStringLiteral("review_guard"));
    turnOptions.remove(QStringLiteral("review_profile"));
    if (agentOrigin.value(QStringLiteral("review_guard")).toBool()) {
        turnOptions.insert(QStringLiteral("review_guard"), true);
        turnOptions.insert(QStringLiteral("review_profile"),
                           agentOrigin.value(QStringLiteral("review_profile")).toString());
    }
    turnOptions.insert(QStringLiteral("compactWindow"),
                       context_harness::compactWindowJson(it->stored.compactWindow));
    if (it->stored.contextRevision > 1) {
        const QJsonObject context{{"worktree_path", it->stored.worktreePath},
                                  {"context_revision", it->stored.contextRevision}};
        input.append(QJsonObject{
            {"type", "text"},
            {"text",
             QStringLiteral("KetPlus execution context for this turn: %1. "
                            "Earlier messages may reference other worktrees. Re-read the "
                            "target worktree's instructions and files before editing.")
                 .arg(QString::fromUtf8(QJsonDocument(context).toJson(QJsonDocument::Compact)))}});
    }
    if (worktreeMissing)
        input.append(QJsonObject{
            {"type", "text"},
            {"text", QStringLiteral("KetPlus: this conversation's worktree %1 no longer exists, so "
                                    "this turn runs in %2. If you recreate a worktree for this "
                                    "work, KetPlus moves the conversation to it after the turn.")
                         .arg(it->stored.worktreePath, executionDirectory(*it))}});
    if (agentOrigin.isEmpty()) {
        addAgentContext(*it, displayText, input, true);
    } else {
        input.append(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"),
                         agent_messaging::automaticTurnGuidance(
                             agentOrigin.value(QStringLiteral("agent_messages")).toArray())}});
    }
    prepareTurn(sessionId, it->stored.generation, input, turnOptions);
    return ok(
        {{QStringLiteral("accepted"), true},
         {QStringLiteral("session_id"), sessionId},
         {QStringLiteral("session_generation"), it->stored.generation},
         {QStringLiteral("number"), journal_->conversationNumber(sessionId, it->stored.generation)},
         {QStringLiteral("last_event_cursor"), it->stored.lastCursor}});
}

QJsonObject SessionHost::renameSession(const QJsonObject& request) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty()) {
        return failure;
    }
    const QString title = request.value(QStringLiteral("payload"))
                              .toObject()
                              .value(QStringLiteral("title"))
                              .toString()
                              .trimmed();
    if (title.isEmpty() || title.toUtf8().size() > maximumTitleBytes) {
        return error(QStringLiteral("invalid_title"),
                     QStringLiteral("title must be non-empty and no larger than 4 KiB."));
    }

    if (!updateSessionTitle(*session, title, true)) {
        return error(QStringLiteral("journal_error"), journal_->errorString());
    }
    session->pendingProviderTitle = title;
    syncProviderTitle(*session);
    return ok({{QStringLiteral("accepted"), true}, {QStringLiteral("title"), title}});
}

QJsonObject SessionHost::markSessionSeen(const QJsonObject& request) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty())
        return failure;
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    qint64 cursor = payload.value(QStringLiteral("cursor")).toInteger(-1);
    if (cursor < 0)
        cursor = session->stored.lastCursor;
    if (cursor < 0)
        cursor = 0;
    if (cursor > session->stored.lastCursor)
        cursor = session->stored.lastCursor;
    if (!journal_->markSessionSeen(session->stored.id, session->stored.generation, cursor)) {
        // No row matched (should not happen after checkedSession) or journal error.
        if (!journal_->errorString().isEmpty())
            return error(QStringLiteral("journal_error"), journal_->errorString());
    }
    session->stored.lastSeenCursor = qMax(session->stored.lastSeenCursor, cursor);
    return ok({{QStringLiteral("accepted"), true},
               {QStringLiteral("session_id"), session->stored.id},
               {QStringLiteral("session_generation"), session->stored.generation},
               {QStringLiteral("last_seen_cursor"), session->stored.lastSeenCursor},
               {QStringLiteral("last_event_cursor"), session->stored.lastCursor}});
}

QJsonObject SessionHost::deleteSession(const QJsonObject& request) {
    const QString sessionId = request.value(QStringLiteral("session_id")).toString();
    const qint64 generation = request.value(QStringLiteral("session_generation")).toInteger(-1);
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const QString operationId =
        payload.value(QStringLiteral("delete_operation_id"))
            .toString(request.value(QStringLiteral("message_id")).toString());
    const qint64 lifecycleRevision =
        payload.contains(QStringLiteral("expected_lifecycle_revision"))
            ? payload.value(QStringLiteral("expected_lifecycle_revision")).toInteger() + 2
            : 1;
    const SessionTombstone prior = journal_->sessionTombstone(sessionId, generation);
    if (!prior.sessionId.isEmpty()) {
        if (prior.operationId != operationId || prior.lifecycleRevision != lifecycleRevision)
            return error(
                QStringLiteral("lifecycle_revision_conflict"),
                QStringLiteral("This session generation was deleted by another operation."));
        return ok({{QStringLiteral("accepted"), true},
                   {QStringLiteral("applied"), true},
                   {QStringLiteral("session_id"), sessionId},
                   {QStringLiteral("session_generation"), generation},
                   {QStringLiteral("lifecycle_revision"), prior.lifecycleRevision},
                   {QStringLiteral("operation_id"), prior.operationId},
                   {QStringLiteral("deleted_at"), prior.deletedAt}});
    }
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty()) {
        return failure;
    }
    const QString state = session->stored.state;
    if (session->startingTurn || state == QStringLiteral("starting") ||
        state == QStringLiteral("running") || state == QStringLiteral("waiting_for_input") ||
        state == QStringLiteral("stopping")) {
        return error(QStringLiteral("session_busy"),
                     QStringLiteral("Stop the active turn before deleting this conversation."));
    }
    const QString threadId = session->stored.threadId;
    if (hasLiveBackgroundTasks(*session))
        return error(QStringLiteral("session_busy"),
                     QStringLiteral("Stop background tasks before deleting this conversation."));
    const auto targets = providerCleanupTargets(session->stored);
    if (!targets)
        return error(QStringLiteral("journal_error"),
                     QStringLiteral("Provider thread ownership could not be read; the conversation "
                                    "was kept."));
    // T-60: only Codex and Claude delete the threads KetPlus started for a conversation. A
    // thread of another provider (Cursor, or one this daemon does not know) lives in that
    // provider's own store, outside KetPlus-owned paths: deleting the conversation removes
    // KetPlus's local data and leaves that history to the provider, as the delete dialog says.
    QJsonArray cleanup;
    for (const auto& value : *targets) {
        const auto provider = value.toObject().value("provider").toString();
        if (provider == "codex" || provider == "claude")
            cleanup.append(value);
    }
    const auto lastCursor = session->stored.lastCursor;
    QString journalError;
    if (!journal_->deleteSession(sessionId, generation, operationId, lifecycleRevision,
                                 &journalError, cleanup)) {
        return error(QStringLiteral("journal_error"), journalError);
    }
    if (!threadId.isEmpty())
        sessionsByThread_.remove(threadId);
    QDir(handoffDirectory(sessionId)).removeRecursively();
    QDir(reviewScratchDirectory(sessionId)).removeRecursively();
    deferredJournalBytes_ -= session->deferredJournalBytes;
    deferredJournalCount_ -= session->deferredJournal.size();
    sessions_.remove(sessionId);
    agents_->forget(sessionId);
    teams_->conversationDeleted(sessionId);
    mediaStore_.releaseSession(sessionId);
    emit eventReady(
        QJsonObject{{"session_id", sessionId},
                    {"session_generation", generation},
                    {"cursor", lastCursor + 1},
                    {"event_type", "session.deleted"},
                    {"sent_at", protocol::timestamp()},
                    {"payload", QJsonObject{{"operation_id", operationId},
                                            {"lifecycle_revision", lifecycleRevision}}}});
    QTimer::singleShot(0, this, &SessionHost::processDeletionWork);
    const SessionTombstone tombstone = journal_->sessionTombstone(sessionId, generation);
    return ok({{QStringLiteral("accepted"), true},
               {QStringLiteral("applied"), true},
               {QStringLiteral("session_id"), sessionId},
               {QStringLiteral("session_generation"), generation},
               {QStringLiteral("lifecycle_revision"), tombstone.lifecycleRevision},
               {QStringLiteral("operation_id"), tombstone.operationId},
               {QStringLiteral("deleted_at"), tombstone.deletedAt}});
}

QJsonObject SessionHost::steer(const QJsonObject& request, CommandCallback outcome,
                               bool localClient, const QJsonObject& agentOrigin) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty()) {
        return failure;
    }
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const QString prompt = payload.value(QStringLiteral("prompt")).toString().trimmed();
    const auto expectedContext =
        request.value("payload").toObject().value("expected_context_revision");
    if ((session->stored.contextRevision > 1 || !expectedContext.isUndefined()) &&
        expectedContext.toInteger(-1) != session->stored.contextRevision)
        return error("stale_context", "The input targets an old execution context.");
    const QJsonArray submittedInput = payload.value(QStringLiteral("input")).toArray();
    const bool hasImage =
        std::any_of(submittedInput.begin(), submittedInput.end(), [](const QJsonValue& value) {
            const QString type = value.toObject().value("type").toString();
            return type == QStringLiteral("localImage") || type == QStringLiteral("image");
        });
    if ((prompt.isEmpty() && !hasImage &&
         payload.value(QStringLiteral("attachment_ids")).toArray().isEmpty()) ||
        prompt.toUtf8().size() > maximumPromptBytes) {
        return error(QStringLiteral("invalid_prompt"), QStringLiteral("Invalid steering prompt."));
    }
    // As for a new turn: journal what the user typed, not the skills wrapped around it.
    QString displayText = payload.value(QStringLiteral("display_text")).toString().trimmed();
    if (displayText.isEmpty() || displayText.toUtf8().size() > maximumPromptBytes)
        displayText = prompt;
    if (!providerSupports(session->stored.adapter, compat::capability::turnSteer)) {
        return error(QStringLiteral("unsupported_provider_operation"),
                     QStringLiteral("This provider does not support steering a running turn."));
    }
    const bool claude = session->stored.adapter == QStringLiteral("claude");
    // A Claude turn takes steering from the moment the sidecar starts it, before
    // the SDK has reported the thread id.
    const bool running = session->stored.state == QStringLiteral("running") &&
                         (claude || !session->stored.threadId.isEmpty()) &&
                         !session->stored.turnId.isEmpty();
    if (!running) {
        return error(QStringLiteral("turn_not_running"),
                     QStringLiteral("The session has no active turn to steer."));
    }
    if (claude && provider(QStringLiteral("claude")) == nullptr) {
        return error(QStringLiteral("provider_unavailable"),
                     missingProviderMessage(QStringLiteral("claude")));
    }
    // Images reach the running turn as they would a new one: the provider reads temporary
    // copies, and turn.steered keeps media-store references so every client can draw them.
    QJsonArray input = payload.value(QStringLiteral("input")).toArray();
    if (input.isEmpty())
        input.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                 {QStringLiteral("text"), prompt}});
    QJsonArray images;
    if (const QJsonObject refused =
            attachMessageMedia(*session, payload, localClient, input, images);
        !refused.isEmpty())
        return refused;
    // A steer can wait behind a running tool. Send image bytes to Codex now rather
    // than a temporary path that may disappear before queued guidance is consumed.
    // Claude's sidecar already converts its image paths to inline image blocks.
    if (!claude) {
        for (qsizetype i = 0; i < input.size(); ++i) {
            const QJsonObject block = input.at(i).toObject();
            if (block.value("type").toString() != "localImage")
                continue;
            QFile image(block.value("path").toString());
            if (!image.open(QIODevice::ReadOnly) || image.size() > McpMediaStore::maximumImageBytes)
                return error("attachment_unreadable", "An attached image could not be read.");
            const QByteArray bytes = image.readAll();
            if (bytes.isEmpty())
                return error("attachment_unreadable", "An attached image is empty.");
            const QString mime = bytes.startsWith("\x89PNG") ? "image/png" : "image/jpeg";
            input[i] = QJsonObject{
                {"type", "image"},
                {"url", "data:" + mime + ";base64," + QString::fromLatin1(bytes.toBase64())}};
        }
    }
    const QString sessionId = session->stored.id;
    const qint64 generation = session->stored.generation;
    const QString commandId = request.value(QStringLiteral("message_id")).toString();
    // The turn the text was meant for: the provider may answer after that turn ended and
    // another began, and the outcome must name the turn it belongs to.
    const QString turnId = session->stored.turnId;
    const qint64 turnSequence = session->stored.currentTurnSequence;
    const QJsonObject turn{{QStringLiteral("command_id"), commandId},
                           {QStringLiteral("turn_id"), turnId},
                           {QStringLiteral("turn_sequence"), turnSequence}};
    const auto answered = std::make_shared<bool>(false);
    if (outcome) {
        // Written before the provider sees the text: a daemon that restarts before the answer
        // is recorded replays this instead of steering a second time.
        journal_->replaceCommandResult(
            commandId, error(QStringLiteral("steer_outcome_unknown"),
                             QStringLiteral("The Mac did not learn whether the agent took this "
                                            "message. Check the conversation before sending it "
                                            "again.")));
        QTimer::singleShot(steerOutcomeTimeoutMs, this, [this, commandId, answered, outcome] {
            if (std::exchange(*answered, true))
                return;
            outcome(journal_->commandResult(commandId));
        });
    }
    auto finish = [this, sessionId, generation, displayText, images, commandId, turn, answered,
                   outcome, agentOrigin](const QJsonObject&, const QString& providerError) {
        QJsonObject result;
        if (providerError.isEmpty()) {
            result = ok({{QStringLiteral("accepted"), true}, {QStringLiteral("applied"), true}});
        } else {
            result = error(QStringLiteral("steer_rejected"), providerError);
        }
        for (auto it = turn.constBegin(); it != turn.constEnd(); ++it)
            result.insert(it.key(), it.value());
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end() && it->stored.generation == generation) {
            // Journal the steer text so restored transcripts keep it. A failed steer
            // leaves the turn running, so it must not be reported as a runtime error.
            QJsonObject payload = providerError.isEmpty()
                                      ? QJsonObject{{QStringLiteral("text"), displayText}}
                                      : QJsonObject{{QStringLiteral("message"), providerError}};
            if (providerError.isEmpty() && !images.isEmpty())
                payload.insert(QStringLiteral("images"), images);
            // The provider that ran the steered turn, so history keeps naming it after the
            // conversation switches provider.
            if (providerError.isEmpty())
                payload.insert(QStringLiteral("provider"), it->stored.adapter);
            if (providerError.isEmpty())
                for (auto field = agentOrigin.constBegin(); field != agentOrigin.constEnd();
                     ++field)
                    payload.insert(field.key(), field.value());
            for (auto field = turn.constBegin(); field != turn.constEnd(); ++field)
                payload.insert(field.key(), field.value());
            appendEvent(*it,
                        providerError.isEmpty() ? QStringLiteral("turn.steered")
                                                : QStringLiteral("turn.steer_failed"),
                        payload);
        }
        if (!outcome)
            return;
        journal_->replaceCommandResult(commandId, result);
        if (!std::exchange(*answered, true))
            outcome(result);
    };
    if (agentOrigin.isEmpty())
        addAgentContext(*session, displayText, input, false);
    if (claude) {
        provider(QStringLiteral("claude"))
            ->call(QStringLiteral("turn/steer"),
                   {{QStringLiteral("sessionId"), sessionId},
                    {QStringLiteral("prompt"), promptText(input)},
                    {QStringLiteral("images"), imagePaths(input)}},
                   std::move(finish));
    } else {
        adapter_->call(QStringLiteral("turn/steer"),
                       {{QStringLiteral("threadId"), session->stored.threadId},
                        {QStringLiteral("expectedTurnId"), turnId},
                        {QStringLiteral("input"), input}},
                       std::move(finish));
    }
    if (outcome)
        return {};
    QJsonObject accepted = ok({{QStringLiteral("accepted"), true}});
    for (auto it = turn.constBegin(); it != turn.constEnd(); ++it)
        accepted.insert(it.key(), it.value());
    return accepted;
}

void SessionHost::handleCommandDeferred(const QJsonObject& request, const QString& clientId,
                                        CommandCallback done) {
    if (request.value("message_type") == "session.configuration.read" &&
        request.value("payload").toObject().value("include_claude_usage").toBool()) {
        readClaudeAccountUsage(request, clientId, std::move(done));
        return;
    }
    if (request.value(QStringLiteral("message_type")).toString() ==
        QStringLiteral("session.media.read")) {
        mediaReadDeferred(request, std::move(done));
        return;
    }
    if (isBackgroundTaskCommand(request.value(QStringLiteral("message_type")).toString())) {
        backgroundTaskCommand(request, std::move(done));
        return;
    }
    // ADR 0114 §10: the disk warning and a done worktree's removal answer once measured or
    // removed. Only for the local client: handleCommand refuses team.* from any other.
    if (localClient(clientId)) {
        const QString type = request.value(QStringLiteral("message_type")).toString();
        if (type == QStringLiteral("team.storage.get") ||
            type == QStringLiteral("team.worktree.remove")) {
            teamStorageCommand(request, std::move(done));
            return;
        }
    }
    // ADR 0114 §1: what a team command asks git is read off the event loop first: the
    // repositories it names, then a task's worktree or its hand to review. Only for the
    // local client: handleCommand refuses any other before it asks anything.
    if (localClient(clientId) && request.value(QStringLiteral("message_type"))
                                     .toString()
                                     .startsWith(QStringLiteral("team."))) {
        auto run = [this, request, clientId, done = std::move(done)] {
            prepareTeamTask(request, [this, request, clientId, done] {
                done(handleCommand(request, clientId));
            });
        };
        learnRepositories(teamCommandPaths(request.value(QStringLiteral("payload")).toObject()),
                          std::move(run));
        return;
    }
    if (request.value(QStringLiteral("message_type")).toString() !=
        QStringLiteral("session.steer")) {
        done(handleCommand(request, clientId));
        return;
    }
    // A redelivered steer answers with what the first delivery recorded: the provider's
    // answer, or steer_outcome_unknown when the daemon stopped before it came.
    const QString messageId = request.value(QStringLiteral("message_id")).toString();
    if (const QJsonObject cached = journal_->commandResult(messageId); !cached.isEmpty()) {
        done(cached);
        return;
    }
    const QJsonObject immediate = steer(request, done, localClient(clientId));
    if (!immediate.isEmpty()) {
        journal_->storeCommandResult(messageId, immediate);
        done(immediate);
    }
}

QJsonObject SessionHost::interrupt(const QJsonObject& request) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty()) {
        return failure;
    }
    const QString sessionId = session->stored.id;
    const qint64 generation = session->stored.generation;
    session->stopRequested = session->startingTurn || !session->stored.turnId.isEmpty() ||
                             session->stored.state == QStringLiteral("running");
    QTimer::singleShot(0, approvals_, [this, sessionId, generation] {
        approvals_->cancelPending(sessionId, generation);
    });
    if (session->stored.adapter == QStringLiteral("claude") ||
        session->stored.adapter == QStringLiteral("cursor") ||
        session->stored.adapter == QStringLiteral("copilot")) {
        if (!session->startingTurn && session->stored.turnId.isEmpty() &&
            session->stored.state != QStringLiteral("running")) {
            return error(QStringLiteral("turn_not_running"),
                         QStringLiteral("The session has no active turn to interrupt."));
        }
        ProviderAdapter* const target = provider(session->stored.adapter);
        if (target == nullptr) {
            return error(QStringLiteral("provider_unavailable"),
                         missingProviderMessage(session->stored.adapter));
        }
        // Claude and ACP providers first create a provider-native session.  There is no native turn
        // to cancel during that short interval, so remember Stop and send it just after the adapter
        // announces turn.started.  Returning accepted keeps the Desktop control responsive.
        if (session->startingTurn) {
            session->interruptPending = true;
            return ok({{QStringLiteral("accepted"), true}});
        }
        const auto finish = [this, sessionId, generation](const QJsonObject&,
                                                          const QString& providerError) {
            auto it = sessions_.find(sessionId);
            if (it != sessions_.end() && it->stored.generation == generation &&
                !providerError.isEmpty()) {
                appendEvent(*it, QStringLiteral("turn.interrupt_failed"),
                            {{QStringLiteral("message"), providerError}});
            }
        };
        target->call(QStringLiteral("turn/interrupt"), {{QStringLiteral("sessionId"), sessionId}},
                     finish);
        return ok({{QStringLiteral("accepted"), true}});
    }
    // Codex can likewise be between turn/start and its turn/started notification. The daemon
    // session id remains stable in that interval, so retain Stop instead of rejecting it.
    if (session->startingTurn) {
        session->interruptPending = true;
        return ok({{QStringLiteral("accepted"), true}});
    }
    if (session->stored.threadId.isEmpty() || session->stored.turnId.isEmpty()) {
        return error(QStringLiteral("turn_not_running"),
                     QStringLiteral("The session has no active turn to interrupt."));
    }
    adapter_->call(QStringLiteral("turn/interrupt"),
                   {{QStringLiteral("threadId"), session->stored.threadId},
                    {QStringLiteral("turnId"), session->stored.turnId}},
                   [this, sessionId, generation](const QJsonObject&, const QString& providerError) {
                       auto it = sessions_.find(sessionId);
                       if (it != sessions_.end() && it->stored.generation == generation &&
                           !providerError.isEmpty()) {
                           // The turn keeps running when an interrupt fails.
                           appendEvent(*it, QStringLiteral("turn.interrupt_failed"),
                                       {{QStringLiteral("message"), providerError}});
                       }
                   });
    return ok({{QStringLiteral("accepted"), true}});
}

QJsonObject SessionHost::resolveApproval(const QJsonObject& request) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty()) {
        return failure;
    }
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    QString approvalId = payload.value(QStringLiteral("approval_id")).toString();
    if (approvalId.isEmpty())
        approvalId = payload.value(QStringLiteral("request_id")).toString();
    QJsonObject resolution = payload.value(QStringLiteral("resolution")).toObject();
    if (resolution.isEmpty()) {
        // Cloud commands carry the provider-neutral resolution directly in the
        // encrypted body. Local desktop callers retain the nested v0 shape.
        resolution = payload;
    }
    const ApprovalResolution result = approvals_->resolve(
        approvalId, session->stored.id, session->stored.generation,
        request.value(QStringLiteral("message_type")) == QStringLiteral("question.answer")
            ? QStringLiteral("input")
            : QStringLiteral("approval"),
        resolution);
    if (!result.accepted) {
        return error(result.errorCode, result.message);
    }
    const bool deferred = result.resultCode == QStringLiteral("deferred");
    const bool input =
        request.value(QStringLiteral("message_type")) == QStringLiteral("question.answer");
    if (result.resultCode != QStringLiteral("already_deferred")) {
        appendEvent(*session,
                    deferred ? QStringLiteral("question.deferred")
                    : input  ? QStringLiteral("question.answered")
                             : QStringLiteral("approval.resolved"),
                    {{QStringLiteral("approval_id"), approvalId},
                     {QStringLiteral("request_id"), resolution.value(QStringLiteral("request_id"))},
                     {QStringLiteral("revision"), resolution.value(QStringLiteral("revision"))},
                     {QStringLiteral("answers"), result.transcriptAnswers},
                     {QStringLiteral("questions"), result.transcriptQuestions}});
    }
    return ok(
        {{QStringLiteral("accepted"), true}, {QStringLiteral("result_code"), result.resultCode}});
}

QJsonObject SessionHost::acknowledge(const QJsonObject& request, const QString& clientId) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty()) {
        return failure;
    }
    const qint64 cursor = request.value(QStringLiteral("payload"))
                              .toObject()
                              .value(QStringLiteral("cursor"))
                              .toInteger(-1);
    if (cursor < 0 || cursor > session->stored.lastCursor) {
        return error(QStringLiteral("invalid_cursor"), QStringLiteral("Invalid cursor."));
    }
    if (!journal_->acknowledgeCursor(clientId, session->stored.id, session->stored.generation,
                                     cursor)) {
        return error(QStringLiteral("journal_error"), journal_->errorString());
    }
    return ok({{QStringLiteral("acknowledged_cursor"), cursor}});
}

QJsonObject SessionHost::checkedSession(const QJsonObject& request, Session** session) {
    const QString sessionId = request.value(QStringLiteral("session_id")).toString();
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) {
        return error(QStringLiteral("session_not_found"), QStringLiteral("Unknown session."));
    }
    const qint64 generation = request.value(QStringLiteral("session_generation")).toInteger(-1);
    if (generation != it->stored.generation) {
        return error(QStringLiteral("stale_generation"),
                     QStringLiteral("The command targets an old session generation."));
    }
    *session = &it.value();
    return {};
}

void SessionHost::handleProviderNotification(const QString& method, const QJsonObject& params) {
    // The account's limits, not one thread's: every Codex conversation mid-turn records them,
    // as the Claude sidecar does for its own, so a phone reading the journal sees them too.
    if (method == QStringLiteral("account/rateLimits/updated")) {
        const QJsonArray rows =
            planLimitRows({{QStringLiteral("limits"), codexPlanLimits(params)}});
        if (rows.isEmpty())
            return;
        for (Session& session : sessions_) {
            if (session.stored.adapter == QStringLiteral("codex") &&
                (session.startingTurn || session.stored.state == QStringLiteral("running")))
                appendEvent(session, QStringLiteral("usage.limits"),
                            {{QStringLiteral("limits"), rows},
                             {QStringLiteral("provider"), QStringLiteral("codex")}});
        }
        return;
    }
    Session* session = sessionForThread(threadIdFrom(params));
    if (!acceptProviderNotification(session, method, params) || session == nullptr) {
        return;
    }
    const QString eventTurnId =
        params.value("turnId").toString(params.value("turn").toObject().value("id").toString());
    if (method == QStringLiteral("turn/started") && !eventTurnId.isEmpty() &&
        (session->startingTurn || session->stored.turnId == eventTurnId) &&
        !session->turnContexts.contains(eventTurnId) && session->submittedContextRevision > 0) {
        session->turnContexts.insert(eventTurnId, session->submittedContextRevision);
    }
    auto eventContext = session->turnContexts.value(eventTurnId, 0);
    if (!eventTurnId.isEmpty() && eventContext == 0) {
        eventContext = journal_->turnContextRevision(session->stored.id, session->stored.generation,
                                                     eventTurnId);
        if (eventContext > 0)
            session->turnContexts.insert(eventTurnId, eventContext);
    }
    auto appendProviderEvent = [&](Session& target, const QString& type, QJsonObject payload) {
        payload.insert("provenance", eventContext > 0 ? "turn_context" : "unknown");
        if (!eventTurnId.isEmpty())
            payload.insert("turn_id", eventTurnId);
        if (eventContext > 0)
            payload.insert("context_revision", eventContext);
        // A turn default is not evidence of a tool's actual working directory.
        if (type == "item.started" || type == "item.completed") {
            payload.insert("resource_provenance", "unknown");
            // Only absolute provider-reported file paths establish origin. Relative
            // paths and commands may have changed cwd and remain unknown.
            const auto item = payload.value("item").toObject();
            QJsonArray references;
            if (eventContext > 0 &&
                (item.value("type") == "fileChange" || item.value("type") == "agentMessage")) {
                QString root;
                for (const auto& value :
                     journal_->sessionContexts(target.stored.id, target.stored.generation)) {
                    const auto context = value.toObject();
                    if (context.value("context_revision").toInteger() == eventContext)
                        root = QFileInfo(context.value("worktree_path").toString())
                                   .canonicalFilePath();
                }
                QStringList reportedPaths;
                for (const auto& value : item.value("changes").toArray())
                    reportedPaths.append(value.toObject().value("path").toString());
                if (item.value("type") == "agentMessage") {
                    static const QRegularExpression links(
                        QStringLiteral(R"(\]\(<?(/[^)<>\r\n]+)>?\))"));
                    auto matches = links.globalMatch(item.value("text").toString());
                    while (matches.hasNext()) {
                        auto path = matches.next().captured(1);
                        path.remove(QRegularExpression(QStringLiteral(R"((:\d+){1,2}$)")));
                        reportedPaths.append(path);
                    }
                }
                for (const auto& path : reportedPaths) {
                    const auto canonical = QFileInfo(path).canonicalFilePath();
                    if (!root.isEmpty() && QFileInfo(path).isAbsolute() &&
                        canonical.startsWith(root + '/')) {
                        references.append(
                            QJsonObject{{"context_revision", eventContext},
                                        {"path", QDir(root).relativeFilePath(canonical)}});
                    }
                }
            }
            if (!references.isEmpty()) {
                payload.insert("resource_provenance", "explicit_path");
                payload.insert("resource_refs", references);
            }
        }
        const auto cursor = appendEvent(target, type, payload);
        if (cursor > 0)
            recordWorktreeActivity(target, type, payload, cursor);
        return cursor;
    };
    if (method == QStringLiteral("thread/name/updated")) {
        const QString providerTitle =
            params.value(QStringLiteral("threadName")).toString().trimmed();
        if (!providerTitle.isEmpty()) {
            if (providerTitle != session->stored.title)
                (void)updateSessionTitle(*session, conversation_title::suggest(providerTitle),
                                         true);
        } else {
            refreshProviderTitle(session->stored.id, session->stored.generation);
        }
        return;
    }
    if (method == QStringLiteral("thread/tokenUsage/updated") ||
        method == QStringLiteral("codex/event/token_count")) {
        const QJsonObject usage = tokenUsageFromNotification(method, params);
        if (!usage.isEmpty()) {
            appendProviderEvent(*session, QStringLiteral("usage.context"),
                                {{QStringLiteral("tokenUsage"), usage}});
        }
        return;
    }
    if (method == QStringLiteral("turn/started")) {
        const QString startedTurnId =
            params.value(QStringLiteral("turn")).toObject().value(QStringLiteral("id")).toString();
        // Its completion already arrived: the turn is over, not running, and a Stop kept for
        // it has nothing left to stop.
        if (!startedTurnId.isEmpty() && startedTurnId == session->completedTurnId) {
            session->startingTurn = false;
            session->interruptPending = false;
            return;
        }
        session->stored.turnId = startedTurnId;
        session->stored.state = QStringLiteral("running");
        session->startingTurn = false;
        journal_->upsertSession(session->stored);
        appendProviderEvent(*session, QStringLiteral("turn.started"),
                            {{QStringLiteral("turn_id"), session->stored.turnId}});
        if (std::exchange(session->interruptPending, false)) {
            const QString sessionId = session->stored.id;
            const qint64 generation = session->stored.generation;
            QTimer::singleShot(0, this, [this, sessionId, generation] {
                auto current = sessions_.find(sessionId);
                if (current == sessions_.end() || current->stored.generation != generation ||
                    current->stored.state != QStringLiteral("running")) {
                    return;
                }
                (void)interrupt({{QStringLiteral("session_id"), sessionId},
                                 {QStringLiteral("session_generation"), generation}});
            });
        }
        return;
    }
    if (method == QStringLiteral("item/agentMessage/delta")) {
        appendProviderEvent(
            *session, QStringLiteral("assistant.delta"),
            {{QStringLiteral("item_id"), params.value(QStringLiteral("itemId")).toString()},
             {QStringLiteral("delta"), params.value(QStringLiteral("delta")).toString()}});
        return;
    }
    if (method == QStringLiteral("item/commandExecution/outputDelta")) {
        appendProviderEvent(
            *session, QStringLiteral("command.output_delta"),
            {{QStringLiteral("item_id"), params.value(QStringLiteral("itemId")).toString()},
             {QStringLiteral("delta"), params.value(QStringLiteral("delta")).toString()}});
        return;
    }
    if (method == QStringLiteral("item/started") || method == QStringLiteral("item/completed")) {
        // User input is already journalled as user.message / turn.steered with media
        // references. Provider echoes must not duplicate it or persist inline image bytes.
        if (params.value("item").toObject().value("type") == "userMessage")
            return;
        // Inline MCP media never reaches the journal (or, from there, cloud and clients).
        const QJsonObject item = withCommandDescription(withWorktreeChangePaths(
            mediaStore_.externalizeItem(params.value(QStringLiteral("item")).toObject(),
                                        session->stored.id, session->stored.worktreePath),
            session->stored.worktreePath));
        QJsonObject payload{
            {QStringLiteral("item_id"), item.value(QStringLiteral("id")).toString()},
            {QStringLiteral("item_type"), item.value(QStringLiteral("type")).toString()},
            {QStringLiteral("item"), item}};
        for (const QString& key :
             {QStringLiteral("text"), QStringLiteral("command"), QStringLiteral("status")}) {
            if (item.contains(key)) {
                payload.insert(key, item.value(key));
            }
        }
        appendProviderEvent(*session,
                            method == QStringLiteral("item/started")
                                ? QStringLiteral("item.started")
                                : QStringLiteral("item.completed"),
                            payload);
        noteBrowserToolItem(*session, item);
        if (method == QStringLiteral("item/completed") &&
            (eventTurnId.isEmpty() || eventTurnId == session->stored.turnId))
            registerAssistantQuestions(*session, item);
        return;
    }
    if (method == QStringLiteral("turn/completed")) {
        const QJsonObject turn = params.value(QStringLiteral("turn")).toObject();
        // A late or repeated completion of another turn does not end the active one, nor
        // release its attachments, browser sessions or worktree. A completion that arrives
        // before turn/started (no active id yet) is still this turn's.
        const QString completedTurnId = turn.value(QStringLiteral("id")).toString();
        if (!completedTurnId.isEmpty() &&
            ((!session->stored.turnId.isEmpty() && completedTurnId != session->stored.turnId) ||
             completedTurnId == session->completedTurnId)) {
            return;
        }
        session->completedTurnId = completedTurnId;
        abandonTurnApprovals(session->stored.id);
        session->stored.turnId.clear();
        session->stored.state = turn.value(QStringLiteral("status")).toString();
        if (session->stored.state.isEmpty() ||
            session->stored.state == QStringLiteral("completed")) {
            session->stored.state = QStringLiteral("ready");
        }
        journal_->upsertSession(session->stored);
        appendProviderEvent(
            *session, QStringLiteral("turn.completed"),
            turnCompletion(*session, turn.value(QStringLiteral("status")).toString()));
        for (const QString& path : std::exchange(session->materializedAttachments, {}))
            QFile::remove(path);
        for (const QString& id : std::exchange(session->stagedAttachmentIds, {}))
            (void)journal_->cancelAttachment(session->stored.id, session->stored.generation, id, 1);
        closeTurnBrowserSessions(*session);
        refreshProviderTitle(session->stored.id, session->stored.generation);
        followRecreatedWorktree(*session);
        return;
    }
    if (method == QStringLiteral("error")) {
        // A final error ends the turn even when turn/completed never follows, so its approvals
        // end with it, before the error is published, and later requests for it are declined.
        // A retried error, or one naming an earlier turn, leaves the running turn alone.
        const QString turnId = params.value(QStringLiteral("turnId")).toString();
        if (!params.value(QStringLiteral("willRetry")).toBool() &&
            (turnId.isEmpty() || turnId == session->stored.turnId)) {
            session->failedTurnId = session->stored.turnId;
            abandonTurnApprovals(session->stored.id);
        }
        const QJsonObject providerError = params.value(QStringLiteral("error")).toObject();
        QJsonObject failure{
            {QStringLiteral("message"), providerError.value(QStringLiteral("message")).toString()}};
        // Codex's own reason, which runtime.error's `code` is classified from first (ADR 0110).
        const QJsonValue info = providerError.value(QStringLiteral("codexErrorInfo"));
        if (!info.isUndefined() && !info.isNull())
            failure.insert(QStringLiteral("provider_info"), info);
        appendProviderEvent(*session, QStringLiteral("runtime.error"), failure);
        for (const QString& path : std::exchange(session->materializedAttachments, {}))
            QFile::remove(path);
        for (const QString& id : std::exchange(session->stagedAttachmentIds, {}))
            (void)journal_->cancelAttachment(session->stored.id, session->stored.generation, id, 1);
    }
}

bool SessionHost::updateSessionTitle(Session& session, const QString& title,
                                     const bool customTitle) {
    if (title.isEmpty() || session.stored.title == title) {
        return true;
    }
    const QString previousTitle = session.stored.title;
    session.stored.title = title;
    if (!journal_->upsertSession(session.stored)) {
        session.stored.title = previousTitle;
        return false;
    }
    appendEvent(session, QStringLiteral("session.renamed"),
                {{QStringLiteral("title"), title}, {QStringLiteral("custom_title"), customTitle}});
    return true;
}

void SessionHost::syncProviderTitle(Session& session) {
    if (session.stored.adapter != QStringLiteral("codex") || session.stored.threadId.isEmpty() ||
        session.pendingProviderTitle.isEmpty()) {
        return;
    }
    const QString sessionId = session.stored.id;
    const qint64 generation = session.stored.generation;
    const QString title = session.pendingProviderTitle;
    const QPointer<SessionHost> self(this);
    adapter_->call(
        QStringLiteral("thread/name/set"),
        {{QStringLiteral("threadId"), session.stored.threadId}, {QStringLiteral("name"), title}},
        [self, sessionId, generation, title](const QJsonObject&, const QString& providerError) {
            if (!self || !providerError.isEmpty()) {
                return;
            }
            auto current = self->sessions_.find(sessionId);
            if (current != self->sessions_.end() && current->stored.generation == generation &&
                current->pendingProviderTitle == title) {
                current->pendingProviderTitle.clear();
            }
        });
}

void SessionHost::refreshProviderTitle(const QString& sessionId, const qint64 generation) {
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || it->stored.generation != generation ||
        it->stored.threadId.isEmpty() || it->stored.adapter != QStringLiteral("codex")) {
        return;
    }
    const QString expectedTitle = it->stored.title;
    const QPointer<SessionHost> self(this);
    adapter_->call(
        QStringLiteral("thread/read"),
        {{QStringLiteral("threadId"), it->stored.threadId},
         {QStringLiteral("includeTurns"), false}},
        [self, sessionId, generation, expectedTitle](const QJsonObject& result,
                                                     const QString& providerError) {
            if (!self || !providerError.isEmpty()) {
                return;
            }
            auto current = self->sessions_.find(sessionId);
            if (current == self->sessions_.end() || current->stored.generation != generation ||
                current->stored.title != expectedTitle) {
                return;
            }
            const QJsonObject thread = result.value(QStringLiteral("thread")).toObject();
            const QString providerName = thread.value(QStringLiteral("name")).toString().trimmed();
            const bool customTitle = !providerName.isEmpty();
            const QString candidate =
                customTitle ? providerName : thread.value(QStringLiteral("preview")).toString();
            // A team member keeps its member name, and no conversation is titled from the
            // envelope of an agent message a turn started with (T-26).
            if (!customTitle &&
                (self->onTeam(sessionId) || agent_messaging::isDeliveryText(candidate)))
                return;
            if (!candidate.trimmed().isEmpty() && candidate != current->stored.title) {
                (void)self->updateSessionTitle(*current, conversation_title::suggest(candidate),
                                               customTitle);
            }
        });
}

void SessionHost::handleProviderRequest(const QJsonValue& requestId, const QString& method,
                                        const QJsonObject& params) {
    Session* session = sessionForThread(threadIdFrom(params));
    if (session == nullptr) {
        adapter_->respond(requestId, codex_requests::declinedResult(method));
        return;
    }
    // A request naming a turn that already ended would outlive the sweep at its completion and
    // be answered during the next turn. A request without a turn id is the current turn's, which
    // a final error may have ended.
    if (const QString turnId = params.value(QStringLiteral("turnId")).toString();
        (!session->failedTurnId.isEmpty() &&
         (turnId.isEmpty() ? session->stored.turnId : turnId) == session->failedTurnId) ||
        (!turnId.isEmpty() &&
         (turnId == session->completedTurnId ||
          (!session->stored.turnId.isEmpty() && turnId != session->stored.turnId)))) {
        declineProviderRequest(*session, requestId, method, QStringLiteral("stale_turn"),
                               params.value(QStringLiteral("serverName")).toString());
        return;
    }
    // Only a request the broker holds is answered through routeProviderResponse/Error.
    const auto admit = [this, session, &requestId](const QString& kind,
                                                   const QJsonObject& details) {
        pendingProviderByRequest_.insert(requestKey(requestId), QStringLiteral("codex"));
        (void)approvals_->request(session->stored.id, session->stored.generation, requestId, kind,
                                  details);
    };
    if (method == QStringLiteral("mcpServer/elicitation/request")) {
        const QString serverName = params.value(QStringLiteral("serverName")).toString();
        if (!codex_requests::isMcpToolApproval(params)) {
            declineProviderRequest(*session, requestId, method,
                                   QStringLiteral("unsupported_elicitation"), serverName);
            return;
        }
        // Browser tools never prompt (ADR 0008). The launch config already sets
        // `default_tools_approval_mode = "approve"`; this covers a Codex that ignores it. A
        // `ketplus_browser` server KetPlus did not inject (user config) gets a card like any other.
        if (codex_requests::isInjectedBrowserToolApproval(params,
                                                          adapter_->launchedBrowserArguments())) {
            adapter_->respond(requestId, codex_requests::mcpElicitationResult(true));
            return;
        }
        // The thread's agent tools were injected with its own config (ADR 0027).
        if (serverName == QLatin1String(agent_tools::serverName) && !agentSocket_.isEmpty()) {
            adapter_->respond(requestId, codex_requests::mcpElicitationResult(true));
            return;
        }
        admit(QStringLiteral("approval"), codex_requests::mcpToolApprovalDetails(params));
        return;
    }
    if (method == QStringLiteral("item/commandExecution/requestApproval") ||
        method == QStringLiteral("item/fileChange/requestApproval")) {
        QJsonObject details{
            {QStringLiteral("request_type"),
             method == QStringLiteral("item/fileChange/requestApproval")
                 ? QStringLiteral("file_change")
                 : QStringLiteral("command")},
            {QStringLiteral("item_id"), params.value(QStringLiteral("itemId")).toString()},
            {QStringLiteral("reason"), params.value(QStringLiteral("reason")).toString()}};
        if (params.contains(QStringLiteral("command"))) {
            details.insert(QStringLiteral("command"), params.value(QStringLiteral("command")));
        }
        admit(QStringLiteral("approval"), details);
        return;
    }
    if (method == QStringLiteral("item/tool/requestUserInput")) {
        admit(QStringLiteral("input"),
              {{QStringLiteral("item_id"), params.value(QStringLiteral("itemId")).toString()},
               {QStringLiteral("is_blocking"),
                params.value(QStringLiteral("isBlocking")).toBool(true)},
               {QStringLiteral("questions"), params.value(QStringLiteral("questions"))}});
        return;
    }
    declineProviderRequest(*session, requestId, method, QStringLiteral("unsupported_request"), {});
}

void SessionHost::declineProviderRequest(Session& session, const QJsonValue& requestId,
                                         const QString& method, const QString& reason,
                                         const QString& serverName) {
    adapter_->respond(requestId, codex_requests::declinedResult(method));
    // Without this row Codex's own "user rejected" wording is the only explanation.
    QJsonObject payload{{QStringLiteral("method"), method}, {QStringLiteral("reason"), reason}};
    if (!serverName.isEmpty())
        payload.insert(QStringLiteral("server_name"), serverName);
    appendEvent(session, QStringLiteral("provider.request_declined"), payload);
}

QJsonObject SessionHost::attachMessageMedia(Session& session, const QJsonObject& payload,
                                            bool localClient, QJsonArray& input,
                                            QJsonArray& messageImages) {
    const QString sessionId = session.stored.id;
    const qint64 generation = session.stored.generation;
    // The provider only sees temporary copies of the message's images, removed once the turn
    // ends, and user.message carried the text alone: neither the desktop nor the phone could
    // show what was sent. Each image is kept in the media store and the message records its
    // reference, never its path, so every client can draw it.
    QJsonArray providerInput;
    for (const QJsonValue& value : std::as_const(input)) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() != QStringLiteral("localImage")) {
            providerInput.append(value);
            continue;
        }
        // A remote client never saw the Mac's files: a path it names would hand the agent, and
        // back through the media store the phone, a file nobody chose to send.
        if (!localClient)
            continue;
        providerInput.append(value);
        if (messageImages.size() >= 5)
            continue;
        QFile file(block.value(QStringLiteral("path")).toString());
        if (!file.open(QIODevice::ReadOnly) || file.size() > McpMediaStore::maximumImageBytes)
            continue;
        const QByteArray bytes = file.readAll();
        const QString mimeType = bytes.startsWith("\x89PNG") ? QStringLiteral("image/png")
                                                             : QStringLiteral("image/jpeg");
        QJsonObject stored = mediaStore_.storeImage(bytes, mimeType, sessionId);
        if (stored.isEmpty())
            continue;
        stored.insert(QStringLiteral("name"), QFileInfo(file.fileName()).fileName().left(255));
        messageImages.append(stored);
    }
    input = providerInput;
    const QJsonArray attachmentIds = payload.value(QStringLiteral("attachment_ids")).toArray();
    if (attachmentIds.size() > 5)
        return error(QStringLiteral("attachment_limit"),
                     QStringLiteral("No more than five attachments may be submitted."));
    QSet<QString> uniqueAttachmentIds;
    qint64 attachmentBytes = 0;
    for (const QJsonValue& value : attachmentIds) {
        const QString attachmentId = value.toString();
        if (attachmentId.isEmpty() || uniqueAttachmentIds.contains(attachmentId))
            return error(QStringLiteral("invalid_attachment_reference"),
                         QStringLiteral("Attachment references must be unique."));
        uniqueAttachmentIds.insert(attachmentId);
        const StagedAttachment attachment =
            journal_->attachment(sessionId, generation, attachmentId);
        if (attachment.id.isEmpty() || attachment.status != QStringLiteral("ready") ||
            attachment.bytes.isEmpty())
            return error(QStringLiteral("attachment_not_ready"),
                         QStringLiteral("An attachment is not ready."));
        if (!QSet<QString>{QStringLiteral("text/plain"), QStringLiteral("image/png"),
                           QStringLiteral("image/jpeg")}
                 .contains(attachment.mediaType))
            return error(QStringLiteral("attachment_type_unsupported"),
                         QStringLiteral("This attachment type is not supported."));
        attachmentBytes += attachment.bytes.size();
        if (attachmentBytes > 40 * 1024 * 1024)
            return error(QStringLiteral("attachment_limit"),
                         QStringLiteral("The attachments exceed the conversation limit."));
    }
    // A steer adds to a turn that already holds its own message's attachments: a failure here
    // must leave those alone.
    QStringList claimed;
    QStringList copies;
    const auto drop = [&](bool cancel) {
        for (const QString& path : std::as_const(copies))
            QFile::remove(path);
        if (cancel)
            for (const QString& id : std::as_const(claimed))
                (void)journal_->cancelAttachment(sessionId, generation, id, 1);
    };
    for (const QJsonValue& value : attachmentIds) {
        const StagedAttachment attachment =
            journal_->attachment(sessionId, generation, value.toString());
        if (attachment.id.isEmpty() || attachment.status != QStringLiteral("ready") ||
            attachment.bytes.isEmpty()) {
            drop(false);
            return error(QStringLiteral("attachment_not_ready"),
                         QStringLiteral("An attachment is not ready."));
        }
        claimed.append(attachment.id);
        if (attachment.mediaType == QStringLiteral("text/plain")) {
            input.append(
                QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                            {QStringLiteral("text"),
                             QStringLiteral("Attached file: %1\n<attachment>\n%2\n</attachment>")
                                 .arg(attachment.name, QString::fromUtf8(attachment.bytes))}});
            continue;
        }
        QJsonObject stored =
            mediaStore_.storeImage(attachment.bytes, attachment.mediaType, sessionId);
        if (!stored.isEmpty()) {
            stored.insert(QStringLiteral("name"), attachment.name.left(255));
            messageImages.append(stored);
        }
        const QString suffix = attachment.mediaType == QStringLiteral("image/png")
                                   ? QStringLiteral(".png")
                                   : QStringLiteral(".jpg");
        const QString path =
            attachmentDirectory_.isValid()
                ? attachmentDirectory_.filePath(
                      QString::fromLatin1(
                          QCryptographicHash::hash(
                              QStringLiteral("%1:%2").arg(sessionId, attachment.id).toUtf8(),
                              QCryptographicHash::Sha256)
                              .toHex()) +
                      suffix)
                : QString();
        QFile file(path);
        if (path.isEmpty() || !file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(attachment.bytes) != attachment.bytes.size()) {
            drop(true);
            return error(QStringLiteral("attachment_materialization_failed"),
                         QStringLiteral("The attachment could not be prepared."));
        }
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        file.close();
        copies.append(path);
        input.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("localImage")},
                                 {QStringLiteral("path"), path}});
    }
    session.stagedAttachmentIds.append(claimed);
    session.materializedAttachments.append(copies);
    return {};
}

void SessionHost::releaseTurnAttachments(Session& session) {
    for (const QString& path : std::exchange(session.materializedAttachments, {}))
        QFile::remove(path);
    for (const QString& id : std::exchange(session.stagedAttachmentIds, {}))
        (void)journal_->cancelAttachment(session.stored.id, session.stored.generation, id, 1);
}

bool SessionHost::journalEvent(Session& session, const QString& type, const QJsonObject& payload,
                               QString* error) {
    QString appendError;
    auto contextualPayload = payload;
    // Pin the turn's default at submission. Do not infer a tool's root (or the
    // context of a late provider notification) from the current selection.
    if (type == QStringLiteral("user.message") || type == QStringLiteral("turn.queued")) {
        if (!journal_->recordSessionContext(session.stored)) {
            if (error)
                *error = journal_->errorString();
            return false;
        }
        session.submittedContextRevision = session.stored.contextRevision;
        contextualPayload.insert(QStringLiteral("context_revision"),
                                 session.submittedContextRevision);
    }
    const qint64 cursor = journal_->appendEvent(session.stored.id, session.stored.generation, type,
                                                contextualPayload, &appendError);
    if (cursor < 0) {
        if (appendError.isEmpty())
            appendError = QStringLiteral("The local journal rejected the event.");
        if (session.lastJournalError != appendError) {
            qWarning("Unable to journal %s for session %s: %s", qUtf8Printable(type),
                     qUtf8Printable(session.stored.id), qUtf8Printable(appendError));
            session.lastJournalError = appendError;
        }
        if (error != nullptr)
            *error = appendError;
        return false;
    }
    session.lastJournalError.clear();
    const QString sessionId = session.stored.id;
    const qint64 generation = session.stored.generation;
    const QPointer<SessionHost> alive(this);
    session.stored.lastCursor = cursor;
    if (type == QStringLiteral("user.message")) {
        ++session.stored.currentTurnSequence;
    }
    const QJsonArray stored =
        journal_->eventsAfter(session.stored.id, session.stored.generation, cursor - 1, 1);
    if (!stored.isEmpty()) {
        const QJsonObject event = stored.first().toObject();
        // The journal stamped this time on the session row with the event.
        if (type == QStringLiteral("user.message"))
            session.stored.lastTurnStartedAt = event.value(QStringLiteral("sent_at")).toString();
        emit eventReady(event);
    }
    if (alive.isNull())
        return true;
    const auto current = sessions_.constFind(sessionId);
    if (current == sessions_.cend() || current->stored.generation != generation)
        return true;
    // Turn-scoped async questions must not survive completion, process failure or a new turn.
    // This emits the existing question.expired event, so live and replayed clients agree.
    if (type == "turn.completed" || type == "runtime.stopped" || type == "user.message")
        approvals_->expireAssistantQuestions(sessionId, generation);
    if (!alive.isNull() && agents_ != nullptr)
        agents_->noteEvent(sessionId, generation, type, contextualPayload);
    // ADR 0114 §7: the leader's digest waits for the end of its turn.
    if (!alive.isNull() && teams_ && (type == "turn.completed" || type == "runtime.stopped"))
        // Only a turn's end says whether the user stopped it; a worker that exits after
        // the turn says nothing about that.
        teams_->memberTurnEnded(
            sessionId,
            type == "turn.completed"
                ? std::optional<bool>(payload.value(QStringLiteral("reason")).toString() ==
                                      QStringLiteral("user"))
                : std::nullopt);
    return true;
}

SessionHost::Session* SessionHost::sessionForThread(const QString& threadId) {
    const QString sessionId = sessionsByThread_.value(threadId);
    auto it = sessions_.find(sessionId);
    return it == sessions_.end() ? nullptr : &it.value();
}

bool SessionHost::isMutating(const QString& messageType) {
    if (messageType.startsWith(QStringLiteral("team.")))
        return !messageType.endsWith(QStringLiteral(".list")) &&
               !messageType.endsWith(QStringLiteral(".get"));
    return messageType == QStringLiteral("session.agent_messages.release") ||
           messageType == QStringLiteral("session.reviewer.set") ||
           messageType == QStringLiteral("storage.size.apply") ||
           messageType == QStringLiteral("storage.retention.apply") ||
           messageType == QStringLiteral("storage.options.apply") ||
           messageType == QStringLiteral("storage.compaction.start") ||
           messageType == QStringLiteral("storage.export.start") ||
           messageType == QStringLiteral("session.worktree.switch") ||
           messageType == QStringLiteral("session.start_turn") ||
           messageType == QStringLiteral("session.rename") ||
           messageType == QStringLiteral("session.mark_seen") ||
           messageType == QStringLiteral("session.set_archived") ||
           messageType == QStringLiteral("session.tags.set") ||
           messageType == QStringLiteral("tags.catalog.set") ||
           messageType == QStringLiteral("session.mark_done") ||
           messageType == QStringLiteral("session.delete") ||
           messageType == QStringLiteral("session.steer") ||
           messageType == QStringLiteral("session.interrupt") ||
           messageType == QStringLiteral("approval.resolve") ||
           messageType == QStringLiteral("question.answer") ||
           messageType == QStringLiteral("session.configuration.apply") ||
           messageType == QStringLiteral("session.execution_policy.apply") ||
           messageType == QStringLiteral("session.mode.apply") ||
           messageType == QStringLiteral("attachment.stage.begin") ||
           messageType == QStringLiteral("attachment.stage.chunk") ||
           messageType == QStringLiteral("attachment.stage.commit") ||
           messageType == QStringLiteral("attachment.stage.cancel") ||
           messageType == QStringLiteral("client.ack_cursor");
}

QString SessionHost::normalizedWorktree(const QString& path) {
    const QFileInfo info(path);
    if (!info.isDir()) {
        return {};
    }
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QString SessionHost::handoffDirectory(const QString& sessionId) const {
    return QFileInfo(journal_->databasePath())
        .dir()
        .filePath(QStringLiteral("handoff/%1").arg(sessionId));
}

void SessionHost::handleCanonicalEvent(const QString& provider, const QString& sessionId,
                                       const QString& type, const QJsonObject& eventPayload) {
    // Transcript rows say which provider wrote them.
    QJsonObject payload = eventPayload;
    payload.insert(QStringLiteral("provider"), provider);
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end() || type.isEmpty()) {
        return;
    }
    if (payload.value("item").isObject())
        payload.insert("item", mediaStore_.externalizeItem(payload.value("item").toObject(),
                                                           sessionId, it->stored.worktreePath));
    // Only an id the SDK issued is a thread: resuming anything else fails every later turn.
    const QString threadId = payload.value(QStringLiteral("thread_id")).toString();
    if (provider != it->stored.adapter) {
        // A run of the provider the conversation moved away from (kept open for background
        // tasks) still reports. Retain its ID for cleanup; never let it replace the current
        // provider's thread or state.
        QJsonObject threads = QJsonDocument::fromJson(it->stored.providerThreads.toUtf8()).object();
        QJsonObject entry = threads.value(provider).toObject();
        if (!threadId.isEmpty() && entry.value("thread_id").toString() != threadId) {
            provider_handoff::rememberThread(threads, provider, threadId);
            it->stored.providerThreads =
                QString::fromUtf8(QJsonDocument(threads).toJson(QJsonDocument::Compact));
            journal_->upsertSession(it->stored);
        }
        return;
    }
    if (!threadId.isEmpty() && it->stored.threadId != threadId) {
        const auto retained = QJsonDocument::fromJson(it->stored.providerThreads.toUtf8()).object();
        // A late notification from A's retired run cannot bind its old context after B -> A.
        if (provider_handoff::threadReferences({}, {}, retained)
                .contains(QJsonObject{{"provider", provider}, {"thread_id", threadId}}))
            return;
        if (!it->stored.threadId.isEmpty())
            sessionsByThread_.remove(it->stored.threadId);
        it->stored.threadId = threadId;
        sessionsByThread_.insert(threadId, sessionId);
        journal_->upsertSession(it->stored);
    }
    if (type == QStringLiteral("thread.started"))
        return; // Only names the thread; nothing for the transcript.
    if (type == QStringLiteral("usage.context")) {
        const QJsonObject usage = occupancyTokenUsage(payload);
        if (!usage.isEmpty()) {
            appendEvent(*it, QStringLiteral("usage.context"),
                        {{QStringLiteral("tokenUsage"), usage}});
        }
        return;
    }
    if (type == QStringLiteral("tasks.changed")) {
        recordBackgroundTasks(*it, payload.value(QStringLiteral("tasks")).toArray());
        return;
    }
    if (type == QStringLiteral("usage.session") && provider == QStringLiteral("claude")) {
        const QString mode = payload.value("mode").toString();
        if (mode != "api_key" && mode != "runtime")
            return;
        QJsonObject usage{{"mode", mode}};
        for (const char* field : {"budgetUsd", "estimatedCostUsd", "inputTokens", "outputTokens",
                                  "cacheReadInputTokens", "cacheCreationInputTokens"}) {
            const auto value = payload.value(QLatin1String(field));
            const double number = value.toDouble(-1);
            const bool tokens = QByteArray(field).endsWith("Tokens");
            if (value.isDouble() && std::isfinite(number) && number >= 0 &&
                (!tokens || (std::floor(number) == number && number <= 9007199254740991.0)))
                usage.insert(QLatin1String(field), value);
        }
        appendEvent(*it, type, usage);
        return;
    }
    if (type == QStringLiteral("usage.limits")) {
        const QJsonArray rows = planLimitRows(payload);
        if (!rows.isEmpty())
            appendEvent(*it, QStringLiteral("usage.limits"), {{QStringLiteral("limits"), rows}});
        return;
    }
    if (type == QStringLiteral("turn.started")) {
        it->stored.turnId = payload.value(QStringLiteral("turn_id")).toString();
        it->stored.state = QStringLiteral("running");
        it->startingTurn = false;
        it->attachedToProvider = true;
        journal_->upsertSession(it->stored);
        appendEvent(*it, QStringLiteral("turn.started"),
                    {{QStringLiteral("turn_id"), it->stored.turnId}});
        if (std::exchange(it->interruptPending, false)) {
            const qint64 generation = it->stored.generation;
            // Cursor emits turn.started immediately before it writes session/prompt. Queue the
            // interrupt so ACP receives cancel after that prompt rather than before it exists.
            QTimer::singleShot(0, this, [this, sessionId, generation] {
                auto current = sessions_.find(sessionId);
                if (current == sessions_.end() || current->stored.generation != generation ||
                    current->stored.state != QStringLiteral("running")) {
                    return;
                }
                (void)interrupt({{QStringLiteral("session_id"), sessionId},
                                 {QStringLiteral("session_generation"), generation}});
            });
        }
        return;
    }
    if (type == QStringLiteral("turn.completed")) {
        // A live background task keeps asking after its turn ends; its requests stay.
        if (!hasLiveBackgroundTasks(*it))
            abandonTurnApprovals(it->stored.id);
        it->stored.turnId.clear();
        const QString status = payload.value(QStringLiteral("status")).toString();
        it->stored.state =
            status == QStringLiteral("failed") ? QStringLiteral("failed") : QStringLiteral("ready");
        journal_->upsertSession(it->stored);
        appendEvent(*it, QStringLiteral("turn.completed"), turnCompletion(*it, status));
        for (const QString& path : std::exchange(it->materializedAttachments, {}))
            QFile::remove(path);
        for (const QString& id : std::exchange(it->stagedAttachmentIds, {}))
            (void)journal_->cancelAttachment(it->stored.id, it->stored.generation, id, 1);
        followRecreatedWorktree(*it);
        return;
    }
    if (type == QStringLiteral("runtime.error")) {
        appendEvent(*it, type, payload);
        for (const QString& path : std::exchange(it->materializedAttachments, {}))
            QFile::remove(path);
        for (const QString& id : std::exchange(it->stagedAttachmentIds, {}))
            (void)journal_->cancelAttachment(it->stored.id, it->stored.generation, id, 1);
        return;
    }
    appendEvent(*it, type, payload);
}

void SessionHost::handleCanonicalPermission(const QString& adapterName, const QJsonValue& requestId,
                                            const QJsonObject& params) {
    auto it = sessions_.find(params.value(QStringLiteral("sessionId")).toString());
    if (it == sessions_.end()) {
        if (ProviderAdapter* const target = provider(adapterName))
            target->respond(requestId, {{QStringLiteral("allow"), false}});
        return;
    }
    const QString tool = params.value(QStringLiteral("tool")).toString();
    const QString label = adapterName == QStringLiteral("copilot")
                              ? QStringLiteral("GitHub Copilot")
                          : adapterName == QStringLiteral("cursor") ? QStringLiteral("Cursor")
                                                                    : QStringLiteral("Claude");
    (void)approvals_->request(
        it->stored.id, it->stored.generation, taggedRequestId(adapterName, requestId),
        QStringLiteral("approval"),
        QJsonObject{
            {QStringLiteral("command"),
             toolCallSummary(tool, params.value(QStringLiteral("input")).toObject())},
            {QStringLiteral("reason"), QStringLiteral("%1 wants to use %2.").arg(label, tool)},
            {QStringLiteral("request_type"), adapterName + QStringLiteral("_tool")}});
}

void SessionHost::handleCanonicalQuestion(const QJsonValue& requestId, const QJsonObject& params,
                                          const QString& provider) {
    auto it = sessions_.find(params.value(QStringLiteral("sessionId")).toString());
    if (it == sessions_.end()) {
        if (ProviderAdapter* const target = this->provider(provider))
            target->respondError(requestId, "unknown_session", "Question has no active session.");
        return;
    }
    (void)approvals_->request(
        it->stored.id, it->stored.generation, taggedRequestId(provider, requestId),
        QStringLiteral("input"),
        QJsonObject{{QStringLiteral("item_id"), params.value(QStringLiteral("itemId"))},
                    {QStringLiteral("is_blocking"), true},
                    {QStringLiteral("questions"), params.value(QStringLiteral("questions"))}});
}

void SessionHost::routeProviderResponse(const QJsonValue& taggedId, const QJsonObject& result) {
    if (answerAssistantQuestions(taggedId, result))
        return;
    QString adapterName;
    QJsonValue requestId = taggedId;
    if (!untagRequestId(taggedId, adapterName, requestId))
        adapterName = pendingProviderByRequest_.take(requestKey(taggedId));
    // Claude and Cursor take answers as they are and decisions as allow/accepted.
    ProviderAdapter* const target = provider(adapterName);
    if (target != nullptr && target != adapter_) {
        if (adapterName == QStringLiteral("copilot") && result.value("cancelled").toBool()) {
            target->respond(requestId, result);
            return;
        }
        if (result.contains(QStringLiteral("answers"))) {
            target->respond(requestId, result);
            return;
        }
        const bool allow =
            result.value(QStringLiteral("decision")).toString() == QStringLiteral("accept");
        target->respond(requestId,
                        {{QStringLiteral("allow"), allow}, {QStringLiteral("accepted"), allow}});
        return;
    }
    adapter_->respond(requestId, result);
}

void SessionHost::forgetProviderRoutes(const QList<QJsonValue>& requestIds) {
    for (const QJsonValue& requestId : requestIds) {
        QString adapterName;
        QJsonValue untagged;
        if (!untagRequestId(requestId, adapterName, untagged))
            pendingProviderByRequest_.remove(requestKey(requestId));
    }
}

void SessionHost::routeProviderError(const QJsonValue& taggedId, const QString& code,
                                     const QString& message) {
    if (taggedId.toObject().value("ketplus_async_question").toBool())
        return;
    QString adapterName;
    QJsonValue requestId = taggedId;
    if (!untagRequestId(taggedId, adapterName, requestId))
        adapterName = pendingProviderByRequest_.take(requestKey(taggedId));
    ProviderAdapter* const target = provider(adapterName);
    (target != nullptr ? target : adapter_)->respondError(requestId, code, message);
}

} // namespace ketplus::daemon
