#include "teams/TeamDialogs.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetSegmentedControl.h"
#include "KetSelect.h"
#include "KetTextField.h"
#include "KetToggle.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"

#include <QButtonGroup>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("TeamDialogs", text); }

constexpr int kDefaultDevelopers = 2;
constexpr int kQueueHintDevelopers = 3;
const QString leader = QStringLiteral("leader");
const QString developer = QStringLiteral("developer");
const QString reviewer = QStringLiteral("reviewer");

QLabel* note(QWidget* parent, const char* name) {
    auto* label = new QLabel(parent);
    label->setObjectName(QLatin1String(name));
    label->setWordWrap(true);
    label->setVisible(false);
    return label;
}

void showNote(QLabel* label, const QString& text) {
    label->setText(text);
    label->setVisible(!text.isEmpty());
}

// An agent's label as wide as its button allows; the whole label stays in the tooltip. A
// ket::Button is as wide as its text, so a long model name used to push the form past the card.
class FittedLabel final : public QObject {
  public:
    FittedLabel(ket::Button* button, QString text) : QObject(button), button_(button) {
        button->setMinimumWidth(80);
        button->setSizePolicy(QSizePolicy::Ignored, button->sizePolicy().verticalPolicy());
        button->installEventFilter(this);
        setText(std::move(text));
    }

    void setText(QString text) {
        text_ = std::move(text);
        button_->setToolTip(text_);
        fit();
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == button_ &&
            (event->type() == QEvent::Resize || event->type() == QEvent::Show))
            fit();
        return false;
    }

  private:
    void fit() {
        const QFontMetrics metrics(button_->font());
        // What the button adds around its text, measured on the text it shows now.
        const int chrome = button_->sizeHint().width() - metrics.horizontalAdvance(button_->text());
        const int room = button_->width() - chrome;
        const QString shown = !button_->isVisible() || metrics.horizontalAdvance(text_) <= room
                                  ? text_
                                  : metrics.elidedText(text_, Qt::ElideRight, qMax(0, room));
        if (shown != button_->text())
            button_->setText(shown);
    }

    ket::Button* button_;
    QString text_;
};

void setAgentLabel(ket::Button* button, const QString& text) {
    for (QObject* child : button->children()) {
        if (auto* fitted = dynamic_cast<FittedLabel*>(child)) {
            fitted->setText(text);
            return;
        }
    }
    new FittedLabel(button, text);
}

QJsonArray splitPaths(const QString& text) {
    QJsonArray paths;
    for (const QString& path :
         text.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts))
        paths.append(path);
    return paths;
}

// The body scrolls and the buttons stay: the card fits a 1024 x 640 window at 125 %.
QScrollArea* scrolling(QWidget* form, QWidget* anchor) {
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("teamDialogScroll"));
    scroll->setWidget(form);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const int window = anchor->window()->height();
    scroll->setMaximumHeight(qMax(240, window - 260));
    scroll->setMinimumHeight(qMin(form->sizeHint().height(), qMax(240, window - 260)));
    return scroll;
}

} // namespace

