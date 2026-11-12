#include "teams/TeamReviewSettings.h"

#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetToggle.h"
#include "teams/TeamCommands.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamPageParts.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QPointer>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("TeamReviewSettings", text); }

// Two radios side by side, as New team lays out who merges.
QWidget* pair(ket::Radio* first, ket::Radio* second, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* group = new QButtonGroup(row);
    group->addButton(first);
    group->addButton(second);
    layout->addWidget(first);
    layout->addWidget(second);
    layout->addStretch();
    return row;
}

} // namespace

QString team_review::summary(const int rounds, const QString& place) {
    if (place == pullRequest)
        return rounds == 1 ? tr("One review round on the pull request")
                           : tr("Two review rounds on the pull request");
    return rounds == 1 ? tr("One review round on the board") : tr("Two review rounds on the board");
}

QString team_review::summary(const QJsonObject& team) { return summary(rounds(team), place(team)); }

int team_review::rounds(const QJsonObject& team) {
    return team.value(QStringLiteral("max_rounds")).toInt(standardRounds) == 1 ? 1 : 2;
}

QString team_review::place(const QJsonObject& team) {
    return team.value(QStringLiteral("review_place")).toString() == pullRequest ? pullRequest
                                                                                : board;
}

TeamReviewFields::TeamReviewFields(const int rounds, const QString& place, const QString& note,
                                   QWidget* parent)
    : QWidget(parent), note_(note), one_(new ket::Radio(tr("One"), this)),
      two_(new ket::Radio(tr("Two"), this)), board_(new ket::Radio(tr("Board"), this)),
      pullRequest_(new ket::Radio(tr("Pull request"), this)) {
    setObjectName(QStringLiteral("teamReviewFields"));
    one_->setObjectName(QStringLiteral("teamReviewRoundsOne"));
    two_->setObjectName(QStringLiteral("teamReviewRoundsTwo"));
    board_->setObjectName(QStringLiteral("teamReviewPlaceBoard"));
    pullRequest_->setObjectName(QStringLiteral("teamReviewPlacePullRequest"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(team_page::px(12));
    roundsRow_ = new ket::FieldRow(tr("Review rounds"), pair(one_, two_, this), {}, this);
    roundsRow_->setObjectName(QStringLiteral("teamReviewRoundsRow"));
    placeRow_ = new ket::FieldRow(tr("Review on"), pair(board_, pullRequest_, this), {}, this);
    placeRow_->setObjectName(QStringLiteral("teamReviewPlaceRow"));
    layout->addWidget(roundsRow_);
    layout->addWidget(placeRow_);
    for (ket::Radio* radio : {one_, two_, board_, pullRequest_})
        connect(radio, &QAbstractButton::toggled, this, &TeamReviewFields::updateHelp);
    setValues(rounds, place);
}

int TeamReviewFields::rounds() const { return one_->isChecked() ? 1 : 2; }

QString TeamReviewFields::place() const {
    return pullRequest_->isChecked() ? team_review::pullRequest : team_review::board;
}

void TeamReviewFields::setValues(const int rounds, const QString& place) {
    (rounds == 1 ? one_ : two_)->setChecked(true);
    (place == team_review::pullRequest ? pullRequest_ : board_)->setChecked(true);
    updateHelp();
}

void TeamReviewFields::updateHelp() {
    roundsRow_->setHelp(rounds() == 1
                            ? tr("Findings in the only round go to the leader, who decides.")
                            : tr("Findings in the first round go back to the developer; findings "
                                 "in the second go to the leader."));
    QString help = place() == team_review::pullRequest
                       ? tr("The reviewer reads the same diff, read-only. Its findings are posted "
                            "as a comment on the task's pull request; the board keeps the verdict "
                            "and the link. A task without a pull request is reviewed on the "
                            "board.")
                       : tr("The reviewer's findings are kept in the task's history on the "
                            "board.");
    if (!note_.isEmpty())
        help += QLatin1Char(' ') + note_;
    placeRow_->setHelp(help);
}

ket::Dialog* teamReviewSettings(QWidget* anchor, TeamDirectory* teams, const QJsonObject& team) {
    const QString teamId = team.value(QStringLiteral("id")).toString();
    auto* dialog = new ket::Dialog(
        tr("Review for %1").arg(team.value(QStringLiteral("name")).toString()), anchor);
    dialog->setObjectName(QStringLiteral("teamReviewDialog"));
    // The kit's dialog hides when it finishes; this one is made for one change.
    QObject::connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(team_page::px(10));
    auto* fields = new TeamReviewFields(team_review::rounds(team), team_review::place(team),
                                        tr("Applies to tasks that enter review from now; a review "
                                           "in progress keeps its rounds and place."),
                                        body);
    layout->addWidget(fields);
    auto* refusal = new ket::Banner(ket::BannerTone::Danger, tr("Not saved"), {}, body);
    refusal->setObjectName(QStringLiteral("teamReviewRefusal"));
    refusal->hide();
    layout->addWidget(refusal);
    dialog->setBody(body);
    dialog->setCardWidth(480);
    auto* cancel = dialog->addButton(tr("Cancel"), ket::ButtonVariant::Ghost);
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* save = dialog->addButton(tr("Save"), ket::ButtonVariant::Primary);
    save->setObjectName(QStringLiteral("teamReviewSave"));
    const QString key = TeamCommands::key(teamId, QStringLiteral("review"));
    teams->commands()->bind(save, key);
    const int rounds = team_review::rounds(team);
    const QString place = team_review::place(team);
    QObject::connect(save, &QAbstractButton::clicked, dialog,
                     [dialog, teams, teamId, key, fields, refusal, rounds, place] {
                         if (fields->rounds() == rounds && fields->place() == place) {
                             dialog->accept();
                             return;
                         }
                         if (teams->commands()->isPending(key))
                             return;
                         const QPointer<ket::Dialog> alive(dialog);
                         teams->commands()->send(
                             key, {}, QStringLiteral("team.update"),
                             {{QStringLiteral("team_id"), teamId},
                              {QStringLiteral("max_rounds"), fields->rounds()},
                              {QStringLiteral("review_place"), fields->place()}},
                             [alive, refusal](const QJsonObject& result, const QString& failure) {
                                 if (!alive)
                                     return;
                                 if (failure.isEmpty()) {
                                     alive->accept();
                                     return;
                                 }
                                 const QString message =
                                     result.value(QStringLiteral("error_message")).toString();
                                 refusal->setMessage(message.isEmpty() ? failure : message);
                                 refusal->show();
                             });
                     });
    dialog->open();
    return dialog;
}

} // namespace ketplus
