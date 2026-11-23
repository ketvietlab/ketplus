// ADR 0114 §10 (T-23): the daemon removes the worktrees of done tasks in the background. The
// page shows what the daemon says in team.storage.get's `cleanup`: the worktrees it holds are
// locked, a line says it is running, and a summary stays when it ends, until the user closes it
// or the next run. Nothing is kept here: a page opened again reads the same from the daemon.
#include "KetBanner.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"
#include "teams/TeamPageParts.h"
#include "teams/TeamWorktreeDialog.h"

#include <QJsonArray>

namespace ketplus {

namespace {

QJsonObject lastCleanup(const QJsonObject& storage) {
    return storage.value(QStringLiteral("cleanup"))
        .toObject()
        .value(QStringLiteral("last"))
        .toObject();
}

} // namespace

bool TeamPage::cleanupRuns() const {
    return !storage_.value(QStringLiteral("cleanup"))
                .toObject()
                .value(QStringLiteral("active"))
                .toArray()
                .isEmpty();
}

bool TeamPage::hasCleanupSummary() const {
    const QJsonObject last = lastCleanup(storage_);
    return !last.isEmpty() &&
           last.value(QStringLiteral("run")).toInteger() > teams_->dismissedCleanup(teamId_);
}

// A worktree the daemon holds for a cleanup, "queued" or "removing": its button waits, with a
// word and a still icon, and says which in its accessible name.
void TeamPage::lockRemoval(ket::Button* remove, const QString& taskId, const QString& removal) {
    if (removal.isEmpty())
        return;
    const bool removing = removal == QStringLiteral("removing");
    remove->setText(removing ? tr("Removing…") : tr("Queued"));
    remove->setIcon(ket::Icons::icon(removing ? u"trash" : u"clock"));
    remove->setAccessibleName(removing ? tr("The worktree of %1 is being removed").arg(taskId)
                                       : tr("The worktree of %1 waits to be removed").arg(taskId));
    remove->setEnabled(false);
}

void TeamPage::buildCleanupSummary() {
    if (cleanupRuns()) {
        const qsizetype count = storage_.value(QStringLiteral("cleanup"))
                                    .toObject()
                                    .value(QStringLiteral("active"))
                                    .toArray()
                                    .size();
        auto* running = new ket::Banner(
            ket::BannerTone::Info,
            count == 1 ? tr("Removing 1 worktree in the background")
                       : tr("Removing %1 worktrees in the background").arg(count),
            tr("Each one goes through the same checks. A summary stays here when the last one "
               "ends; the rest of the page keeps working meanwhile."),
            column_);
        running->setObjectName(QStringLiteral("teamCleanupRunning"));
        layout_->addWidget(running);
        return;
    }
    if (!hasCleanupSummary())
        return;
    const QJsonObject last = lastCleanup(storage_);
    const qsizetype removed = last.value(QStringLiteral("removed")).toArray().size();
    const qsizetype kept = last.value(QStringLiteral("kept")).toArray().size();
    QStringList lines;
    for (const auto& value : last.value(QStringLiteral("kept")).toArray())
        lines.append(QStringLiteral("%1: %2").arg(
            value.toObject().value(QStringLiteral("task_id")).toString(),
            value.toObject().value(QStringLiteral("reason")).toString()));
    const QJsonArray notes = last.value(QStringLiteral("notes")).toArray();
    for (const auto& value : notes)
        lines.append(QStringLiteral("%1: %2").arg(
            value.toObject().value(QStringLiteral("task_id")).toString(),
            value.toObject().value(QStringLiteral("note")).toString()));

    QString title;
    if (removed == 0) {
        title = tr("No worktree was removed.");
    } else {
        title = removed == 1 ? tr("Removed 1 worktree") : tr("Removed %1 worktrees").arg(removed);
        const qint64 freed = last.value(QStringLiteral("freed_bytes")).toInteger();
        const bool exact = last.value(QStringLiteral("freed_exact")).toBool();
        if (freed > 0)
            title += QStringLiteral(", ") +
                     (exact ? tr("freeing %1.").arg(team_storage::size(freed))
                            : tr("freeing at least %1.").arg(team_storage::size(freed)));
        else
            title += QLatin1Char('.');
    }
    if (kept > 0)
        title += QLatin1Char(' ') + (kept == 1 ? tr("Kept 1 with its reason.")
                                               : tr("Kept %1 with their reasons.").arg(kept));
    auto* summary =
        new ket::Banner(lines.isEmpty() ? ket::BannerTone::Info : ket::BannerTone::Warning, title,
                        lines.join(QLatin1Char('\n')), column_);
    summary->setObjectName(QStringLiteral("teamDoneWorktreesSummary"));
    const qint64 run = last.value(QStringLiteral("run")).toInteger();
    auto* close = summary->setAction(tr("Dismiss"));
    close->setObjectName(QStringLiteral("teamCleanupDismiss"));
    connect(close, &QAbstractButton::clicked, this, [this, run] {
        teams_->dismissCleanup(teamId_, run);
        rebuild();
    });
    layout_->addWidget(summary);
}

} // namespace ketplus
