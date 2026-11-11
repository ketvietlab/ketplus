#include "runtime/SessionHost.h"

#include "runtime/ApprovalBroker.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

namespace ketplus::daemon {
namespace {

QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}

QByteArray git(const QString& directory, const QStringList& arguments) {
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (executable.isEmpty())
        return {};
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const QString& key : environment.keys()) {
        if (key.startsWith(QStringLiteral("GIT_")))
            environment.remove(key);
    }
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.start(executable, arguments);
    if (!process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    const auto output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        output.size() > 1024 * 1024)
        return {};
    return output;
}

QString commonDirectory(const QString& directory) {
    // An empty directory would run Git in the daemon's own working directory.
    if (directory.isEmpty())
        return {};
    auto output = git(directory, {"rev-parse", "--path-format=absolute", "--git-common-dir"});
    if (output.endsWith('\n'))
        output.chop(1);
    return output.isEmpty() ? QString{} : QFileInfo(QString::fromUtf8(output)).canonicalFilePath();
}

QJsonArray worktrees(const QString& root) {
    const auto output = git(root, {"worktree", "list", "--porcelain", "-z"});
    QJsonArray result;
    QJsonObject entry;
    bool unavailable = false;
    for (const auto& field : output.split('\0')) {
        if (field.isEmpty()) {
            if (!entry.isEmpty() && !unavailable &&
                QFileInfo(entry.value("worktree_path").toString()).isDir())
                result.append(entry);
            entry = {};
            unavailable = false;
        } else if (field.startsWith("worktree ")) {
            entry.insert("worktree_path",
                         QFileInfo(QString::fromUtf8(field.mid(9))).canonicalFilePath());
        } else if (field.startsWith("branch ")) {
            entry.insert("branch", QString::fromUtf8(field.mid(7)));
        } else if (field == "detached") {
            entry.insert("detached", true);
        } else if (field == "bare" || field.startsWith("prunable")) {
            unavailable = true;
        }
    }
    return result;
}

} // namespace

