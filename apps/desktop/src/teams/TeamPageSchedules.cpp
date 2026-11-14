#include "teams/TeamPage.h"

#include "schedules/ScheduleClient.h"
#include "schedules/ScheduleRules.h"
#include "teams/TeamPageParts.h"
#include "teams/TeamScheduleFields.h"

#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

namespace ketplus {
namespace {

using team_page::px;
using team_page::Text;

QString leaderSession(const QJsonObject& team) {
    for (const QJsonValue value : team.value(QStringLiteral("members")).toArray()) {
        const QJsonObject member = value.toObject();
        if (member.value(QStringLiteral("role")).toString() == QLatin1String("leader"))
            return member.value(QStringLiteral("session_id")).toString();
    }
    return {};
}

QString startLabel(const QJsonObject& start) {
    const QString at = start.value(QStringLiteral("at")).toString();
    const QString zone = start.value(QStringLiteral("timezone")).toString();
    if (at.isEmpty())
        return {};
    return at.left(16).replace(QLatin1Char('T'), QLatin1Char(' ')) +
           (zone.isEmpty() ? QString{} : QStringLiteral(" (") + zone + QLatin1Char(')'));
}

} // namespace

void TeamPage::setScheduleClient(ScheduleClient* client) {
    if (schedules_ == client)
        return;
    if (schedules_ != nullptr)
        disconnect(schedules_, nullptr, this, nullptr);
    schedules_ = client;
    if (schedules_ != nullptr) {
        connect(schedules_, &ScheduleClient::changed, this, [this] { rebuild(); });
        connect(schedules_, &ScheduleClient::runsChanged, this,
                [this](const QString&) { rebuild(); });
        schedules_->refreshTeam(teamId_);
    }
    rebuild();
}

void TeamPage::buildSchedules(const QJsonObject& team) {
    const QJsonObject start = team.value(QStringLiteral("start_at")).toObject();
    const QString when = startLabel(start);
    if (!when.isEmpty()) {
        auto* banner =
            new ket::Banner(ket::BannerTone::Info, tr("Starts at %1").arg(when),
                            tr("The leader will start planning at the scheduled time."), column_);
        banner->setObjectName(QStringLiteral("teamStartBanner"));
        layout_->addWidget(banner);
        auto* actions = new QHBoxLayout;
        auto* startNow = new ket::Button(tr("Start now"), ket::ButtonVariant::Primary, column_);
        auto* change = new ket::Button(tr("Change time"), ket::ButtonVariant::Secondary, column_);
        auto* cancel = new ket::Button(tr("Cancel start"), ket::ButtonVariant::Secondary, column_);
        startNow->setObjectName(QStringLiteral("teamPageStartNow"));
        change->setObjectName(QStringLiteral("teamPageChangeStart"));
        cancel->setObjectName(QStringLiteral("teamPageCancelStart"));
        actions->addWidget(startNow);
        actions->addWidget(change);
        actions->addWidget(cancel);
        actions->addStretch();
        layout_->addLayout(actions);
        connect(startNow, &QAbstractButton::clicked, this, [this] {
            send(QStringLiteral("start"), QString(), QStringLiteral("team.start"), {});
        });
        connect(cancel, &QAbstractButton::clicked, this, [this] {
            auto* dialog = new ket::Dialog(tr("Cancel the scheduled team start?"), this);
            dialog->setObjectName(QStringLiteral("teamCancelStartDialog"));
            auto* body = new QLabel(tr("The team and its conversations will be kept. Choose Start "
                                       "now when you want the leader to begin."),
                                    dialog);
            body->setWordWrap(true);
            dialog->setBody(body);
            auto* keep = dialog->addButton(tr("Keep scheduled start"));
            auto* confirm = dialog->addButton(tr("Cancel start"), ket::ButtonVariant::Danger);
            connect(keep, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
            connect(confirm, &QAbstractButton::clicked, this, [this, dialog] {
                send(QStringLiteral("cancel-start"), QString(),
                     QStringLiteral("team.startAt.cancel"), {});
                dialog->accept();
            });
            dialog->open();
        });
        connect(change, &QAbstractButton::clicked, this, [this, start] {
            auto* dialog = new ket::Dialog(tr("Change time"), this);
            dialog->setObjectName(QStringLiteral("teamChangeStartDialog"));
            auto* fields = new TeamScheduleFields;
            fields->setStartOnly(true);
            const QString at = start.value(QStringLiteral("at")).toString();
            fields->setStart(at.section(QLatin1Char('T'), 0, 0), at.section(QLatin1Char('T'), 1, 1),
                             start.value(QStringLiteral("timezone")).toString());
            dialog->setBody(fields);
            auto* dismiss = dialog->addButton(tr("Cancel"));
            auto* save = dialog->addButton(tr("Save"), ket::ButtonVariant::Primary);
            connect(dismiss, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
            connect(save, &QAbstractButton::clicked, this, [this, dialog, fields] {
                fields->showProblem(QDateTime::currentDateTimeUtc());
                if (!fields->problem(QDateTime::currentDateTimeUtc()).isEmpty())
                    return;
                send(QStringLiteral("change-start"), QString(),
                     QStringLiteral("team.startAt.update"),
                     {{QStringLiteral("start_at"), fields->startAt()}});
                dialog->accept();
            });
            dialog->open();
        });
    }

    addSection(tr("Schedules"), {}, "teamSchedulesHeading");
    int shown = 0;
    if (schedules_ != nullptr) {
        for (const schedule::Schedule& entry : schedules_->schedules()) {
            if (entry.target.teamId != teamId_)
                continue;
            if (entry.target.kind != schedule::TargetKind::Team &&
                entry.target.kind != schedule::TargetKind::TeamStart)
                continue;
            if (entry.target.kind == schedule::TargetKind::TeamStart)
                continue;
            ++shown;
            auto* block = new QWidget(column_);
            block->setObjectName(QStringLiteral("teamSchedule_") + entry.id);
            auto* rows = new QVBoxLayout(block);
            rows->setContentsMargins(0, 0, 0, 0);
            auto* name = new Text(entry.name, block, Text::Fit::Wrap);
            name->setType(14, QFont::Medium);
            rows->addWidget(name);
            const QString summary =
                scheduleRuleSummary(entry.rule) +
                (entry.enabled ? tr(" · next: %1").arg(entry.nextRunAt) : tr(" · paused"));
            auto* line = new Text(summary, block, Text::Fit::Wrap);
            line->setTone(QStringLiteral("text-secondary"));
            rows->addWidget(line);
            const schedule::Run run = schedules_->latestRun(entry.id);
            if (!run.id.isEmpty() && !run.errorMessage.isEmpty()) {
                auto* reason = new Text(run.errorMessage, block, Text::Fit::Wrap);
                reason->setObjectName(QStringLiteral("teamScheduleReason"));
                rows->addWidget(reason);
            } else if (!schedules_->runsLoaded(entry.id)) {
                const QString id = entry.id;
                QTimer::singleShot(0, this, [this, id] {
                    if (schedules_ != nullptr && !schedules_->runsLoaded(id))
                        schedules_->loadRuns(id, false);
                });
            }
            auto* buttons = new QHBoxLayout;
            auto* edit = new ket::Button(tr("Edit"), ket::ButtonVariant::Secondary, block);
            auto* pause = new ket::Button(entry.enabled ? tr("Pause") : tr("Resume"),
                                          ket::ButtonVariant::Secondary, block);
            auto* open =
                new ket::Button(tr("Open in Schedules"), ket::ButtonVariant::Secondary, block);
            edit->setObjectName(QStringLiteral("teamScheduleEdit"));
            pause->setObjectName(QStringLiteral("teamSchedulePause"));
            open->setObjectName(QStringLiteral("teamScheduleOpen"));
            buttons->addWidget(edit);
            buttons->addWidget(pause);
            buttons->addWidget(open);
            const QString leader = leaderSession(team);
            if (!leader.isEmpty()) {
                auto* conversation =
                    new ket::Button(tr("Leader"), ket::ButtonVariant::Ghost, block);
                conversation->setObjectName(QStringLiteral("teamScheduleLeader"));
                buttons->addWidget(conversation);
                connect(conversation, &QAbstractButton::clicked, this,
                        [this, leader] { emit conversationRequested(leader); });
            }
            buttons->addStretch();
            rows->addLayout(buttons);
            layout_->addWidget(block);
            const QString id = entry.id;
            const bool enabled = entry.enabled;
            connect(edit, &QAbstractButton::clicked, this,
                    [this, id] { emit scheduleRequested(id, true); });
            connect(open, &QAbstractButton::clicked, this,
                    [this, id] { emit scheduleRequested(id, false); });
            connect(pause, &QAbstractButton::clicked, this, [this, id, enabled] {
                if (schedules_ == nullptr)
                    return;
                schedules_->request(
                    QStringLiteral("schedule.setEnabled"),
                    {{QStringLiteral("id"), id}, {QStringLiteral("enabled"), !enabled}});
            });
        }
    }
    if (shown == 0) {
        auto* empty =
            new Text(tr("No recurring schedules for this team yet."), column_, Text::Fit::Wrap);
        empty->setObjectName(QStringLiteral("teamSchedulesEmpty"));
        empty->setTone(QStringLiteral("text-secondary"));
        layout_->addWidget(empty);
    }
}

} // namespace ketplus
