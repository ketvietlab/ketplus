#include "agents/ReviewIntegrity.h"

#include "agents/GitProcess.h"

#include <QCryptographicHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

#include <memory>

namespace ketplus::daemon::review_integrity {
namespace {

QString firstLine(const std::optional<QString>& output) {
    return output ? output->section(QLatin1Char('\n'), 0, 0).trimmed() : QString{};
}

void captureRemote(QObject* owner, const Scope& scope, std::shared_ptr<Snapshot> snapshot,
                   std::function<void(Snapshot)> done) {
    snapshot->taskHead = scope.taskHead;
    if (scope.branch.isEmpty() || scope.remoteDirectory.isEmpty()) {
        done(*snapshot);
        return;
    }
    runGit(owner, scope.remoteDirectory,
           {QStringLiteral("ls-remote"), QStringLiteral("origin"),
            QStringLiteral("refs/heads/") + scope.branch},
           [snapshot, done](const std::optional<QString>& output) {
               snapshot->remote = output.has_value();
               // No line: the branch is not on the remote.
               snapshot->remoteHead = firstLine(output).section(QLatin1Char('\t'), 0, 0);
               done(*snapshot);
           });
}

void add(QJsonArray& changes, const QString& field, const QString& before, const QString& after) {
    if (before == after)
        return;
    changes.append(QJsonObject{{QStringLiteral("field"), field},
                               {QStringLiteral("before"), before},
                               {QStringLiteral("after"), after}});
}

} // namespace

void capture(QObject* owner, const Scope& scope, std::function<void(Snapshot)> done) {
    auto snapshot = std::make_shared<Snapshot>();
    if (scope.worktree.isEmpty()) {
        captureRemote(owner, scope, snapshot, std::move(done));
        return;
    }
    const QPointer<QObject> guard(owner);
    const QString worktree = scope.worktree;
    runGit(owner, worktree, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")},
           [guard, scope, snapshot, done, worktree](const std::optional<QString>& head) {
               if (!guard)
                   return;
               snapshot->head = firstLine(head);
               runGit(guard, worktree,
                      {QStringLiteral("rev-parse"), QStringLiteral("--symbolic-full-name"),
                       QStringLiteral("HEAD")},
                      [guard, scope, snapshot, done, worktree](const std::optional<QString>& ref) {
                          if (!guard)
                              return;
                          snapshot->symbolicRef = firstLine(ref);
                          runGit(
                              guard, worktree,
                              {QStringLiteral("status"), QStringLiteral("--porcelain=v2"),
                               QStringLiteral("--untracked-files=all")},
                              [guard, scope, snapshot, done](const std::optional<QString>& status) {
                                  if (!guard)
                                      return;
                                  snapshot->local = status.has_value();
                                  if (status)
                                      snapshot->statusHash = QString::fromLatin1(
                                          QCryptographicHash::hash(status->toUtf8(),
                                                                   QCryptographicHash::Sha256)
                                              .toHex());
                                  captureRemote(guard, scope, snapshot, done);
                              });
                      });
           });
}

QJsonArray changes(const Snapshot& before, const Snapshot& after) {
    QJsonArray result;
    // A worktree that could not be read before the turn cannot be vouched for: the review
    // does not count.
    if (!before.local)
        result.append(
            QJsonObject{{QStringLiteral("field"), QStringLiteral("worktree")},
                        {QStringLiteral("before"), QStringLiteral("unreadable")},
                        {QStringLiteral("after"),
                         after.local ? QStringLiteral("readable") : QStringLiteral("unreadable")}});
    if (before.local) {
        add(result, QStringLiteral("head"), before.head, after.head);
        add(result, QStringLiteral("symbolic_ref"), before.symbolicRef, after.symbolicRef);
        add(result, QStringLiteral("status"), before.statusHash,
            after.local ? after.statusHash : QStringLiteral("unreadable"));
    }
    add(result, QStringLiteral("task_head"), before.taskHead, after.taskHead);
    if (before.remote && after.remote)
        add(result, QStringLiteral("remote_head"), before.remoteHead, after.remoteHead);
    return result;
}

bool remoteCompared(const Scope& scope, const Snapshot& before, const Snapshot& after) {
    return scope.branch.isEmpty() || (before.remote && after.remote);
}

QString describe(const QJsonArray& changes) {
    QStringList lines;
    for (const QJsonValue& value : changes) {
        const QJsonObject change = value.toObject();
        const QString field = change.value(QStringLiteral("field")).toString();
        const auto shown = [&field](const QString& text) {
            if (text.isEmpty())
                return QStringLiteral("(none)");
            // A commit or a status hash reads well enough by its first twelve characters.
            return field == QStringLiteral("symbolic_ref") ? text : text.left(12);
        };
        const QString name = field == QStringLiteral("head")           ? QStringLiteral("HEAD")
                             : field == QStringLiteral("symbolic_ref") ? QStringLiteral("branch")
                             : field == QStringLiteral("status") ? QStringLiteral("worktree status")
                             : field == QStringLiteral("worktree") ? QStringLiteral("worktree")
                             : field == QStringLiteral("task_head")
                                 ? QStringLiteral("task head_commit")
                                 : QStringLiteral("remote branch head");
        lines.append(QStringLiteral("%1: %2 → %3")
                         .arg(name, shown(change.value(QStringLiteral("before")).toString()),
                              shown(change.value(QStringLiteral("after")).toString())));
    }
    return lines.join(QStringLiteral("; "));
}

QString codexReviewInstructions() {
    return QStringLiteral(
        "You review with Full access. Stay a reviewer: build and test only in a scratch copy "
        "outside the reviewed worktree, and do not edit, commit, push, merge, approve, comment "
        "on pull requests or switch branches there. KetPlus records the reviewed branch before "
        "this turn and compares it after; any change voids this review.");
}

} // namespace ketplus::daemon::review_integrity