NewTeamForm::NewTeamForm(TeamDirectory* teams, const QString& repository,
                         const QVariantList& models, QWidget* parent)
    : QWidget(parent), teams_(teams), repository_(repository), models_(models),
      name_(new ket::TextField(this)), goal_(new ket::TextField(this)),
      branch_(new ket::TextField(this)), leaderMerges_(new ket::Radio(tr("The leader"), this)),
      userMerges_(new ket::Radio(tr("Me"), this)), permissions_(new ket::Select(this)),
      developers_(new ket::SegmentedControl({"1", "2", "3", "4", "5"}, this)),
      requirements_(new ket::Checkbox(tr("The leader also writes the requirements"), this)),
      hint_(note(this, "teamDialogHint")), error_(note(this, "teamDialogError")) {
    setObjectName(QStringLiteral("newTeamForm"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    name_->setObjectName(QStringLiteral("teamNameField"));
    name_->setPlaceholderText(tr("Checkout"));
    goal_->setObjectName(QStringLiteral("teamGoalField"));
    goal_->setPlaceholderText(tr("One sentence, or an issue"));
    branch_->setObjectName(QStringLiteral("teamIntegrationBranchField"));
    branch_->setPlaceholderText(tr("The branch reviewed tasks merge into"));
    rows_.insert(QStringLiteral("name"), new ket::FieldRow(tr("Name"), name_, {}, this));
    layout->addWidget(rows_.value(QStringLiteral("name")));
    layout->addWidget(new ket::FieldRow(tr("Goal"), goal_, {}, this));
    layout->addWidget(new ket::FieldRow(tr("Integration branch"), branch_, {}, this));

    auto* merges = new QWidget(this);
    auto* mergeRow = new QHBoxLayout(merges);
    mergeRow->setContentsMargins(0, 0, 0, 0);
    leaderMerges_->setObjectName(QStringLiteral("teamLeaderMerges"));
    userMerges_->setObjectName(QStringLiteral("teamUserMerges"));
    auto* group = new QButtonGroup(this);
    group->addButton(leaderMerges_);
    group->addButton(userMerges_);
    leaderMerges_->setChecked(true);
    mergeRow->addWidget(leaderMerges_);
    mergeRow->addWidget(userMerges_);
    mergeRow->addStretch();
    layout->addWidget(new ket::FieldRow(tr("Merges tasks"), merges, {}, this));

    // What the leader and the developers may do without asking, as the composer's Mode
    // lists it. The reviewer is not in it: a review is always read-only.
    permissions_->setObjectName(QStringLiteral("teamPermissions"));
    permissions_->addItem(tr("Read only"), QStringLiteral("read-only"));
    permissions_->addItem(tr("Workspace"), QStringLiteral("ask"));
    permissions_->addItem(tr("Full access"), QStringLiteral("full"));
    permissions_->setCurrentIndex(1);
    permissionsRow_ = new ket::FieldRow(tr("Permissions"), permissions_, {}, this);
    permissionsRow_->setObjectName(QStringLiteral("teamPermissionsRow"));
    connect(permissions_, &QComboBox::currentIndexChanged, this, &NewTeamForm::describePermissions);
    describePermissions();
    layout->addWidget(permissionsRow_);

    const team_agents::Defaults defaults = team_agents::defaults(models_);
    agents_.insert(leader, defaults.leader);
    agents_.insert(developer, defaults.developer);
    agents_.insert(reviewer, defaults.reviewer);
    developers_->setObjectName(QStringLiteral("teamDeveloperCount"));
    developers_->setAccessibleName(tr("Developers"));
    developers_->setCurrentIndex(kDefaultDevelopers - 1);
    for (const QString& role : {leader, developer, reviewer}) {
        auto* button = new ket::Button({}, ket::ButtonVariant::Secondary, this);
        setAgentLabel(button, team_agents::label(models_, agents_.value(role)));
        button->setObjectName(QStringLiteral("teamAgent_") + role);
        button->setAccessibleName(tr("Agent of the %1").arg(team_roles::label(role).toLower()));
        connect(button, &QAbstractButton::clicked, this, [this, role] { chooseAgent(role); });
        agentButtons_.insert(role, button);
        auto* control = new QWidget(this);
        auto* row = new QHBoxLayout(control);
        row->setContentsMargins(0, 0, 0, 0);
        if (role == developer) {
            row->addWidget(developers_);
        } else {
            auto* fixed = new QLabel(QStringLiteral("1"), control);
            fixed->setObjectName(QStringLiteral("teamFixedCount_") + role);
            fixed->setToolTip(tr("A team has one %1.").arg(team_roles::label(role).toLower()));
            row->addWidget(fixed);
        }
        row->addWidget(button, 1);
        const QString label = role == developer ? tr("Developers") : team_roles::label(role);
        auto* field = new ket::FieldRow(label, control, {}, this);
        rows_.insert(role, field);
        layout->addWidget(field);
    }
    requirements_->setObjectName(QStringLiteral("teamLeaderWritesRequirements"));
    requirements_->setChecked(true);
    layout->addWidget(requirements_);
    if (defaults.singleProvider)
        showNote(hint_, tr("Everything runs on one provider. A second provider gives a more "
                           "independent review."));
    layout->addWidget(hint_);
    layout->addWidget(error_);
    layout->addStretch();
    connect(developers_, &ket::SegmentedControl::currentChanged, this,
            [this] { emit memberCountChanged(memberCount()); });
    connect(name_, &QLineEdit::textChanged, this, [this] {
        if (!errors_.value(QStringLiteral("name")).isEmpty() &&
            !name_->text().trimmed().isEmpty()) {
            errors_.remove(QStringLiteral("name"));
            rows_.value(QStringLiteral("name"))->setError({});
        }
    });
}

int NewTeamForm::memberCount() const { return developers_->currentIndex() + 1 + 2; }

void NewTeamForm::setAgent(const QString& role, const TeamAgent& agent) {
    agents_.insert(role, agent);
    setAgentLabel(agentButtons_.value(role), team_agents::label(models_, agent));
    if (errors_.remove(role) > 0)
        rows_.value(role)->setError({});
}

void NewTeamForm::chooseAgent(const QString& role) {
    const QPointer<NewTeamForm> alive(this);
    TeamAgentChooser::present(window(), team_roles::label(role), models_, agents_.value(role),
                              role == reviewer ? QLatin1String(team_agents::reviewerProvider)
                                               : QString{},
                              [alive, role](const TeamAgent& agent) {
                                  if (alive)
                                      alive->setAgent(role, agent);
                              });
}

void NewTeamForm::describePermissions() {
    const QString policy = permissions_->currentData().toString();
    const QString what = policy == QStringLiteral("read-only")
                             ? tr("Inspect files; stays in plan mode.")
                         : policy == QStringLiteral("full") ? tr("Skip permission prompts.")
                                                            : tr("Edit project files; asks before "
                                                                 "commands.");
    permissionsRow_->setHelp(
        tr("%1 The leader and the developers start with this; change it for one member in its "
           "conversation. The reviewer stays read-only.")
            .arg(what));
}

QJsonObject NewTeamForm::payload() const {
    QJsonObject developers = agents_.value(developer).json();
    developers.insert(QStringLiteral("count"), developers_->currentIndex() + 1);
    QJsonObject result{{QStringLiteral("repository"), repository_},
                       {QStringLiteral("name"), name_->text().trimmed()},
                       {QStringLiteral("merge_policy"), userMerges_->isChecked()
                                                            ? QStringLiteral("user")
                                                            : QStringLiteral("leader")},
                       {QStringLiteral("execution_policy"), permissions_->currentData().toString()},
                       {QStringLiteral("leader_writes_requirements"), requirements_->isChecked()},
                       {QStringLiteral("plan"),
                        QJsonObject{{QStringLiteral("leader"), agents_.value(leader).json()},
                                    {QStringLiteral("developers"), developers},
                                    {QStringLiteral("reviewer"), agents_.value(reviewer).json()}}}};
    if (const QString goal = goal_->text().trimmed(); !goal.isEmpty())
        result.insert(QStringLiteral("goal"), goal);
    if (const QString branch = branch_->text().trimmed(); !branch.isEmpty())
        result.insert(QStringLiteral("integration_branch"), branch);
    return result;
}

void NewTeamForm::clearErrors() {
    for (auto it = errors_.cbegin(); it != errors_.cend(); ++it)
        if (auto* row = rows_.value(it.key()))
            row->setError({});
    errors_.clear();
    showNote(error_, {});
}

bool NewTeamForm::validate() {
    clearErrors();
    if (name_->text().trimmed().isEmpty())
        errors_.insert(QStringLiteral("name"), tr("Name the team."));
    for (const QString& role : {leader, developer, reviewer})
        if (agents_.value(role).isEmpty())
            errors_.insert(role, role == reviewer
                                     ? tr("The reviewer needs Claude. Set up Claude in "
                                          "Settings → Agents.")
                                     : tr("Choose an agent."));
    for (auto it = errors_.cbegin(); it != errors_.cend(); ++it)
        rows_.value(it.key())->setError(it.value());
    if (!errors_.value(QStringLiteral("name")).isEmpty())
        name_->setFocus();
    return errors_.isEmpty();
}

void NewTeamForm::showErrors(const QJsonObject& refusal) {
    const QString role = refusal.value(QStringLiteral("role")).toString();
    const QString message = refusal.value(QStringLiteral("error_message")).toString();
    // The team's own name has no role; the daemon names it "The team" (TeamRules validName).
    const bool aboutName = role.isEmpty() && message.contains(QStringLiteral("The team")) &&
                           message.contains(QStringLiteral("name"));
    const QString where = rows_.contains(role) ? role
                          : aboutName          ? QStringLiteral("name")
                                               : QString{};
    errors_.insert(where, message);
    if (where.isEmpty())
        showNote(error_, message);
    else
        rows_.value(where)->setError(message);
}

void NewTeamForm::submit(std::function<void(const QJsonObject& team)> created) {
    if (!validate())
        return;
    const QPointer<NewTeamForm> alive(this);
    setEnabled(false);
    teams_->request(
        QStringLiteral("team.create"), payload(),
        [alive, created = std::move(created)](const QJsonObject& result, const QString& error) {
            if (!alive)
                return;
            alive->setEnabled(true);
            if (!error.isEmpty()) {
                QJsonObject refusal = result;
                if (!refusal.contains(QStringLiteral("error_message")))
                    refusal.insert(QStringLiteral("error_message"), error);
                alive->showErrors(refusal);
                return;
            }
            if (created)
                created(result.value(QStringLiteral("team")).toObject());
        });
}

QString NewTeamForm::hint() const { return hint_->isHidden() ? QString{} : hint_->text(); }

ket::Dialog* NewTeamForm::present(QWidget* anchor, TeamDirectory* teams, const QString& repository,
                                  const QVariantList& models,
                                  std::function<void(const QJsonObject& team)> created) {
    auto* dialog = new ket::Dialog(tr("New team"), anchor);
    dialog->setObjectName(QStringLiteral("newTeamDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* form = new NewTeamForm(teams, repository, models);
    dialog->setBody(scrolling(form, anchor));
    dialog->setCardWidth(560);
    auto* cancel = dialog->addButton(tr("Cancel"));
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* create = dialog->addButton({}, ket::ButtonVariant::Primary);
    create->setObjectName(QStringLiteral("teamCreateButton"));
    const auto label = [create](const int count) {
        create->setText(tr("Create with %1 members").arg(count));
    };
    label(form->memberCount());
    QObject::connect(form, &NewTeamForm::memberCountChanged, create, label);
    QObject::connect(create, &QAbstractButton::clicked, dialog,
                     [dialog, form, created = std::move(created)] {
                         form->submit([dialog, created](const QJsonObject& team) {
                             if (created)
                                 created(team);
                             dialog->accept();
                         });
                     });
    dialog->open();
    form->nameField()->setFocus();
    return dialog;
}

AddDeveloperForm::AddDeveloperForm(TeamDirectory* teams, const QJsonObject& team,
                                   const QVariantList& models, QWidget* parent)
    : QWidget(parent), teams_(teams), team_(team), models_(models),
      agent_(team_agents::defaults(models).developer), name_(new ket::TextField(this)),
      lane_(new ket::TextField(this)), paths_(new ket::TextField(this)),
      agentButton_(new ket::Button({}, ket::ButtonVariant::Secondary, this)),
      nameRow_(new ket::FieldRow(tr("Name"), name_, {}, this)), hint_(note(this, "teamDialogHint")),
      error_(note(this, "teamDialogError")) {
    setObjectName(QStringLiteral("addDeveloperForm"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    name_->setObjectName(QStringLiteral("developerNameField"));
    lane_->setObjectName(QStringLiteral("developerLaneField"));
    lane_->setPlaceholderText(tr("Web, API, …"));
    paths_->setObjectName(QStringLiteral("developerPathsField"));
    paths_->setPlaceholderText(tr("src/web, docs/web"));
    agentButton_->setObjectName(QStringLiteral("teamAgent_developer"));
    setAgentLabel(agentButton_, team_agents::label(models_, agent_));
    layout->addWidget(nameRow_);
    layout->addWidget(new ket::FieldRow(tr("Lane"), lane_, {}, this));
    layout->addWidget(new ket::FieldRow(
        tr("Paths"), paths_, tr("Relative to the repository root, separated by commas."), this));
    layout->addWidget(new ket::FieldRow(tr("Agent"), agentButton_, {}, this));
    connect(agentButton_, &QAbstractButton::clicked, this, [this] {
        const QPointer<AddDeveloperForm> alive(this);
        TeamAgentChooser::present(window(), tr("Developer"), models_, agent_, {},
                                  [alive](const TeamAgent& agent) {
                                      if (alive)
                                          alive->setAgent(agent);
                                  });
    });
    const int developers = TeamDirectory::count(team_, developer);
    if (isFull())
        showNote(hint_, tr("A team has at most five developers."));
    else if (developers + 1 >= kQueueHintDevelopers)
        showNote(hint_, tr("With %1 developers the single reviewer becomes the queue: tasks "
                           "wait their turn for review.")
                            .arg(developers + 1));
    layout->addWidget(hint_);
    layout->addWidget(error_);
    layout->addStretch();
}

bool AddDeveloperForm::isFull() const {
    return TeamDirectory::count(team_, developer) >= team_roles::maximumDevelopers;
}

QString AddDeveloperForm::hint() const { return hint_->isHidden() ? QString{} : hint_->text(); }
QString AddDeveloperForm::error() const { return error_->isHidden() ? QString{} : error_->text(); }

void AddDeveloperForm::setAgent(const TeamAgent& agent) {
    agent_ = agent;
    setAgentLabel(agentButton_, team_agents::label(models_, agent_));
}

QJsonObject AddDeveloperForm::payload() const {
    QJsonObject result = agent_.json();
    result.insert(QStringLiteral("team_id"), team_.value(QStringLiteral("id")).toString());
    result.insert(QStringLiteral("name"), name_->text().trimmed());
    if (const QString lane = lane_->text().trimmed(); !lane.isEmpty())
        result.insert(QStringLiteral("lane"), lane);
    if (const QJsonArray paths = splitPaths(paths_->text()); !paths.isEmpty())
        result.insert(QStringLiteral("paths"), paths);
    return result;
}

bool AddDeveloperForm::validate() {
    showNote(error_, {});
    const bool named = !name_->text().trimmed().isEmpty();
    nameRow_->setError(named ? QString{} : tr("Name the developer."));
    if (!named)
        name_->setFocus();
    if (agent_.isEmpty())
        showNote(error_, tr("Choose the developer's agent."));
    return named && !agent_.isEmpty() && !isFull();
}

void AddDeveloperForm::submit(std::function<void()> added) {
    if (!validate())
        return;
    const QPointer<AddDeveloperForm> alive(this);
    setEnabled(false);
    teams_->request(QStringLiteral("team.member.add"), payload(),
                    [alive, added = std::move(added)](const QJsonObject&, const QString& error) {
                        if (!alive)
                            return;
                        alive->setEnabled(true);
                        if (!error.isEmpty()) {
                            showNote(alive->error_, error);
                            return;
                        }
                        if (added)
                            added();
                    });
}

ket::Dialog* AddDeveloperForm::present(QWidget* anchor, TeamDirectory* teams,
                                       const QJsonObject& team, const QVariantList& models) {
    auto* dialog = new ket::Dialog(
        tr("Add a developer to %1").arg(team.value(QStringLiteral("name")).toString()), anchor);
    dialog->setObjectName(QStringLiteral("addDeveloperDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* form = new AddDeveloperForm(teams, team, models);
    dialog->setBody(scrolling(form, anchor));
    dialog->setCardWidth(480);
    auto* cancel = dialog->addButton(tr("Cancel"));
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* add = dialog->addButton(tr("Add developer"), ket::ButtonVariant::Primary);
    add->setObjectName(QStringLiteral("addDeveloperButton"));
    add->setEnabled(!form->isFull());
    QObject::connect(add, &QAbstractButton::clicked, dialog,
                     [dialog, form] { form->submit([dialog] { dialog->accept(); }); });
    dialog->open();
    form->nameField()->setFocus();
    return dialog;
}

} // namespace ketplus