QJsonObject SessionHost::resolveResource(const QJsonObject& request) {
    Session* session = nullptr;
    const auto rejected = checkedSession(request, &session);
    if (!rejected.isEmpty())
        return rejected;
    const auto input = request.value("payload").toObject();
    const auto cursor = input.value("event_cursor").toInteger(-1);
    const auto path = input.value("path").toString();
    if (cursor < 1 || input.value("event_cursor").toDouble() != double(cursor) || path.isEmpty() ||
        path.size() > 4096)
        return failure("resource_context_unknown", "The file has no verified source.");
    const auto events =
        journal_->eventsAfter(session->stored.id, session->stored.generation, cursor - 1, 1);
    if (events.isEmpty() || events.first().toObject().value("cursor").toInteger() != cursor)
        return failure("resource_context_unknown", "The source event is unavailable.");
    const auto payload = events.first().toObject().value("payload").toObject();
    for (const auto& value : payload.value("resource_refs").toArray()) {
        const auto reference = value.toObject();
        const auto revision = reference.value("context_revision").toInteger();
        const auto relative = reference.value("path").toString();
        if (revision < 1 || relative.isEmpty() || QDir::isAbsolutePath(relative) ||
            QDir::cleanPath(relative).startsWith("../"))
            continue;
        for (const auto& contextValue :
             journal_->sessionContexts(session->stored.id, session->stored.generation)) {
            const auto context = contextValue.toObject();
            if (context.value("context_revision").toInteger() != revision)
                continue;
            const auto root =
                QFileInfo(context.value("worktree_path").toString()).canonicalFilePath();
            if (root.isEmpty() || (path != relative && path != QDir(root).filePath(relative)))
                continue;
            return {{"ok", true},
                    {"event_cursor", cursor},
                    {"path", relative},
                    {"resource_context_revision", revision},
                    {"worktree_path", root}};
        }
    }
    // A provider may read/edit another checkout without changing the turn's cwd.
    // Only paths present in this exact durable event authorize that source root.
    const auto revision = payload.value("context_revision").toInteger();
    const auto item = payload.value("item").toObject();
    QStringList reported;
    for (const auto& value : item.value("changes").toArray())
        reported.append(value.toObject().value("path").toString());
    const auto type = item.value("type").toString();
    if (type == "agentMessage") {
        // A reply can link another checkout without changing the turn's cwd.
        // Accept only destinations recorded in this exact durable message.
        static const QRegularExpression links(
            QStringLiteral(R"(\]\(\s*(?:<([^<>\r\n]+)>|([^\s()<>]+))\s*\))"));
        auto matches = links.globalMatch(item.value("text").toString());
        while (matches.hasNext()) {
            const auto match = matches.next();
            auto destination = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
            destination.remove(QRegularExpression(QStringLiteral(R"((:\d+){1,2}$)")));
            const QUrl url(destination);
            if (!url.isValid() || (!url.scheme().isEmpty() && url.scheme() != "file") ||
                !url.host().isEmpty())
                continue;
            auto linkedPath = url.path(QUrl::FullyDecoded);
            linkedPath.remove(QRegularExpression(QStringLiteral(R"((:\d+){1,2}$)")));
            if (QDir::isAbsolutePath(linkedPath)) {
                reported.append(linkedPath);
            } else if (!linkedPath.isEmpty() && !QDir::cleanPath(linkedPath).startsWith("../") &&
                       QDir::cleanPath(linkedPath) != "..") {
                for (const auto& value :
                     journal_->sessionContexts(session->stored.id, session->stored.generation)) {
                    const auto context = value.toObject();
                    if (context.value("context_revision").toInteger() == revision)
                        reported.append(
                            QDir(context.value("worktree_path").toString()).filePath(linkedPath));
                }
            }
        }
    }
    const auto tool = item.value("tool").toString(item.value("name").toString()).toLower();
    const bool fileTool = QStringList{"read", "read_file", "write",     "write_file",
                                      "edit", "edit_file", "multiedit", "notebookedit"}
                              .contains(tool);
    if (fileTool || type == "fileChange") {
        QList<QJsonObject> inputs{item};
        for (const auto& key : {"arguments", "input"}) {
            const auto value = item.value(key);
            inputs.append(value.isObject()
                              ? value.toObject()
                              : QJsonDocument::fromJson(value.toString().toUtf8()).object());
        }
        for (const auto& input : inputs)
            for (const auto& key : {"path", "file_path", "notebook_path"})
                reported.append(input.value(key).toString());
    }
    for (const auto& candidate : reported) {
        if (revision < 1 || !QDir::isAbsolutePath(candidate))
            continue;
        const auto canonical = QFileInfo(candidate).canonicalFilePath();
        if (canonical.isEmpty())
            continue;
        QDir folder(QFileInfo(canonical).absolutePath());
        do {
            if (!QFileInfo::exists(folder.filePath(".git")))
                continue;
            const auto root = folder.canonicalPath();
            const auto relative = QDir(root).relativeFilePath(canonical);
            if ((path == candidate || path == canonical || path == relative) &&
                !relative.split('/').contains(".git") && !commonDirectory(root).isEmpty())
                return {{"ok", true},
                        {"event_cursor", cursor},
                        {"path", relative},
                        {"resource_context_revision", revision},
                        {"worktree_path", root}};
            break;
        } while (folder.cdUp());
    }
    return failure("resource_context_unknown", "The file has no verified source.");
}

// A conversation outlives its checkout. Use the recorded repository only to list
// replacements; never silently change where a user's next turn executes.
QString SessionHost::worktreeCatalogRoot(const Session& session) const {
    if (QFileInfo(session.stored.worktreePath).isDir()) {
        const auto common = commonDirectory(session.stored.worktreePath);
        if (QFileInfo(common).fileName() == ".git") {
            journal_->rememberWorktree(session.stored.worktreePath);
            journal_->rememberWorktreeRepository(session.stored.worktreePath,
                                                 QFileInfo(common).absolutePath());
        }
        return session.stored.worktreePath;
    }
    const auto root = journal_->worktreeRepositories().value(session.stored.worktreePath);
    return !root.isEmpty() && !commonDirectory(root).isEmpty() ? root : QString{};
}

