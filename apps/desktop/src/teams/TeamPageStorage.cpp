// ADR 0114 §10 (T-37): the team page's disk warning and its worktrees of done tasks. The daemon
// measures; the page only shows it and offers each removal, which the user confirms.
#include "KetBanner.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPage.h"
#include "teams/TeamPageParts.h"
#include "teams/TeamWorktreeDialog.h"

#include "KetDialog.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QPointer>
#include <QVBoxLayout>

namespace ketplus {

using team_page::px;
using team_page::Text;

QString TeamPage::doneTasks(const QJsonObject& team) {
    QStringList done;
    for (const auto& value : team.value(QStringLiteral("tasks")).toArray()) {
        const QJsonObject task = value.toObject();
        if (task.value(QStringLiteral("state")).toString() == QStringLiteral("done"))
            done.append(task.value(QStringLiteral("id")).toString() + QLatin1Char('=') +
                        task.value(QStringLiteral("worktree")).toString());
    }
    return done.join(QLatin1Char('\n'));
}

void TeamPage::readStorage() {
    if (storagePending_ || !teams_->team(teamId_))
        return;
    storagePending_ = true;
    const QPointer<TeamPage> alive(this);
    teams_->request(QStringLiteral("team.storage.get"), {{QStringLiteral("team_id"), teamId_}},
                    [alive](const QJsonObject& result, const QString& error) {
                        if (!alive)
                            return;
                        alive->storagePending_ = false;
                        // A daemon without teams.storage/1 refuses it: the page shows neither.
                        const QJsonObject storage =
                            error.isEmpty() ? result.value(QStringLiteral("storage")).toObject()
                                            : QJsonObject{};
                        if (storage == alive->storage_)
                            return;
                        alive->storage_ = storage;
                        alive->forgetStaleRemoval();
                        alive->rebuild();
                    });
}

void TeamPage::buildDiskBanner() {
    const team_storage::Warning warning = team_storage::warning(storage_);
    if (warning.title.isEmpty())
        return;
    auto* banner =
        new ket::Banner(warning.stop ? ket::BannerTone::Danger : ket::BannerTone::Warning,
                        warning.title, warning.message, column_);
    banner->setObjectName(QStringLiteral("teamDiskBanner"));
    layout_->addWidget(banner);
}

// T-62: a summary of "Remove all…" that names a kept worktree no longer listed (removed
// since, one by one) is not true any more, so it goes.
void TeamPage::forgetStaleRemoval() {
    if (!removalOutcome_ || removingAll_)
        return;
    QStringList listed;
    for (const auto& value : storage_.value(QStringLiteral("worktrees")).toArray())
        listed.append(value.toObject().value(QStringLiteral("task_id")).toString());
    for (auto it = removalOutcome_->kept.cbegin(); it != removalOutcome_->kept.cend(); ++it)
        if (!listed.contains(it.key())) {
            removalOutcome_.reset();
            return;
        }
}

void TeamPage::buildRemovalSummary() {
    if (!removalOutcome_)
        return;
    auto* summary = new ket::Banner(removalOutcome_->kept.isEmpty() ? ket::BannerTone::Info
                                                                    : ket::BannerTone::Warning,
                                    team_storage::summary(*removalOutcome_), {}, column_);
    summary->setObjectName(QStringLiteral("teamDoneWorktreesSummary"));
    layout_->addWidget(summary);
}

void TeamPage::buildDoneWorktrees(const QJsonObject& team) {
    const QJsonArray worktrees = storage_.value(QStringLiteral("worktrees")).toArray();
    if (worktrees.isEmpty()) {
        // Everything was removed: the section stays for the summary of the run.
        if (removalOutcome_) {
            addSection(tr("Worktrees of done tasks"), tr("None left on disk"),
                       "teamDoneWorktreesHeading");
            buildRemovalSummary();
        }
        return;
    }
    // T-62: every listed worktree through the same removal, after one confirmation.
    auto* removeAll = new ket::Button(tr("Remove all…"), ket::ButtonVariant::Ghost);
    removeAll->setObjectName(QStringLiteral("teamDoneWorktreesRemoveAll"));
    removeAll->setIcon(ket::Icons::icon(u"trash"));
    removeAll->setEnabled(!removingAll_);
    connect(removeAll, &QAbstractButton::clicked, this, [this, worktrees] {
        if (removingAll_)
            return;
        const QPointer<TeamPage> alive(this);
        teamWorktreeRemovalAll(
            this, teams_, teamId_, worktrees,
            [alive] {
                if (!alive)
                    return;
                alive->removingAll_ = true;
                alive->removalOutcome_.reset();
                alive->rebuild();
            },
            [alive](const team_storage::Outcome& outcome) {
                if (!alive)
                    return;
                alive->removingAll_ = false;
                alive->removalOutcome_ = outcome;
                // The kept worktrees are listed again from the daemon's measure.
                alive->storageTasks_ = QStringLiteral("-");
                alive->readStorage();
                alive->rebuild();
            })
            ->open();
    });
    addSection(tr("Worktrees of done tasks"),
               worktrees.size() == 1 ? tr("1 worktree on disk")
                                     : tr("%1 worktrees on disk").arg(worktrees.size()),
               "teamDoneWorktreesHeading", removeAll);
    buildRemovalSummary();
    auto* panel = new team_page::Panel(column_);
    panel->setObjectName(QStringLiteral("teamDoneWorktrees"));
    auto* rows = new QVBoxLayout(panel);
    rows->setContentsMargins(1, 1, 1, 1);
    rows->setSpacing(0);
    bool first = true;
    for (const auto& value : worktrees) {
        const QJsonObject entry = value.toObject();
        const QString taskId = entry.value(QStringLiteral("task_id")).toString();
        auto* row = new team_page::Row(panel, !first);
        first = false;
        row->setObjectName(QStringLiteral("teamDoneWorktree_") + taskId);
        row->setMinimumHeight(px(56));
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(px(18), px(8), px(12), px(8));
        line->setSpacing(px(14));
        auto* id = new Text(taskId, row, Text::Fit::Elide);
        id->setType(12, QFont::Medium, true);
        id->setFixedWidth(px(36));
        line->addWidget(id);
        auto* what = new QVBoxLayout;
        what->setSpacing(px(3));
        QString title = entry.value(QStringLiteral("title")).toString();
        for (const auto& task : team.value(QStringLiteral("tasks")).toArray())
            if (task.toObject().value(QStringLiteral("id")).toString() == taskId)
                title = task.toObject().value(QStringLiteral("title")).toString(title);
        auto* name = new Text(title, row, Text::Fit::Elide);
        name->setObjectName(QStringLiteral("teamDoneWorktreeTitle"));
        what->addWidget(name);
        const QString where = entry.value(QStringLiteral("path")).toString();
        auto* path = new Text(where, row, Text::Fit::ElideMiddle);
        path->setObjectName(QStringLiteral("teamDoneWorktreePath"));
        path->setType(12, QFont::Normal, true);
        path->setTone(QStringLiteral("text-muted"));
        what->addWidget(path);
        // T-62: why the last "Remove all…" kept it, in the daemon's or git's words.
        if (removalOutcome_ && removalOutcome_->kept.contains(taskId)) {
            auto* reason = new Text(removalOutcome_->kept.value(taskId), row);
            reason->setObjectName(QStringLiteral("teamDoneWorktreeReason"));
            reason->setType(12);
            reason->setTone(QStringLiteral("danger"));
            what->addWidget(reason);
        }
        line->addLayout(what, 1);
        const QJsonValue bytes = entry.value(QStringLiteral("size_bytes"));
        auto* size = new Text(bytes.isDouble() ? team_storage::size(bytes.toInteger()) : tr("–"),
                              row, Text::Fit::Elide);
        size->setObjectName(QStringLiteral("teamDoneWorktreeSize"));
        size->setTone(QStringLiteral("text-secondary"));
        line->addWidget(size);
        auto* remove = new ket::Button(tr("Remove…"), ket::ButtonVariant::Ghost, row);
        remove->setObjectName(QStringLiteral("teamDoneWorktreeRemove"));
        remove->setIcon(ket::Icons::icon(u"trash"));
        remove->setAccessibleName(tr("Remove the worktree of %1…").arg(taskId));
        remove->setEnabled(!removingAll_);
        connect(remove, &QAbstractButton::clicked, this,
                [this, entry] { teamWorktreeRemoval(this, teams_, teamId_, entry)->open(); });
        line->addWidget(remove);
        rows->addWidget(row);
    }
    layout_->addWidget(panel);
}

} // namespace ketplus
