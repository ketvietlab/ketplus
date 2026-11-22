#include "teams/TeamProviderDialog.h"

#include "agent/ProviderDisplay.h"

#include "KetBanner.h"
#include "KetButton.h"
#include "KetChoice.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetSelect.h"
#include "KetToggle.h"
#include "teams/TeamAgentDialog.h"
#include "teams/TeamCommands.h"
#include "teams/TeamDialogs.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"
#include "teams/TeamPageParts.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QPointer>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("TeamProviderDialog", text); }

const QString codex = QStringLiteral("codex");

QString providerKey(const QJsonObject& team) {
    return TeamCommands::key(team.value(QStringLiteral("id")).toString(),
                             QStringLiteral("provider"));
}

bool fullAccess(const QJsonObject& team) {
    return team.value(QStringLiteral("execution_policy")).toString() == QStringLiteral("full");
}

QString memberTitle(const QJsonObject& member) {
    const QString number = member.value(QStringLiteral("number")).toString();
    const QString name = member.value(QStringLiteral("name")).toString();
    return number.isEmpty() ? name : QStringLiteral("#%1 · %2").arg(number, name);
}

} // namespace

QString team_provider::suggested(const QJsonObject& team, const QStringList& ready) {
    QHash<QString, int> runs;
    QString most;
    for (const QJsonValue& value : team.value(QStringLiteral("members")).toArray()) {
        const QString provider = value.toObject().value(QStringLiteral("provider")).toString();
        if (++runs[provider] > runs.value(most))
            most = provider;
    }
    // Codex <-> Claude, the usual move; else the first other provider set up.
    const QString other = most == codex ? QStringLiteral("claude") : codex;
    if (ready.contains(other))
        return other;
    for (const QString& provider : ready)
        if (provider != most)
            return provider;
    return ready.isEmpty() ? QString{} : ready.first();
}

QString team_provider::memberLine(const QJsonObject& member) {
    QStringList parts{team_roles::label(member.value(QStringLiteral("role")).toString()),
                      provider_display::name(member.value(QStringLiteral("provider")).toString())};
    const QString pending = member.value(QStringLiteral("pending_switch"))
                                .toObject()
                                .value(QStringLiteral("provider"))
                                .toString();
    if (!pending.isEmpty())
        parts.append(tr("switches to %1 at its next turn").arg(provider_display::name(pending)));
    if (!member.value(QStringLiteral("idle")).toBool(true))
        parts.append(tr("running a turn: switches when it ends"));
    return parts.join(QStringLiteral(" · "));
}

QString team_provider::resultLine(const QJsonObject& result, const QString& provider) {
    const QString name = provider_display::name(provider);
    const QString status = result.value(QStringLiteral("status")).toString();
    if (status == QStringLiteral("pending"))
        return result.value(QStringLiteral("applies")).toString() ==
                       QStringLiteral("after_current_turn")
                   ? tr("Switches to %1 when its current turn ends").arg(name)
                   : tr("Switches to %1 at its next turn").arg(name);
    if (status == QStringLiteral("unchanged"))
        return tr("Already on %1").arg(name);
    const QString reason = result.value(QStringLiteral("reason")).toString();
    return reason.isEmpty() ? tr("Not switched") : tr("Not switched: %1").arg(reason);
}

