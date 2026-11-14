#include "KetButton.h"
#include "KetDialog.h"
#include "KetSelect.h"
#include "KetTextField.h"
#include "teams/TeamCommands.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPageParts.h"
#include "teams/TeamTaskDialog.h"

#include <QJsonArray>
#include <QPointer>
#include <QVBoxLayout>

#include <utility>

// The task dialog's actions: presenting it, reading its history and handing the task over.

namespace ketplus {

void TeamTaskDialog::readHistory(const bool earlier) {
    if (historyPending_) {
        historyAgain_ = historyAgain_ || !earlier;
        return;
    }
    if (earlier && historyStart_ <= 0)
        return;
    historyPending_ = true;
    QJsonObject payload{{QStringLiteral("team_id"), teamId_},
                        {QStringLiteral("task_id"), taskId_},
                        {QStringLiteral("history_limit"), 50}};
    if (earlier)
        payload.insert(QStringLiteral("history_before"), historyStart_);
    const QPointer<TeamTaskDialog> alive(this);
    teams_->request(
        QStringLiteral("team.task.get"), payload,
        [alive, earlier](const QJsonObject& result, const QString& error) {
            if (!alive)
                return;
            alive->historyPending_ = false;
            const QJsonObject task = result.value(QStringLiteral("task")).toObject();
            if (error.isEmpty() && task.contains(QStringLiteral("history_total"))) {
                const QJsonArray page = task.value(QStringLiteral("history")).toArray();
                const qsizetype start = task.value(QStringLiteral("history_start")).toInteger();
                const qsizetype shownEnd = alive->historyStart_ + alive->history_.size();
                QJsonArray joined;
                if (earlier && alive->historyRead_ && start + page.size() == alive->historyStart_) {
                    // The page before the first entry shown.
                    joined = page;
                    for (const auto& value : alive->history_)
                        joined.append(value);
                    alive->historyStart_ = start;
                } else if (!earlier && alive->historyRead_ && start >= alive->historyStart_ &&
                           start <= shownEnd) {
                    // A newer page: keep what was read before it.
                    for (qsizetype index = 0; index < start - alive->historyStart_; ++index)
                        joined.append(alive->history_.at(index));
                    for (const auto& value : page)
                        joined.append(value);
                } else {
                    joined = page;
                    alive->historyStart_ = start;
                }
                alive->history_ = joined;
                alive->historyTotal_ = task.value(QStringLiteral("history_total")).toInteger();
                alive->historyRead_ = true;
                alive->rebuild();
            }
            if (std::exchange(alive->historyAgain_, false))
                alive->readHistory();
        });
}

QString TeamTaskDialog::handoverKey() const {
    return TeamCommands::key(teamId_, QStringLiteral("task/%1/handover").arg(taskId_));
}

ket::Dialog* TeamTaskDialog::openHandover() {
    if (handover_ || !canHandOver_)
        return handover_;
    auto* dialog = new ket::Dialog(ownerSession_.isEmpty() ? tr("Assign %1").arg(taskId_)
                                                           : tr("Hand over %1").arg(taskId_),
                                   this);
    dialog->setObjectName(QStringLiteral("teamHandoverDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(form_);
    form_->show();
    error_->setVisible(false);
    reason_->setInvalid(false);
    dialog->setBody(body);
    auto* cancel = dialog->addButton(tr("Cancel"), ket::ButtonVariant::Ghost);
    connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* hand = dialog->addButton(ownerSession_.isEmpty() ? tr("Assign") : tr("Hand over"),
                                   ket::ButtonVariant::Primary);
    hand->setObjectName(QStringLiteral("teamHandoverButton"));
    teams_->commands()->bind(hand, handoverKey());
    connect(hand, &QAbstractButton::clicked, this, &TeamTaskDialog::handOver);
    // The fields outlive the dialog: they come back here before it deletes its body.
    connect(dialog, &ket::Dialog::finished, this, [this, dialog] {
        form_->hide();
        form_->setParent(this);
        // A finished dialog only hides: forget it, so Hand over… opens a new one.
        if (handover_ == dialog)
            handover_ = nullptr;
        dialog->deleteLater();
    });
    handover_ = dialog;
    dialog->open();
    return dialog;
}

void TeamTaskDialog::handOver() {
    const QString owner = owner_->currentData().toString();
    const QString reason = reason_->text().trimmed();
    const auto fail = [this](const QString& message) {
        error_->setText(message);
        error_->setVisible(!message.isEmpty());
    };
    if (owner.isEmpty())
        return fail(tr("Choose the new owner."));
    if (reason.isEmpty()) {
        reason_->setInvalid(true);
        return fail(tr("Give the reason for the handover."));
    }
    reason_->setInvalid(false);
    fail({});
    const QPointer<TeamTaskDialog> alive(this);
    // The answer belongs to the dialog that asked. One that was cancelled meanwhile, or
    // replaced by a new one with a new draft, is not touched by it; the task itself shows
    // the new owner either way.
    const QPointer<ket::Dialog> asked(handover_);
    teams_->commands()->send(handoverKey(), {}, QStringLiteral("team.task.handover"),
                             {{QStringLiteral("team_id"), teamId_},
                              {QStringLiteral("task_id"), taskId_},
                              {QStringLiteral("owner"), owner},
                              {QStringLiteral("reason"), reason}},
                             [alive, asked, fail](const QJsonObject&, const QString& error) {
                                 if (!alive || asked != alive->handover_)
                                     return;
                                 fail(error);
                                 if (!error.isEmpty())
                                     return;
                                 alive->reason_->clear();
                                 // Done: back to the task, which now shows its new owner.
                                 if (alive->handover_)
                                     alive->handover_->accept();
                             });
}

ket::Dialog* TeamTaskDialog::present(QWidget* anchor, TeamDirectory* teams, const QString& teamId,
                                     const QString& taskId) {
    const auto team = teams->team(teamId);
    QString title;
    if (team)
        for (const auto& value : team->value(QStringLiteral("tasks")).toArray())
            if (value.toObject().value(QStringLiteral("id")).toString() == taskId)
                title = value.toObject().value(QStringLiteral("title")).toString();
    auto* dialog = new ket::Dialog(QStringLiteral("%1 · %2").arg(taskId, title), anchor);
    dialog->setObjectName(QStringLiteral("teamTaskDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* body = new TeamTaskDialog(teams, teamId, taskId);
    dialog->setBody(body);
    dialog->setCardWidth(kCardWidth);
    auto* hand = dialog->addButton(tr("Hand over…"), ket::ButtonVariant::Ghost);
    hand->setObjectName(QStringLiteral("teamHandoverOpen"));
    connect(hand, &QAbstractButton::clicked, body, &TeamTaskDialog::openHandover);
    auto* owner = dialog->addButton(tr("Open the owner"));
    owner->setObjectName(QStringLiteral("teamTaskOpenOwner"));
    connect(owner, &QAbstractButton::clicked, body, [body, dialog] {
        const QString session = body->ownerSession_;
        dialog->accept();
        emit body->ownerRequested(session);
    });
    auto* close = dialog->addButton(tr("Close"), ket::ButtonVariant::Primary);
    connect(close, &QAbstractButton::clicked, dialog, &ket::Dialog::accept);
    const auto follow = [hand, owner, body](const bool hasOwner, const bool canHandOver) {
        owner->setVisible(hasOwner);
        hand->setVisible(canHandOver);
        hand->setText(body->ownerSession_.isEmpty() ? tr("Assign…") : tr("Hand over…"));
    };
    connect(body, &TeamTaskDialog::actionsChanged, dialog, follow);
    follow(!body->ownerSession_.isEmpty(), body->canHandOver_);
    dialog->open();
    return dialog;
}

} // namespace ketplus
