#include "teams/TeamTurnBudget.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetTextField.h"
#include "teams/TeamCommands.h"
#include "teams/TeamDirectory.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QPointer>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace ketplus {

namespace team_budget {

QString error(const QString& text, int* value) {
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,4}$"));
    const QString trimmed = text.trimmed();
    const int number = trimmed.toInt();
    if (!digits.match(trimmed).hasMatch() || number < fewest || number > most)
        return QCoreApplication::translate("TeamTurnBudget",
                                           "Automatic turns are a whole number from %1 to %2.")
            .arg(fewest)
            .arg(most);
    if (value)
        *value = number;
    return {};
}

ket::TextField* field(const QString& accessibleName, QWidget* parent) {
    auto* edit = new ket::TextField(parent);
    edit->setAccessibleName(accessibleName);
    edit->setMaxLength(4);
    edit->setInputMethodHints(Qt::ImhDigitsOnly);
    edit->setText(QString::number(standard));
    return edit;
}

void readGlobal(TeamDirectory* teams, QObject* context, std::function<void(int)> done) {
    if (!teams || !teams->isAvailable()) {
        done(standard);
        return;
    }
    const QPointer<QObject> alive(context);
    teams->request(QStringLiteral("team.settings.get"), {},
                   [alive, done = std::move(done)](const QJsonObject& result, const QString&) {
                       if (!alive)
                           return;
                       int budget = standard;
                       const QJsonValue value = result.value(QStringLiteral("settings"))
                                                    .toObject()
                                                    .value(QStringLiteral("turn_budget"));
                       if (!value.isDouble() ||
                           !error(QString::number(value.toInt()), &budget).isEmpty())
                           budget = standard;
                       done(budget);
                   });
}

QString usage(const QJsonObject& team) {
    return QCoreApplication::translate("TeamTurnBudget", "%1 of %2 automatic turns used")
        .arg(team.value(QStringLiteral("automatic_turns")).toInt())
        .arg(team.value(QStringLiteral("turn_budget")).toInt(standard));
}

} // namespace team_budget

TeamTurnBudgetForm::TeamTurnBudgetForm(TeamDirectory* teams, const QJsonObject& team,
                                       QWidget* parent)
    : QWidget(parent), teams_(teams), teamId_(team.value(QStringLiteral("id")).toString()),
      budget_(team.value(QStringLiteral("turn_budget")).toInt(team_budget::standard)),
      field_(team_budget::field(tr("Automatic turns"), this)) {
    setObjectName(QStringLiteral("teamTurnBudgetForm"));
    field_->setObjectName(QStringLiteral("teamTurnBudget"));
    field_->setText(QString::number(budget_));
    const int used = team.value(QStringLiteral("automatic_turns")).toInt();
    const bool budgetPaused =
        team.value(QStringLiteral("paused")).toBool() &&
        team.value(QStringLiteral("paused_reason")).toString() == QStringLiteral("budget");
    QString help = used == 1 ? tr("Applies from the next automatic turn. The 1 turn used stays "
                                  "counted")
                             : tr("Applies from the next automatic turn. The %1 turns used stay "
                                  "counted")
                                   .arg(used);
    help += budgetPaused ? tr("; a budget that leaves turns resumes the team.") : tr(".");
    row_ = new ket::FieldRow(tr("Automatic turns"), field_, help, this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(row_);
    connect(field_, &QLineEdit::textEdited, this, [this] {
        if (!error_.isEmpty())
            showError(team_budget::error(field_->text()));
    });
}

void TeamTurnBudgetForm::showError(const QString& message) {
    error_ = message;
    row_->setError(message);
    field_->setInvalid(!message.isEmpty());
}

void TeamTurnBudgetForm::submit(std::function<void()> saved) {
    int budget = 0;
    showError(team_budget::error(field_->text(), &budget));
    if (!error_.isEmpty()) {
        field_->setFocus();
        return;
    }
    if (budget == budget_) {
        if (saved)
            saved();
        return;
    }
    const QString key = TeamCommands::key(teamId_, QStringLiteral("turn_budget"));
    if (teams_->commands()->isPending(key))
        return;
    const QPointer<TeamTurnBudgetForm> alive(this);
    setEnabled(false);
    teams_->commands()->send(
        key, {}, QStringLiteral("team.update"),
        {{QStringLiteral("team_id"), teamId_}, {QStringLiteral("turn_budget"), budget}},
        [alive, saved = std::move(saved)](const QJsonObject& result, const QString& failure) {
            if (!alive)
                return;
            alive->setEnabled(true);
            if (!failure.isEmpty()) {
                const QString message = result.value(QStringLiteral("error_message")).toString();
                alive->showError(message.isEmpty() ? failure : message);
                return;
            }
            if (saved)
                saved();
        });
}

ket::Dialog* TeamTurnBudgetForm::present(QWidget* anchor, TeamDirectory* teams,
                                         const QJsonObject& team) {
    auto* dialog = new ket::Dialog(
        tr("Automatic turns for %1").arg(team.value(QStringLiteral("name")).toString()), anchor);
    dialog->setObjectName(QStringLiteral("teamTurnBudgetDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* form = new TeamTurnBudgetForm(teams, team);
    dialog->setBody(form);
    dialog->setCardWidth(480);
    auto* cancel = dialog->addButton(tr("Cancel"));
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* save = dialog->addButton(tr("Save"), ket::ButtonVariant::Primary);
    save->setObjectName(QStringLiteral("teamTurnBudgetSave"));
    teams->commands()->bind(save, TeamCommands::key(team.value(QStringLiteral("id")).toString(),
                                                    QStringLiteral("turn_budget")));
    QObject::connect(save, &QAbstractButton::clicked, dialog,
                     [dialog, form] { form->submit([dialog] { dialog->accept(); }); });
    QObject::connect(form->budgetField(), &QLineEdit::returnPressed, save, &QAbstractButton::click);
    dialog->open();
    form->budgetField()->setFocus();
    form->budgetField()->selectAll();
    return dialog;
}

} // namespace ketplus