TeamProviderForm::TeamProviderForm(TeamDirectory* teams, const QJsonObject& team,
                                   const QVariantList& models, QWidget* parent)
    : QWidget(parent), teams_(teams), team_(team), models_(models), fields_(new QWidget(this)),
      chooser_(new TeamAgentChooser(
          models,
          team_agents::defaultFor(
              models, team_provider::suggested(team, team_agents::readyProviders(models))),
          AgentChoice::team, fields_)),
      all_(new ket::Checkbox(tr("All members"), fields_)),
      access_(new ket::Banner(ket::BannerTone::Warning, tr("A Codex reviewer needs Full access"),
                              {}, fields_)),
      grant_(new ket::Choice(tr("Give the team Full access so the reviewer runs on Codex"),
                             tr("Every member skips permission prompts from its next turn; a turn "
                                "running now keeps its permissions."),
                             ket::ToggleKind::Checkbox, fields_)),
      summary_(new ket::Banner(ket::BannerTone::Info, {}, {}, fields_)),
      refusal_(new ket::Banner(ket::BannerTone::Danger, tr("Not switched"), {}, this)),
      results_(new QWidget(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(team_page::px(12));
    auto* fields = new QVBoxLayout(fields_);
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setSpacing(team_page::px(12));
    fields->addWidget(chooser_);

    // Every member is checked: the change is for the whole team unless one is cleared.
    auto* list = new QWidget(fields_);
    auto* rows = new QVBoxLayout(list);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(team_page::px(4));
    all_->setObjectName(QStringLiteral("teamProviderAll"));
    all_->setChecked(true);
    rows->addWidget(all_);
    for (const QJsonValue& value : team.value(QStringLiteral("members")).toArray()) {
        const QJsonObject member = value.toObject();
        const QString session = member.value(QStringLiteral("session_id")).toString();
        auto* choice = new ket::Choice(memberTitle(member), team_provider::memberLine(member),
                                       ket::ToggleKind::Checkbox, list);
        choice->setObjectName(QStringLiteral("teamProviderMember_") + session);
        choice->setChecked(true);
        members_.insert(session, choice);
        order_.append(session);
        rows->addWidget(choice);
        connect(choice, &QAbstractButton::toggled, this, [this] {
            const QSignalBlocker block(all_);
            all_->setChecked(chosen().size() == order_.size());
            describe();
        });
    }
    connect(all_, &QAbstractButton::toggled, this, [this](const bool on) {
        for (ket::Choice* choice : std::as_const(members_)) {
            const QSignalBlocker block(choice);
            choice->setChecked(on);
        }
        describe();
    });
    auto* membersRow = new ket::FieldRow(tr("Members"), list,
                                         tr("Every member switches unless you clear it."), fields_);
    membersRow->setStacked(true);
    fields->addWidget(membersRow);

    access_->setObjectName(QStringLiteral("teamProviderAccess"));
    access_->setMessage(tr("Claude keeps a review read-only. Codex reviews only when the team "
                           "starts members with Full access, which skips permission prompts for "
                           "every member."));
    fields->addWidget(access_);
    grant_->setObjectName(QStringLiteral("teamProviderFullAccess"));
    fields->addWidget(grant_);
    connect(grant_, &QAbstractButton::toggled, this, &TeamProviderForm::describe);
    summary_->setObjectName(QStringLiteral("teamProviderSummary"));
    fields->addWidget(summary_);
    layout->addWidget(fields_);

    refusal_->setObjectName(QStringLiteral("teamProviderRefusal"));
    refusal_->hide();
    layout->addWidget(refusal_);
    results_->setObjectName(QStringLiteral("teamProviderResults"));
    results_->hide();
    layout->addWidget(results_);

    connect(chooser_->providers(), &QComboBox::currentIndexChanged, this,
            &TeamProviderForm::describe);
    describe();
}

QStringList TeamProviderForm::chosen() const {
    QStringList result;
    for (const QString& session : order_)
        if (members_.value(session)->isChecked())
            result.append(session);
    return result;
}

bool TeamProviderForm::reviewerNeedsAccess() const {
    if (chooser_->agent().provider != codex || fullAccess(team_))
        return false;
    for (const QJsonValue& value : team_.value(QStringLiteral("members")).toArray()) {
        const QJsonObject member = value.toObject();
        if (member.value(QStringLiteral("role")).toString() == QStringLiteral("reviewer") &&
            members_.value(member.value(QStringLiteral("session_id")).toString())->isChecked())
            return true;
    }
    return false;
}

bool TeamProviderForm::grantsFullAccess() const {
    return reviewerNeedsAccess() && grant_->isChecked();
}

QStringList TeamProviderForm::switching() const {
    QStringList result = chosen();
    if (!reviewerNeedsAccess() || grant_->isChecked())
        return result;
    for (const QJsonValue& value : team_.value(QStringLiteral("members")).toArray())
        if (value.toObject().value(QStringLiteral("role")).toString() == QStringLiteral("reviewer"))
            result.removeAll(value.toObject().value(QStringLiteral("session_id")).toString());
    return result;
}

QJsonObject TeamProviderForm::payload() const {
    const TeamAgent agent = chooser_->agent();
    QJsonObject payload{{QStringLiteral("team_id"), team_.value(QStringLiteral("id"))},
                        {QStringLiteral("provider"), agent.provider}};
    if (!agent.model.isEmpty())
        payload.insert(QStringLiteral("model"), agent.model);
    if (!agent.effort.isEmpty())
        payload.insert(QStringLiteral("effort"), agent.effort);
    // Without "members" the whole team moves and the provider becomes the team's own.
    const QStringList sent = switching();
    if (sent.size() != order_.size())
        payload.insert(QStringLiteral("members"), QJsonArray::fromStringList(sent));
    // The acknowledgement Create asks for; the daemon refuses Full access without it.
    if (grantsFullAccess()) {
        payload.insert(QStringLiteral("execution_policy"), QStringLiteral("full"));
        payload.insert(QStringLiteral("risk_acknowledged"), true);
    }
    return payload;
}

void TeamProviderForm::describe() {
    const QString provider = provider_display::name(chooser_->agent().provider);
    const QStringList sent = switching();
    int running = 0;
    for (const QJsonValue& value : team_.value(QStringLiteral("members")).toArray())
        if (sent.contains(value.toObject().value(QStringLiteral("session_id")).toString()) &&
            !value.toObject().value(QStringLiteral("idle")).toBool(true))
            ++running;
    access_->setVisible(reviewerNeedsAccess());
    grant_->setVisible(reviewerNeedsAccess());
    if (sent.isEmpty()) {
        summary_->setTitle(tr("No member is chosen"));
        summary_->setMessage(tr("Check the members that move to %1.").arg(provider));
    } else {
        summary_->setTitle(tr("Each member starts a fresh context"));
        QString message = tr("A member's next turn runs on %1 with the conversation so far as a "
                             "transcript it reads when needed; nothing is replayed.")
                              .arg(provider);
        if (running > 0)
            message += QLatin1Char(' ') +
                       (running == 1 ? tr("The one running now switches when its turn ends; no "
                                          "turn is interrupted.")
                                     : tr("The %1 running now switch when their turn ends; no "
                                          "turn is interrupted.")
                                           .arg(running));
        if (reviewerNeedsAccess() && !grant_->isChecked())
            message += QLatin1Char(' ') + tr("The reviewer stays on its provider.");
        summary_->setMessage(message);
    }
    emit changed();
}

void TeamProviderForm::submit(std::function<void()> sent) {
    error_.clear();
    refusal_->hide();
    if (switching().isEmpty())
        return;
    const QString provider = chooser_->agent().provider;
    const QPointer<TeamProviderForm> alive(this);
    teams_->commands()->send(
        providerKey(team_), {}, QStringLiteral("team.provider.set"), payload(),
        [alive, provider, sent = std::move(sent)](const QJsonObject& result,
                                                  const QString& failure) {
            if (!alive)
                return;
            if (!failure.isEmpty()) {
                const QString message = result.value(QStringLiteral("error_message")).toString();
                alive->error_ = message.isEmpty() ? failure : message;
                alive->refusal_->setMessage(alive->error_);
                alive->refusal_->show();
                return;
            }
            alive->sentProvider_ = provider;
            alive->showResults(result.value(QStringLiteral("results")).toArray());
            if (sent)
                sent();
        });
}

void TeamProviderForm::showResults(const QJsonArray& results) {
    const QString& provider = sentProvider_;
    auto* layout = new QVBoxLayout(results_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(team_page::px(10));
    int moving = 0;
    for (const QJsonValue& value : results)
        if (value.toObject().value(QStringLiteral("status")).toString() ==
            QStringLiteral("pending"))
            ++moving;
    auto* banner = new ket::Banner(
        moving > 0 ? ket::BannerTone::Success : ket::BannerTone::Info,
        moving == 0 ? tr("No member moves")
        : moving == 1
            ? tr("1 member moves to %1").arg(provider_display::name(provider))
            : tr("%1 members move to %2").arg(moving).arg(provider_display::name(provider)),
        tr("Each one switches at its next turn; a running turn finishes first."), results_);
    banner->setObjectName(QStringLiteral("teamProviderDone"));
    layout->addWidget(banner);
    for (const QJsonValue& value : results) {
        const QJsonObject result = value.toObject();
        const QString session = result.value(QStringLiteral("session_id")).toString();
        auto* row = new QWidget(results_);
        row->setObjectName(QStringLiteral("teamProviderResult_") + session);
        auto* lines = new QVBoxLayout(row);
        lines->setContentsMargins(0, 0, 0, 0);
        lines->setSpacing(team_page::px(2));
        QJsonObject member;
        for (const QJsonValue& entry : team_.value(QStringLiteral("members")).toArray())
            if (entry.toObject().value(QStringLiteral("session_id")).toString() == session)
                member = entry.toObject();
        auto* name =
            new team_page::Text(member.isEmpty() ? result.value(QStringLiteral("name")).toString()
                                                 : memberTitle(member),
                                row);
        name->setType(13, QFont::Medium);
        auto* line = new team_page::Text(team_provider::resultLine(result, provider), row);
        line->setObjectName(QStringLiteral("teamProviderResultLine"));
        line->setType(12);
        line->setTone(result.value(QStringLiteral("status")).toString() == QStringLiteral("skipped")
                          ? QStringLiteral("danger")
                          : QStringLiteral("text-secondary"));
        lines->addWidget(name);
        lines->addWidget(line);
        layout->addWidget(row);
    }
    fields_->hide();
    results_->show();
    showsResults_ = true;
    emit resultsShown();
}

ket::Dialog* TeamProviderForm::present(QWidget* anchor, TeamDirectory* teams,
                                       const QJsonObject& team, const QVariantList& models) {
    auto* dialog = new ket::Dialog(
        tr("Change provider for %1").arg(team.value(QStringLiteral("name")).toString()), anchor);
    dialog->setObjectName(QStringLiteral("teamProviderDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* form = new TeamProviderForm(teams, team, models);
    QScrollArea* scroll = team_dialogs::scrolling(form, anchor);
    dialog->setBody(scroll);
    dialog->setCardWidth(560);
    const auto fit = [scroll, form] {
        QTimer::singleShot(0, scroll, [scroll, form] {
            scroll->setMinimumHeight(qMin(form->sizeHint().height(), scroll->maximumHeight()));
        });
    };
    auto* cancel = dialog->addButton(tr("Cancel"));
    QObject::connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* send = dialog->addButton({}, ket::ButtonVariant::Primary);
    send->setObjectName(QStringLiteral("teamProviderSwitch"));
    const auto label = [send, form,
                        count = int(team.value(QStringLiteral("members")).toArray().size())] {
        const int sent = int(form->switching().size());
        send->setText(sent == count && sent > 1 ? tr("Switch all %1 members").arg(sent)
                      : sent == 1               ? tr("Switch 1 member")
                                                : tr("Switch %1 members").arg(sent));
        send->setEnabled(sent > 0);
    };
    label();
    QObject::connect(form, &TeamProviderForm::changed, send, label);
    QObject::connect(form, &TeamProviderForm::changed, scroll, fit);
    teams->commands()->bind(send, providerKey(team));
    QObject::connect(send, &QAbstractButton::clicked, dialog, [form] { form->submit(); });
    // Once sent, the dialog shows each member's result and only closes.
    QObject::connect(form, &TeamProviderForm::resultsShown, dialog, [dialog, cancel, send, fit] {
        send->hide();
        cancel->setText(tr("Done"));
        fit();
    });
    dialog->open();
    fit();
    form->chooser()->providers()->setFocus();
    return dialog;
}

} // namespace ketplus