QJsonObject SessionHost::sessionWorktrees(const QJsonObject& request) {
    Session* session = nullptr;
    const auto rejected = checkedSession(request, &session);
    if (!rejected.isEmpty())
        return rejected;
    const auto payload = request.value("payload").toObject();
    // Trusted local IPC may list another repository's worktrees before choosing one
    // (ADR 0105). Cloud commands strip repository_path, so phones keep their catalog.
    const auto requested = payload.value("repository_path").toString();
    const auto catalogRoot =
        requested.isEmpty() ? worktreeCatalogRoot(*session) : normalizedWorktree(requested);
    if (!requested.isEmpty() && (catalogRoot.isEmpty() || commonDirectory(catalogRoot).isEmpty()))
        return failure("invalid_repository", "Select a Git repository.");
    auto entries = catalogRoot.isEmpty() ? QJsonArray{} : worktrees(catalogRoot);
    if (entries.isEmpty())
        return failure("worktree_unavailable", "The repository's worktrees could not be read.");
    const auto touched =
        journal_->touchedWorktreePaths(session->stored.id, session->stored.generation);
    QList<QJsonObject> ranked;
    QString currentBranch;
    const auto search = payload.value("query").toString().trimmed();
    const auto common = commonDirectory(catalogRoot);
    const auto repositoryRoot =
        QFileInfo(common).fileName() == ".git" ? QFileInfo(common).absolutePath() : QString{};
    // Browsing does not enrol a repository: it joins the catalog, which phones see, only
    // once the conversation switches to it.
    // A conversation whose checkout is gone has no repository to compare with.
    const auto sessionRoot = requested.isEmpty() ? QString{} : worktreeCatalogRoot(*session);
    const bool ownRepository =
        requested.isEmpty() || (!sessionRoot.isEmpty() && common == commonDirectory(sessionRoot));
    for (const auto& value : entries) {
        auto entry = value.toObject();
        const auto path = entry.value("worktree_path").toString();
        if (ownRepository) {
            journal_->rememberWorktree(path);
            if (!repositoryRoot.isEmpty())
                journal_->rememberWorktreeRepository(path, repositoryRoot);
        }
        auto branch = entry.value("branch").toString();
        branch.remove(0, branch.startsWith("refs/heads/") ? 11 : 0);
        entry.insert("branch", branch.isEmpty() ? QFileInfo(path).fileName() : branch);
        entry.insert("model_touched", touched.contains(path));
        // The repository's main worktree, marked "Main" in the picker.
        entry.insert("main", !repositoryRoot.isEmpty() &&
                                 path == QFileInfo(repositoryRoot).canonicalFilePath());
        if (path == QFileInfo(session->stored.worktreePath).canonicalFilePath())
            currentBranch = entry.value("branch").toString();
        if (search.isEmpty() || (branch + " " + path).contains(search, Qt::CaseInsensitive))
            ranked.append(entry);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.value("model_touched").toBool() > b.value("model_touched").toBool();
    });
    const int offset = qMax(0, payload.value("offset").toInt());
    const int limit = qBound(1, payload.value("limit").toInt(20), 100);
    entries = {};
    for (int i = offset; i < qMin(offset + limit, int(ranked.size())); ++i)
        entries.append(ranked[i]);
    // Trusted local clients may browse every repository already enrolled with this daemon.
    // The cloud projection removes these host paths; phones use their granted catalog.
    QStringList repositories;
    for (const auto& root : journal_->worktreeRepositories()) {
        if (!root.isEmpty() && QFileInfo(root).isDir() && !repositories.contains(root))
            repositories.append(root);
    }
    std::sort(repositories.begin(), repositories.end());
    return {{"ok", true},
            {"repositories", QJsonArray::fromStringList(repositories)},
            {"repository_path", repositoryRoot},
            {"total", ranked.size()},
            {"offset", offset},
            {"current_branch", currentBranch},
            {"current_model_touched",
             touched.contains(QFileInfo(session->stored.worktreePath).canonicalFilePath())},
            {"switch_available",
             (session->stored.adapter == "codex" || session->stored.adapter == "claude" ||
              !QFileInfo(session->stored.worktreePath).isDir()) &&
                 !(session->stored.adapter == "claude" && hasLiveBackgroundTasks(*session)) &&
                 !session->startingTurn && !session->interruptPending &&
                 (session->stored.state == "ready" || session->stored.state == "idle" ||
                  (session->stored.adapter == "codex" && session->stored.state == "running") ||
                  session->stored.state == "failed")},
            {"context_revision", session->stored.contextRevision},
            {"worktree_path", session->stored.worktreePath},
            {"worktrees", entries},
            {"contexts", journal_->sessionContexts(session->stored.id, session->stored.generation)},
            {"switch_supported", session->stored.adapter == "codex" ||
                                     session->stored.adapter == "claude" ||
                                     !QFileInfo(session->stored.worktreePath).isDir()}};
}

