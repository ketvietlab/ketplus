// ADR 0114 §10 (T-62): "Remove all…" for the worktrees of done tasks. One confirmation names
// how many and their size; then each worktree goes through team.worktree.remove, the same
// daemon path and guards as a single Remove, one after another. A refusal keeps its worktree
// and its reason; the others are still removed.
#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPageParts.h"
#include "teams/TeamWorktreeDialog.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QPointer>
#include <QVBoxLayout>

#include <functional>
#include <memory>

namespace ketplus {
namespace {

using team_page::Text;

QString tr(const char* text) { return QCoreApplication::translate("TeamWorktreeDialog", text); }

// The removals left, and what they did, while the dialog sends them one at a time.
struct Run final {
    QString teamId;
    QJsonArray entries;
    qsizetype next{0};
    team_storage::Outcome outcome;
};

} // namespace

QString team_storage::summary(const Outcome& outcome) {
    const qsizetype removed = outcome.removed.size();
    const qsizetype kept = outcome.kept.size();
    const QString done =
        removed == 1 ? tr("Removed 1 worktree.") : tr("Removed %1 worktrees.").arg(removed);
    if (kept == 0)
        return done;
    return done + QLatin1Char(' ') +
           (kept == 1 ? tr("Kept 1 with its reason.")
                      : tr("Kept %1 with their reasons.").arg(kept));
}

ket::Dialog* teamWorktreeRemovalAll(QWidget* anchor, TeamDirectory* teams, const QString& teamId,
                                    const QJsonArray& entries, std::function<void()> started,
                                    std::function<void(const team_storage::Outcome&)> finished) {
    const qsizetype count = entries.size();
    auto* dialog = new ket::Dialog(
        count == 1 ? tr("Remove 1 worktree?") : tr("Remove %1 worktrees?").arg(count), anchor);
    dialog->setObjectName(QStringLiteral("teamWorktreeAllDialog"));
    QObject::connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(team_page::px(10));
    qint64 total = 0;
    bool unknown = false;
    QStringList tasks;
    for (const auto& value : entries) {
        const QJsonObject entry = value.toObject();
        tasks.append(entry.value(QStringLiteral("task_id")).toString());
        const QJsonValue bytes = entry.value(QStringLiteral("size_bytes"));
        if (bytes.isDouble())
            total += bytes.toInteger();
        else
            unknown = true;
    }
    const QString freed =
        unknown ? tr("at least %1").arg(team_storage::size(total)) : team_storage::size(total);
    auto* consequence =
        new Text(tr("The folders of %1 are deleted from disk, freeing %2 in total. The tasks' "
                    "branches and their commits stay.")
                     .arg(tasks.join(QStringLiteral(", ")), freed),
                 body);
    consequence->setObjectName(QStringLiteral("teamWorktreeAllConsequence"));
    consequence->setTone(QStringLiteral("text-secondary"));
    layout->addWidget(consequence);
    auto* how = new Text(tr("Each one goes through the same checks as Remove…, one after another: "
                            "a worktree with uncommitted or untracked changes, with commits on no "
                            "remote branch, or the folder of a conversation that is running or "
                            "waits for you is kept with the reason, and the others are still "
                            "removed."),
                         body);
    how->setTone(QStringLiteral("text-muted"));
    how->setType(12);
    layout->addWidget(how);
    auto* result = new ket::Banner(ket::BannerTone::Info, {}, {}, body);
    result->setObjectName(QStringLiteral("teamWorktreeAllResult"));
    result->hide();
    layout->addWidget(result);
    dialog->setBody(body);
    dialog->setCardWidth(500);

    auto* cancel = dialog->addButton(tr("Cancel"), ket::ButtonVariant::Ghost);
    cancel->setObjectName(QStringLiteral("teamWorktreeAllCancel"));
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* remove =
        dialog->addButton(count == 1 ? tr("Remove worktree") : tr("Remove %1 worktrees").arg(count),
                          ket::ButtonVariant::Danger);
    remove->setObjectName(QStringLiteral("teamWorktreeAllRemove"));

    auto run = std::make_shared<Run>(Run{.teamId = teamId, .entries = entries});
    const QPointer<ket::Dialog> alive(dialog);
    // Shows the outcome in the dialog and hands it to the page, which keeps the reasons.
    const auto done = [=] {
        if (finished)
            finished(run->outcome);
        if (!alive)
            return;
        QStringList lines;
        for (auto it = run->outcome.kept.cbegin(); it != run->outcome.kept.cend(); ++it)
            lines.append(QStringLiteral("%1: %2").arg(it.key(), it.value()));
        for (auto it = run->outcome.notes.cbegin(); it != run->outcome.notes.cend(); ++it)
            lines.append(QStringLiteral("%1: %2").arg(it.key(), it.value()));
        result->setTone(run->outcome.kept.isEmpty() ? ket::BannerTone::Info
                                                    : ket::BannerTone::Warning);
        result->setTitle(team_storage::summary(run->outcome));
        result->setMessage(lines.join(QLatin1Char('\n')));
        result->show();
        remove->hide();
        cancel->setText(tr("Close"));
        cancel->setEnabled(true);
    };
    // Sends the next removal once the one before answered, so they run in order.
    // The pending reply holds the chain; the function holds itself only weakly.
    auto step = std::make_shared<std::function<void()>>();
    const std::weak_ptr<std::function<void()>> weak = step;
    *step = [=] {
        const auto self = weak.lock();
        if (!self)
            return;
        if (run->next >= run->entries.size()) {
            done();
            return;
        }
        const QString taskId =
            run->entries.at(run->next++).toObject().value(QStringLiteral("task_id")).toString();
        teams->request(
            QStringLiteral("team.worktree.remove"),
            {{QStringLiteral("team_id"), run->teamId}, {QStringLiteral("task_id"), taskId}},
            [run, self, taskId](const QJsonObject& answer, const QString& error) {
                if (!error.isEmpty()) {
                    run->outcome.kept.insert(taskId, error);
                } else {
                    run->outcome.removed.append(taskId);
                    const QString note = answer.value(QStringLiteral("note")).toString();
                    if (!note.isEmpty())
                        run->outcome.notes.insert(taskId, note);
                }
                (*self)();
            });
    };
    QObject::connect(remove, &QAbstractButton::clicked, dialog, [=] {
        // Sent once; the dialog closes only when every removal answered.
        remove->setEnabled(false);
        cancel->setEnabled(false);
        if (started)
            started();
        (*step)();
    });
    return dialog;
}

} // namespace ketplus
