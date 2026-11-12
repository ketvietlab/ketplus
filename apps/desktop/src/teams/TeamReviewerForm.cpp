#include "teams/TeamReviewerForm.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetTextField.h"
#include "teams/TeamCommands.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"

#include <QCoreApplication>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("TeamDialogs", text); }

const QString reviewer = QStringLiteral("reviewer");
constexpr int kMostReviewers = 2;

// The same key as an added developer: one new member per team at a time.
QString addKey(const QJsonObject& team) {
    return TeamCommands::key(team.value(QStringLiteral("id")).toString(),
                             QStringLiteral("members/add"));
}

} // namespace

AddReviewerForm::AddReviewerForm(TeamDirectory* teams, const QJsonObject& team,
                                 const QVariantList& models, QWidget* parent)
    : QWidget(parent), teams_(teams), team_(team), models_(models),
      agent_(team_agents::defaults(models).reviewer), name_(new ket::TextField(this)),
      agentButton_(new ket::Button({}, ket::ButtonVariant::Secondary, this)),
      error_(new QLabel(this)) {
    setObjectName(QStringLiteral("addReviewerForm"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto* about = new QLabel(tr("A second reviewer reviews another task at the same time. A task "
                                "goes back to the reviewer of its last round; a new one goes to "
                                "the reviewer with less waiting."),
                             this);
    about->setObjectName(QStringLiteral("addReviewerAbout"));
    about->setWordWrap(true);
    layout->addWidget(about);
    name_->setObjectName(QStringLiteral("addReviewerName"));
    name_->setText(QStringLiteral("Reviewer 2"));
    nameRow_ = new ket::FieldRow(tr("Name"), name_, {}, this);
    layout->addWidget(nameRow_);
    agentButton_->setObjectName(QStringLiteral("teamAgent_reviewer"));
    agentButton_->setText(team_agents::label(models_, agent_));
    agentRow_ = new ket::FieldRow(tr("Agent"), agentButton_,
                                  tr("Reviews stay read-only, which only Claude keeps."), this);
    layout->addWidget(agentRow_);
    connect(agentButton_, &QAbstractButton::clicked, this, [this] {
        const QPointer<AddReviewerForm> alive(this);
        TeamAgentChooser::present(window(), team_roles::label(reviewer), models_, agent_,
                                  QLatin1String(team_agents::reviewerProvider),
                                  [alive](const TeamAgent& agent) {
                                      if (alive)
                                          alive->setAgent(agent);
                                  });
    });
    error_->setObjectName(QStringLiteral("teamDialogError"));
    error_->setWordWrap(true);
    error_->setVisible(false);
    layout->addWidget(error_);
    if (isFull())
        showError(tr("The team has two reviewers, the most a team has."));
    layout->addStretch();
}

bool AddReviewerForm::isFull() const {
    return TeamDirectory::count(team_, reviewer) >= kMostReviewers;
}

QString AddReviewerForm::error() const { return error_->isHidden() ? QString{} : error_->text(); }

void AddReviewerForm::showError(const QString& text) {
    error_->setText(text);
    error_->setVisible(!text.isEmpty());
}

void AddReviewerForm::setAgent(const TeamAgent& agent) {
    agent_ = agent;
    agentButton_->setText(team_agents::label(models_, agent_));
    agentRow_->setError({});
}

QJsonObject AddReviewerForm::payload() const {
    QJsonObject result = agent_.json();
    result.insert(QStringLiteral("team_id"), team_.value(QStringLiteral("id")).toString());
    result.insert(QStringLiteral("role"), reviewer);
    result.insert(QStringLiteral("name"), name_->text().trimmed());
    return result;
}

bool AddReviewerForm::validate() {
    showError({});
    const bool named = !name_->text().trimmed().isEmpty();
    nameRow_->setError(named ? QString{} : tr("Name the reviewer."));
    if (!named)
        name_->setFocus();
    agentRow_->setError(agent_.isEmpty()
                            ? tr("The reviewer needs Claude. Set up Claude in Settings → Agents.")
                            : QString{});
    if (isFull())
        showError(tr("The team has two reviewers, the most a team has."));
    return named && !agent_.isEmpty() && !isFull();
}

void AddReviewerForm::submit(std::function<void()> added) {
    if (!validate() || teams_->commands()->isPending(addKey(team_)))
        return;
    const QPointer<AddReviewerForm> alive(this);
    setEnabled(false);
    teams_->commands()->send(
        addKey(team_), {}, QStringLiteral("team.member.add"), payload(),
        [alive, added = std::move(added)](const QJsonObject&, const QString& error) {
            if (!alive)
                return;
            alive->setEnabled(true);
            if (!error.isEmpty()) {
                alive->showError(error);
                return;
            }
            if (added)
                added();
        });
}

ket::Dialog* AddReviewerForm::present(QWidget* anchor, TeamDirectory* teams,
                                      const QJsonObject& team, const QVariantList& models) {
    auto* dialog = new ket::Dialog(
        tr("Add a second reviewer to %1").arg(team.value(QStringLiteral("name")).toString()),
        anchor);
    dialog->setObjectName(QStringLiteral("addReviewerDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* form = new AddReviewerForm(teams, team, models);
    dialog->setBody(form);
    dialog->setCardWidth(480);
    auto* cancel = dialog->addButton(tr("Cancel"));
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* add = dialog->addButton(tr("Add reviewer"), ket::ButtonVariant::Primary);
    add->setObjectName(QStringLiteral("addReviewerButton"));
    add->setEnabled(!form->isFull());
    teams->commands()->bind(add, addKey(team));
    QObject::connect(add, &QAbstractButton::clicked, dialog,
                     [dialog, form] { form->submit([dialog] { dialog->accept(); }); });
    dialog->open();
    form->nameField()->setFocus();
    return dialog;
}

} // namespace ketplus