void SessionHost::recordWorktreeActivity(Session& session, const QString& type,
                                         const QJsonObject& payload, qint64 cursor) {
    const auto revision = payload.value("context_revision").toInteger();
    if (revision <= 0)
        return; // Never infer an origin for old/unknown provider turns.
    QString root;
    for (const auto& v : journal_->sessionContexts(session.stored.id, session.stored.generation)) {
        const auto c = v.toObject();
        if (c.value("context_revision").toInteger() == revision)
            root = c.value("worktree_path").toString();
    }
    if (root.isEmpty())
        return;
    if (type == "turn.started") {
        journal_->recordWorktreeActivity(session.stored, root, cursor);
        return;
    }
    if (type != "item.started" && type != "item.completed")
        return;
    const auto item = payload.value("item").toObject();
    QStringList paths;
    if (item.value("type") == "commandExecution")
        paths.append(item.value("cwd").toString());
    if (item.value("type") == "fileChange")
        for (const auto& v : item.value("changes").toArray())
            paths.append(v.toObject().value("path").toString());
    if (paths.isEmpty())
        return;
    const auto entries = worktrees(root);
    for (const auto& path : paths) {
        if (!QFileInfo(path).isAbsolute())
            continue;
        const auto canonical = QFileInfo(path).canonicalFilePath();
        if (canonical.isEmpty())
            continue;
        QString matched;
        for (const auto& v : entries) {
            const auto candidate = v.toObject().value("worktree_path").toString();
            if ((canonical == candidate || canonical.startsWith(candidate + '/')) &&
                candidate.size() > matched.size())
                matched = candidate;
        }
        if (!matched.isEmpty()) {
            journal_->recordWorktreeActivity(session.stored, matched, cursor);
            const bool succeeded =
                item.value("status") == "completed" &&
                (item.value("type") != "commandExecution" || item.value("exitCode").toInt(-1) == 0);
            // Never follow prose, a failed command, an old turn, or an unrelated repository.
            // Keep a manual choice made during this turn, and do not bounce back when a later
            // bookkeeping command uses the submitted cwd again.
            if (type == "item.completed" && succeeded && session.stored.adapter == "codex" &&
                !session.stored.turnId.isEmpty() &&
                payload.value("turn_id") == session.stored.turnId &&
                revision == session.worktreeFollowRevision &&
                session.stored.contextRevision == session.worktreeFollowRevision && matched != root)
                session.workingWorktree = matched;
        }
    }
}

QJsonObject SessionHost::switchWorktree(const QJsonObject& request) {
    Session* session = nullptr;
    const auto rejected = checkedSession(request, &session);
    if (!rejected.isEmpty())
        return rejected;
    const auto payload = request.value("payload").toObject();
    if (payload.value("expected_context_revision").toInteger(-1) != session->stored.contextRevision)
        return failure("stale_context", "Refresh the conversation's execution context.");
    if (session->startingTurn || session->interruptPending || session->stored.state == "starting" ||
        session->stored.state == "waiting_for_input" || session->stored.state == "stopping" ||
        !approvals_->pendingPage(session->stored.id, session->stored.generation)
             .value("pending_approvals")
             .toArray()
             .isEmpty())
        return failure("session_busy", "Wait for startup or shutdown to finish, or answer the "
                                       "pending request before switching.");
    // Claude's query is cwd-bound. Resume it only between turns and after its tasks end.
    if (session->stored.adapter == "claude" &&
        (session->stored.state == "running" || hasLiveBackgroundTasks(*session)))
        return failure("session_busy", "Wait for the Claude turn and background tasks to finish "
                                       "before switching worktrees.");
    // Other providers need a tested resume/handoff implementation before advertising this.
    if (session->stored.adapter != "codex" && session->stored.adapter != "claude" &&
        (QFileInfo(session->stored.worktreePath).isDir() || session->stored.state == "running"))
        return failure("context_switch_unsupported",
                       "This provider does not support worktree switching yet.");
    const auto target = normalizedWorktree(payload.value("worktree_path").toString());
    if (target.isEmpty())
        return failure("invalid_worktree", "The target worktree does not exist.");
    // Local IPC may choose another repository. Cloud commands reach this point only
    // after resolving a granted opaque catalog target; never trust a remote path.
    if (commonDirectory(target).isEmpty())
        return failure("invalid_worktree", "The target must be a Git worktree.");
    bool found = false;
    for (const auto& entry : worktrees(target))
        found |= entry.toObject().value("worktree_path").toString() == target;
    if (!found)
        return failure("invalid_worktree", "Select a registered Git worktree root.");
    return moveSessionWorktree(*session, target, request.value("message_id").toString());
}

