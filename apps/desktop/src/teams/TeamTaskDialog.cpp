#include "teams/TeamTaskDialog.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetSelect.h"
#include "KetTextField.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("TeamTaskDialog", text); }

QLabel* line(QWidget* parent, const QString& value, const char* name, bool bold = false) {
    auto* label = new QLabel(value, parent);
    label->setObjectName(QLatin1String(name));
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    if (bold) {
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
    }
    return label;
}

QJsonObject taskOf(const QJsonObject& team, const QString& taskId) {
    for (const auto& value : team.value(QStringLiteral("tasks")).toArray())
        if (value.toObject().value(QStringLiteral("id")).toString() == taskId)
            return value.toObject();
    return {};
}

} // namespace

TeamTaskDialog::TeamTaskDialog(TeamDirectory* teams, const QString& teamId, const QString& taskId,
                               QWidget* parent)
    : QWidget(parent), teams_(teams), teamId_(teamId), taskId_(taskId), details_(new QWidget(this)),
      detailsLayout_(new QVBoxLayout(details_)), owner_(new ket::Select(this)),
      reason_(new ket::TextField(this)), error_(line(this, {}, "teamTaskError")) {
    setObjectName(QStringLiteral("teamTaskDialogBody"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    detailsLayout_->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(details_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumHeight(220);
    layout->addWidget(scroll, 1);
    layout->addWidget(line(this, tr("Hand over"), "teamHandoverHeading", true));
    owner_->setObjectName(QStringLiteral("teamHandoverOwner"));
    reason_->setObjectName(QStringLiteral("teamHandoverReason"));
    reason_->setPlaceholderText(tr("Why it changes hands"));
    layout->addWidget(new ket::FieldRow(tr("New owner"), owner_, {}, this));
    layout->addWidget(new ket::FieldRow(tr("Reason"), reason_, {}, this));
    error_->setVisible(false);
    layout->addWidget(error_);
    connect(teams_, &TeamDirectory::changed, this, &TeamTaskDialog::rebuild);
    rebuild();
}

QString TeamTaskDialog::error() const { return error_->isHidden() ? QString{} : error_->text(); }

void TeamTaskDialog::rebuild() {
    while (QLayoutItem* item = detailsLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto team = teams_->team(teamId_);
    const QJsonObject task = team ? taskOf(*team, taskId_) : QJsonObject{};
    if (task.isEmpty()) {
        detailsLayout_->addWidget(
            line(details_, tr("This task no longer exists."), "teamTaskGone"));
        owner_->setEnabled(false);
        return;
    }
    const QString owner = task.value(QStringLiteral("owner")).toString();
    QStringList facts{team_states::label(task.value(QStringLiteral("state")).toString())};
    if (const QString lane = task.value(QStringLiteral("lane")).toString(); !lane.isEmpty())
        facts.append(tr("lane %1").arg(lane));
    const QJsonObject member = TeamDirectory::member(*team, owner);
    facts.append(owner.isEmpty()
                     ? tr("unassigned")
                     : tr("owned by %1").arg(member.value(QStringLiteral("name")).toString()));
    if (const QString branch = task.value(QStringLiteral("branch")).toString(); !branch.isEmpty())
        facts.append(branch);
    detailsLayout_->addWidget(line(details_, facts.join(QStringLiteral(" · ")), "teamTaskFacts"));
    if (const QString description = task.value(QStringLiteral("description")).toString();
        !description.isEmpty())
        detailsLayout_->addWidget(line(details_, description, "teamTaskDescription"));
    detailsLayout_->addWidget(line(details_, tr("Criteria"), "teamTaskCriteriaHeading", true));
    const QJsonArray criteria = task.value(QStringLiteral("criteria")).toArray();
    if (criteria.isEmpty())
        detailsLayout_->addWidget(line(details_, tr("None yet."), "teamTaskCriterion"));
    for (const auto& value : criteria) {
        const QJsonObject criterion = value.toObject();
        detailsLayout_->addWidget(
            line(details_,
                 (criterion.value(QStringLiteral("met")).toBool() ? QStringLiteral("✓ ")
                                                                  : QStringLiteral("○ ")) +
                     criterion.value(QStringLiteral("text")).toString(),
                 "teamTaskCriterion"));
    }
    QStringList depends;
    for (const auto& value : task.value(QStringLiteral("depends_on")).toArray())
        depends.append(value.toString());
    detailsLayout_->addWidget(line(details_,
                                   depends.isEmpty() ? tr("Depends on nothing.")
                                                     : tr("Depends on %1.").arg(depends.join(", ")),
                                   "teamTaskDependencies"));
    detailsLayout_->addWidget(line(details_, tr("History"), "teamTaskHistoryHeading", true));
    for (const auto& value : task.value(QStringLiteral("history")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString actor = entry.value(QStringLiteral("actor")).toString();
        const QJsonObject who = TeamDirectory::member(*team, actor);
        const QString at =
            QDateTime::fromString(entry.value(QStringLiteral("at")).toString(), Qt::ISODateWithMs)
                .toLocalTime()
                .toString(QStringLiteral("d MMM HH:mm"));
        detailsLayout_->addWidget(
            line(details_,
                 QStringLiteral("%1 · %2 · %3")
                     .arg(at, who.isEmpty() ? actor : who.value(QStringLiteral("name")).toString(),
                          entry.value(QStringLiteral("text")).toString()),
                 "teamTaskHistory"));
    }
    detailsLayout_->addStretch();
    // Another developer of the team; keep the choice across updates.
    const QString chosen = owner_->currentData().toString();
    const QSignalBlocker block(owner_);
    owner_->clear();
    for (const auto& value : team->value(QStringLiteral("members")).toArray()) {
        const QJsonObject candidate = value.toObject();
        if (candidate.value(QStringLiteral("role")).toString() == QStringLiteral("developer") &&
            candidate.value(QStringLiteral("session_id")).toString() != owner)
            owner_->addItem(candidate.value(QStringLiteral("name")).toString(),
                            candidate.value(QStringLiteral("session_id")).toString());
    }
    const int index = owner_->findData(chosen);
    owner_->setCurrentIndex(index >= 0 ? index : 0);
    const bool done = task.value(QStringLiteral("state")).toString() == QStringLiteral("done");
    owner_->setEnabled(owner_->count() > 0 && !done);
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
    teams_->request(QStringLiteral("team.task.handover"),
                    {{QStringLiteral("team_id"), teamId_},
                     {QStringLiteral("task_id"), taskId_},
                     {QStringLiteral("owner"), owner},
                     {QStringLiteral("reason"), reason}},
                    [alive, fail](const QJsonObject&, const QString& error) {
                        if (!alive)
                            return;
                        fail(error);
                        if (error.isEmpty())
                            alive->reason_->clear();
                    });
}

ket::Dialog* TeamTaskDialog::present(QWidget* anchor, TeamDirectory* teams, const QString& teamId,
                                     const QString& taskId) {
    const auto team = teams->team(teamId);
    const QString title =
        team ? taskOf(*team, taskId).value(QStringLiteral("title")).toString() : QString{};
    auto* dialog = new ket::Dialog(QStringLiteral("%1 · %2").arg(taskId, title), anchor);
    dialog->setObjectName(QStringLiteral("teamTaskDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* body = new TeamTaskDialog(teams, teamId, taskId);
    dialog->setBody(body);
    dialog->setCardWidth(560);
    auto* close = dialog->addButton(tr("Close"));
    QObject::connect(close, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* hand = dialog->addButton(tr("Hand over"), ket::ButtonVariant::Primary);
    hand->setObjectName(QStringLiteral("teamHandoverButton"));
    QObject::connect(hand, &QAbstractButton::clicked, body, &TeamTaskDialog::handOver);
    dialog->open();
    return dialog;
}

} // namespace ketplus
