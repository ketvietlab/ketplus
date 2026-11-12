#include "app/SettingsTeamsPage.h"

#include "KetAppearance.h"
#include "KetRecipes.h"
#include "KetTextField.h"
#include "app/SettingsRows.h"
#include "teams/TeamTurnBudget.h"

#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QLabel* textLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(objectName);
    label->setWordWrap(true);
    return label;
}

} // namespace

SettingsTeamsPage::SettingsTeamsPage(Requester requester, QWidget* parent)
    : QWidget(parent), requester_(std::move(requester)),
      field_(team_budget::field(tr("Automatic turns per team"), this)),
      error_(textLabel({}, QStringLiteral("teamsTurnBudgetError"), this)),
      saved_(team_budget::standard) {
    setObjectName(QStringLiteral("settingsWorkspaceTeams"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 24);
    layout->setSpacing(0);
    layout->addWidget(
        textLabel(tr("Teams"), QStringLiteral("settingsWorkspaceSectionTitle"), this));
    layout->addSpacing(7);
    layout->addWidget(textLabel(
        tr("Agent teams work on their own: members start turns when a task, a review or a "
           "message reaches them. After a number of these automatic turns the team pauses and "
           "asks you whether to continue."),
        QStringLiteral("settingsWorkspaceIntro"), this));
    layout->addSpacing(14);
    field_->setObjectName(QStringLiteral("teamsTurnBudget"));
    layout->addWidget(settings_rows::field(
        tr("Automatic turns per team"),
        tr("New teams start with this; change it for one team in its settings. Changing it here "
           "leaves existing teams as they are."),
        field_, this));
    error_->setVisible(false);
    layout->addSpacing(4);
    layout->addWidget(error_);
    layout->addSpacing(14);
    layout->addWidget(textLabel(tr("A whole number from %1 to %2. The default is %3.")
                                    .arg(team_budget::fewest)
                                    .arg(team_budget::most)
                                    .arg(team_budget::standard),
                                QStringLiteral("settingsWorkspaceHelp"), this));
    layout->addStretch(1);
    paintError();
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            &SettingsTeamsPage::paintError);
    connect(field_, &QLineEdit::editingFinished, this, &SettingsTeamsPage::save);
    connect(field_, &QLineEdit::textEdited, this, [this] {
        if (error_->isVisible())
            showError(team_budget::error(field_->text()));
    });
}

QString SettingsTeamsPage::error() const {
    return error_->isVisible() ? error_->text() : QString{};
}

void SettingsTeamsPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
}

void SettingsTeamsPage::refresh() {
    if (!requester_ || pending_ || field_->hasFocus())
        return;
    pending_ = true;
    const QPointer<SettingsTeamsPage> alive(this);
    requester_(QStringLiteral("team.settings.get"), {},
               [alive](const QJsonObject& result, const QString& error) {
                   if (alive)
                       alive->receive(result, error);
               });
}

void SettingsTeamsPage::save() {
    int budget = 0;
    const QString refusal = team_budget::error(field_->text(), &budget);
    showError(refusal);
    if (!refusal.isEmpty() || budget == saved_ || !requester_ || pending_)
        return;
    pending_ = true;
    const QPointer<SettingsTeamsPage> alive(this);
    requester_(QStringLiteral("team.settings.update"), {{QStringLiteral("turn_budget"), budget}},
               [alive](const QJsonObject& result, const QString& error) {
                   if (alive)
                       alive->receive(result, error);
               });
}

void SettingsTeamsPage::receive(const QJsonObject& result, const QString& error) {
    pending_ = false;
    if (!error.isEmpty()) {
        const QString message = result.value(QStringLiteral("error_message")).toString();
        showError(message.isEmpty() ? error : message);
        return;
    }
    const QJsonValue value =
        result.value(QStringLiteral("settings")).toObject().value(QStringLiteral("turn_budget"));
    if (value.isDouble())
        saved_ = value.toInt();
    // A value typed while the daemon answered stays for the next save.
    if (!field_->hasFocus())
        field_->setText(QString::number(saved_));
    showError({});
}

void SettingsTeamsPage::showError(const QString& message) {
    error_->setText(message);
    error_->setVisible(!message.isEmpty());
    field_->setInvalid(!message.isEmpty());
}

void SettingsTeamsPage::paintError() {
    QPalette palette = error_->palette();
    palette.setColor(QPalette::WindowText,
                     ket::fieldRowRecipe(ket::Appearance::instance().theme()).error);
    error_->setPalette(palette);
}

} // namespace ketplus