QJsonObject SessionHost::moveSessionWorktree(Session& session, const QString& target,
                                             const QString& messageId) {
    const bool changed = target != session.stored.worktreePath;
    const auto revision = session.stored.contextRevision + (changed ? 1 : 0);
    QString branch;
    for (const auto& entry : worktrees(target)) {
        if (entry.toObject().value("worktree_path").toString() == target)
            branch = entry.toObject().value("branch").toString();
    }
    if (branch.startsWith("refs/heads/"))
        branch.remove(0, 11);
    const QJsonObject result{{"ok", true},
                             {"current_branch", branch},
                             {"session_id", session.stored.id},
                             {"session_generation", session.stored.generation},
                             {"worktree_path", target},
                             {"context_revision", revision}};
    if (!changed)
        return result;
    QString error;
    const auto cursor =
        journal_->switchSessionWorktree(session.stored, target, messageId, result, &error);
    if (cursor < 0)
        return failure("journal_error", error);
    session.stored.worktreePath = target;
    session.stored.contextRevision = revision;
    session.stored.lastCursor = cursor;
    // The current turn retains its submitted context. Resume the same thread with
    // the chosen cwd on the next turn; do not restart or interrupt the active turn.
    session.attachedToProvider = false;
    const auto events =
        journal_->eventsAfter(session.stored.id, session.stored.generation, cursor - 1, 1);
    if (!events.isEmpty())
        emit eventReady(events.first().toObject());
    return result;
}

QString SessionHost::executionDirectory(const Session& session) const {
    if (QFileInfo(session.stored.worktreePath).isDir())
        return session.stored.worktreePath;
    const auto root = worktreeCatalogRoot(session);
    if (!root.isEmpty())
        return root;
    QString path = session.stored.worktreePath;
    while (!path.isEmpty() && !QFileInfo(path).isDir()) {
        const auto parent = QFileInfo(path).absolutePath();
        if (parent == path)
            break;
        path = parent;
    }
    return QFileInfo(path).isDir() ? path : QDir::homePath();
}

void SessionHost::noteWorktreesBeforeTurn(Session& session) {
    session.worktreesBeforeTurn.clear();
    session.workingWorktree.clear();
    session.worktreeFollowRevision = session.stored.contextRevision;
    if (QFileInfo(session.stored.worktreePath).isDir())
        return;
    const auto root = worktreeCatalogRoot(session);
    for (const auto& entry : root.isEmpty() ? QJsonArray{} : worktrees(root))
        session.worktreesBeforeTurn.append(entry.toObject().value("worktree_path").toString());
}

void SessionHost::followRecreatedWorktree(Session& session) {
    const auto before = std::exchange(session.worktreesBeforeTurn, {});
    const auto used = std::exchange(session.workingWorktree, {});
    if (session.worktreeFollowRevision != session.stored.contextRevision)
        return; // An explicit user selection takes precedence over tool activity.
    if (!used.isEmpty() && QFileInfo(used).isDir() &&
        commonDirectory(used) == commonDirectory(worktreeCatalogRoot(session))) {
        const auto moved = moveSessionWorktree(session, used, {});
        if (!moved.value("ok").toBool())
            qWarning("Unable to follow the working checkout: %s",
                     qUtf8Printable(moved.value("error_message").toString()));
        return;
    }
    if (QFileInfo(session.stored.worktreePath).isDir())
        return; // Recreated in place: the conversation already points at it.
    const auto root = worktreeCatalogRoot(session);
    if (root.isEmpty())
        return;
    // Prefer a checkout with the deleted one's folder name; otherwise the single worktree the
    // turn added. Several new ones are ambiguous and left to the user's worktree picker.
    const auto name = QFileInfo(session.stored.worktreePath).fileName();
    QString sameName;
    QStringList added;
    for (const auto& value : worktrees(root)) {
        const auto path = value.toObject().value("worktree_path").toString();
        if (path == root)
            continue;
        if (QFileInfo(path).fileName() == name)
            sameName = path;
        if (!before.isEmpty() && !before.contains(path))
            added.append(path);
    }
    const auto target = !sameName.isEmpty() ? sameName
                        : added.size() == 1 ? added.first()
                                            : QString{};
    if (target.isEmpty())
        return;
    const auto moved = moveSessionWorktree(session, target, {});
    if (!moved.value("ok").toBool())
        qWarning("Unable to follow the recreated worktree of %s: %s",
                 qUtf8Printable(session.stored.id),
                 qUtf8Printable(moved.value("error_message").toString()));
}

} // namespace ketplus::daemon
