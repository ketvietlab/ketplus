// The page's sections that are lists of rows: what waits for the user, the members, the updates.
#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetButton.h"
#include "KetIcon.h"
#include "KetTokens.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"
#include "teams/TeamPage.h"
#include "teams/TeamPageParts.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QSet>
#include <QVBoxLayout>

#include <algorithm>

namespace ketplus {
namespace {

using team_page::px;
using team_page::Text;

// Past this a choice is a sentence, not a button's label (mockups/qt, Teams).
constexpr int kButtonChoiceLength = 28;
constexpr int kUpdateLines = 3;

QString leaderOf(const QJsonObject& team) {
    for (const auto& value : team.value(QStringLiteral("members")).toArray())
        if (value.toObject().value(QStringLiteral("role")).toString() == QStringLiteral("leader"))
            return value.toObject().value(QStringLiteral("session_id")).toString();
    return {};
}

// Today's time alone; an earlier day with its date.
QString when(const QString& iso) {
    const QDateTime at = QDateTime::fromString(iso, Qt::ISODateWithMs).toLocalTime();
    return at.toString(at.date() == QDate::currentDate() ? QStringLiteral("HH:mm")
                                                         : QStringLiteral("d MMM, HH:mm"));
}

ket::BadgeTone roleTone(const QString& role) {
    return role == QStringLiteral("leader")     ? ket::BadgeTone::Accent
           : role == QStringLiteral("reviewer") ? ket::BadgeTone::Info
                                                : ket::BadgeTone::Neutral;
}

ket::Icon* glyph(const QString& name, const QStringView tone, QWidget* parent) {
    auto* icon = new ket::Icon(name, 14, parent);
    icon->setColor(ket::Tokens::color(tone, ket::Appearance::instance().theme()));
    return icon;
}

// Two lines of a member's or an update's row.
QVBoxLayout* pair(QWidget* first, QWidget* second) {
    auto* lines = new QVBoxLayout;
    lines->setContentsMargins(0, 0, 0, 0);
    lines->setSpacing(px(5));
    lines->addWidget(first);
    if (second)
        lines->addWidget(second);
    return lines;
}

} // namespace

void TeamPage::buildDecisions(const QJsonObject& team) {
    QList<QJsonObject> open;
    for (const auto& value : team.value(QStringLiteral("decisions")).toArray())
        if (value.toObject().value(QStringLiteral("state")).toString() == QStringLiteral("open"))
            open.append(value.toObject());
    // Absent when nothing waits.
    if (open.isEmpty())
        return;
    addSection(tr("Waiting for you"),
               open.size() == 1 ? tr("1 decision") : tr("%1 decisions").arg(open.size()),
               "teamWaitingHeading");
    for (const QJsonObject& decision : std::as_const(open)) {
        const QString id = decision.value(QStringLiteral("id")).toString();
        const QString question = decision.value(QStringLiteral("question")).toString();
        auto* card = new team_page::Panel(column_, true);
        card->setObjectName(QStringLiteral("teamDecision_") + id);
        card->setAccessibleName(question);
        auto* column = new QVBoxLayout(card);
        column->setContentsMargins(px(18), px(16), px(18), px(16));
        column->setSpacing(px(6));
        auto* asked = new Text(question, card);
        asked->setObjectName(QStringLiteral("teamDecisionQuestion"));
        asked->setType(15, QFont::Medium);
        column->addWidget(asked);
        if (const QString context = decision.value(QStringLiteral("context")).toString();
            !context.isEmpty()) {
            auto* detail = new Text(context, card);
            detail->setObjectName(QStringLiteral("teamDecisionContext"));
            detail->setTone(QStringLiteral("text-secondary"));
            column->addWidget(detail);
        }
        column->addSpacing(px(4));

        QStringList ordered;
        for (const auto& value : decision.value(QStringLiteral("choices")).toArray())
            ordered.append(value.toString());
        const QString recommended = decision.value(QStringLiteral("recommended")).toString();
        if (ordered.removeAll(recommended) > 0)
            ordered.prepend(recommended);
        auto* error = new Text({}, card);
        error->setObjectName(QStringLiteral("teamDecisionError"));
        error->setTone(QStringLiteral("danger"));
        error->setVisible(false);
        const auto answers = [this, id, error](QAbstractButton* button, const QString& choice) {
            button->setObjectName(QStringLiteral("teamDecisionChoice"));
            button->setProperty("choice", choice);
            connect(button, &QAbstractButton::clicked, this, [this, id, choice, error] {
                send(QStringLiteral("team.decision.answer"),
                     {{QStringLiteral("decision_id"), id}, {QStringLiteral("choice"), choice}},
                     error);
            });
        };
        // A choice written as a sentence is a row of its own; short choices are buttons.
        const bool sentences =
            std::any_of(ordered.cbegin(), ordered.cend(),
                        [](const QString& choice) { return choice.size() > kButtonChoiceLength; });
        auto* actions = new QWidget(card);
        auto* flow = new team_page::Flow(actions, px(8), px(8));
        for (const QString& choice : std::as_const(ordered)) {
            const bool best = choice == recommended;
            if (sentences) {
                auto* row = new team_page::ChoiceRow(choice, best, card);
                answers(row, choice);
                column->addWidget(row);
                continue;
            }
            auto* button = new team_page::ChoiceButton(
                best ? tr("%1 (recommended)").arg(choice) : choice,
                best ? ket::ButtonVariant::Primary : ket::ButtonVariant::Secondary, actions);
            answers(button, choice);
            flow->addWidget(button);
        }
        auto* elsewhere =
            new ket::Button(tr("Answer in the conversation"), ket::ButtonVariant::Ghost, actions);
        elsewhere->setObjectName(QStringLiteral("teamDecisionInConversation"));
        connect(elsewhere, &QAbstractButton::clicked, this,
                [this, leader = leaderOf(team)] { emit conversationRequested(leader); });
        flow->addWidget(elsewhere);

        // Beside the choices: who asked, when, and what the answer unblocks.
        const QString raisedBy = decision.value(QStringLiteral("raised_by")).toString();
        QStringList about{
            id, raisedBy == QStringLiteral("daemon") ? tr("KetPlus") : memberName(team, raisedBy),
            when(decision.value(QStringLiteral("raised_at")).toString())};
        if (const QString task = decision.value(QStringLiteral("task_id")).toString();
            !task.isEmpty())
            about.append(tr("blocks %1").arg(task));
        auto* meta = new Text(about.join(QStringLiteral(" · ")), card, Text::Fit::Elide);
        meta->setObjectName(QStringLiteral("teamDecisionAbout"));
        meta->setTone(QStringLiteral("text-muted"));
        meta->setType(12);
        auto* foot = new QHBoxLayout;
        foot->setContentsMargins(0, sentences ? px(2) : 0, 0, 0);
        foot->setSpacing(px(12));
        foot->addWidget(actions, 1);
        if (roomy_) {
            foot->addWidget(meta, 0, Qt::AlignVCenter);
            column->addLayout(foot);
        } else {
            column->addLayout(foot);
            column->addWidget(meta);
        }
        column->addWidget(error);
        layout_->addWidget(card);
    }
}

void TeamPage::buildMembers(const QJsonObject& team) {
    const int developers = TeamDirectory::count(team, QStringLiteral("developer"));
    addSection(tr("Members"),
               tr("1 leader · %1 · 1 reviewer")
                   .arg(developers == 1 ? tr("1 developer") : tr("%1 developers").arg(developers)),
               "teamMembersHeading");
    auto* panel = new team_page::Panel(column_);
    panel->setObjectName(QStringLiteral("teamMembersPanel"));
    auto* rows = new QVBoxLayout(panel);
    rows->setContentsMargins(1, 1, 1, 1);
    rows->setSpacing(0);
    bool first = true;
    for (const auto& value : team.value(QStringLiteral("members")).toArray()) {
        const QJsonObject member = value.toObject();
        const QString session = member.value(QStringLiteral("session_id")).toString();
        const QString role = member.value(QStringLiteral("role")).toString();
        const QString number = member.value(QStringLiteral("number")).toString();
        auto* row = new team_page::Row(panel, !first);
        first = false;
        row->setObjectName(QStringLiteral("teamMember_") + session);
        row->setMinimumHeight(px(64));
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(px(18), px(10), px(12), px(10));
        line->setSpacing(px(16));

        // Who: the conversation's name, then its number and what runs it.
        auto* name =
            new Text(member.value(QStringLiteral("name")).toString(), row, Text::Fit::Elide);
        name->setObjectName(QStringLiteral("teamMemberName"));
        name->setType(14, QFont::Medium);
        auto* badge = new ket::Badge(team_roles::label(role), roleTone(role), row);
        badge->setObjectName(QStringLiteral("teamMemberRole"));
        auto* titled = new QHBoxLayout;
        titled->setSpacing(px(8));
        titled->addWidget(name);
        // A narrow page has no role column: the role sits beside the name.
        if (!roomy_)
            titled->addWidget(badge);
        titled->addStretch(1);
        auto* identity = new QHBoxLayout;
        identity->setSpacing(px(8));
        if (!number.isEmpty()) {
            auto* reference = new Text(QStringLiteral("#") + number, row, Text::Fit::Elide);
            reference->setTone(QStringLiteral("text-secondary"));
            reference->setType(12, QFont::Medium, true);
            identity->addWidget(reference);
        }
        auto* provider =
            new Text(member.value(QStringLiteral("provider")).toString(), row, Text::Fit::Elide);
        provider->setTone(QStringLiteral("text-muted"));
        provider->setType(12);
        identity->addWidget(provider, 1);
        auto* who = new QVBoxLayout;
        who->setSpacing(px(5));
        who->addLayout(titled);
        who->addLayout(identity);
        line->addLayout(who, 4);

        if (roomy_) {
            auto* slot = new QHBoxLayout;
            slot->addWidget(badge);
            slot->addStretch(1);
            auto* holder = new QWidget(row);
            holder->setLayout(slot);
            slot->setContentsMargins(0, 0, 0, 0);
            holder->setFixedWidth(px(104));
            line->addWidget(holder);
        }

        // What it holds: a developer's lane and its paths, or what the role does.
        const bool developer = role == QStringLiteral("developer");
        const QString lane = member.value(QStringLiteral("lane")).toString();
        auto* holds =
            new Text(developer ? (lane.isEmpty() ? tr("No lane yet") : lane)
                     : role == QStringLiteral("reviewer") ? tr("Reviews every lane")
                                                          : tr("Assigns tasks, orders the merges"),
                     row, Text::Fit::Elide);
        holds->setObjectName(QStringLiteral("teamMemberLane"));
        holds->setTone(developer && !lane.isEmpty() ? QStringLiteral("text-main")
                                                    : QStringLiteral("text-secondary"));
        QStringList paths;
        for (const auto& path : member.value(QStringLiteral("paths")).toArray())
            paths.append(path.toString());
        Text* where = nullptr;
        if (developer && !paths.isEmpty()) {
            where =
                new Text(paths.size() == 1
                             ? paths.first()
                             : QStringLiteral("%1  +%2").arg(paths.first()).arg(paths.size() - 1),
                         row, Text::Fit::ElideMiddle);
            where->setObjectName(QStringLiteral("teamMemberPaths"));
            where->setTone(QStringLiteral("text-muted"));
            where->setType(12, QFont::Medium, true);
            where->setToolTip(paths.join(QLatin1Char('\n')));
        }
        line->addLayout(pair(holds, where), compact_ ? 0 : 6);
        // The narrowest page keeps who and the actions; the lane stays one hover away.
        holds->setVisible(!compact_);
        if (where)
            where->setVisible(!compact_);
        if (compact_)
            row->setToolTip(holds->text());

        // What it does now.
        QString doing = tr("Idle");
        bool busy = false;
        for (const auto& task : team.value(QStringLiteral("tasks")).toArray())
            if (task.toObject().value(QStringLiteral("owner")).toString() == session &&
                task.toObject().value(QStringLiteral("state")).toString() ==
                    QStringLiteral("in_progress")) {
                doing = tr("On %1").arg(task.toObject().value(QStringLiteral("id")).toString());
                busy = true;
            }
        if (role == QStringLiteral("reviewer") &&
            !team.value(QStringLiteral("reviewing")).toString().isEmpty()) {
            doing = tr("Reviewing %1").arg(team.value(QStringLiteral("reviewing")).toString());
            busy = true;
        }
        auto* status = new Text(doing, row, Text::Fit::Elide);
        status->setObjectName(QStringLiteral("teamMemberStatus"));
        status->setTone(busy ? QStringLiteral("accent-text") : QStringLiteral("text-muted"));
        status->setType(13, busy ? QFont::Medium : QFont::Normal);
        status->setFixedWidth(px(140));
        // The status column goes on a narrow page; the row still says it to a screen reader.
        status->setVisible(roomy_);
        line->addWidget(status);
        row->setAccessibleName(QStringLiteral("%1, %2, %3")
                                   .arg(memberName(team, session), team_roles::label(role), doing));

        auto* open = new ket::Button(tr("Open"), ket::ButtonVariant::Ghost, row);
        open->setObjectName(QStringLiteral("teamMemberOpen"));
        open->setAccessibleName(tr("Open %1").arg(memberName(team, session)));
        connect(open, &QAbstractButton::clicked, this,
                [this, session] { emit conversationRequested(session); });
        line->addWidget(open);
        // Only a developer leaves a team; the others keep the column so Open stays aligned.
        auto* remove = new ket::Button(tr("Remove…"), ket::ButtonVariant::GhostDanger, row);
        if (developer) {
            remove->setObjectName(QStringLiteral("teamMemberRemove"));
            remove->setAccessibleName(tr("Remove %1…").arg(memberName(team, session)));
            const QString refusal = leaveRefusal(team, session);
            remove->setEnabled(refusal.isEmpty());
            remove->setToolTip(refusal);
            connect(remove, &QAbstractButton::clicked, this,
                    [this, session] { emit removeMemberRequested(teamId_, session); });
            line->addWidget(remove);
        } else {
            line->addSpacing(remove->sizeHint().width() + line->spacing());
            delete remove;
        }
        rows->addWidget(row);
    }
    layout_->addWidget(panel);
}

void TeamPage::buildUpdates() {
    QList<QJsonObject> updates;
    QSet<qint64> seen;
    for (const QJsonArray* page : {&updates_, &olderUpdates_})
        for (const auto& value : *page) {
            const QJsonObject update = value.toObject();
            if (!seen.contains(update.value(QStringLiteral("id")).toInteger())) {
                seen.insert(update.value(QStringLiteral("id")).toInteger());
                updates.append(update);
            }
        }
    std::sort(updates.begin(), updates.end(),
              [](const QJsonObject& left, const QJsonObject& right) {
                  return left.value(QStringLiteral("id")).toInteger() >
                         right.value(QStringLiteral("id")).toInteger();
              });
    addSection(tr("Updates"), {}, "teamUpdatesHeading");
    auto* panel = new team_page::Panel(column_);
    panel->setObjectName(QStringLiteral("teamUpdatesPanel"));
    auto* rows = new QVBoxLayout(panel);
    rows->setContentsMargins(1, 1, 1, 1);
    rows->setSpacing(0);
    if (updates.isEmpty()) {
        auto* nothing = new Text(tr("Nothing yet."), panel);
        nothing->setObjectName(QStringLiteral("teamUpdatesEmpty"));
        nothing->setTone(QStringLiteral("text-secondary"));
        nothing->setContentsMargins(px(18), px(16), px(18), px(16));
        rows->addWidget(nothing);
    }
    const auto found = teams_->team(teamId_);
    const QJsonObject team = found ? *found : QJsonObject{};
    for (qsizetype index = 0; index < updates.size(); ++index) {
        const QJsonObject& update = updates.at(index);
        const QString kind = update.value(QStringLiteral("kind")).toString();
        const bool warning = kind == QStringLiteral("outside_lane");
        auto* row = new team_page::Row(panel, index > 0);
        row->setObjectName(QStringLiteral("teamUpdateRow"));
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(px(18), px(11), px(18), px(11));
        line->setSpacing(px(12));
        // Not colour alone: a flagged update carries the warning icon.
        line->addWidget(glyph(warning ? QStringLiteral("warning")
                              : kind.startsWith(QStringLiteral("decision"))
                                  ? QStringLiteral("clock")
                              : kind == QStringLiteral("met") ? QStringLiteral("check")
                                                              : QStringLiteral("info"),
                              warning ? u"warning" : u"text-secondary", row),
                        0, Qt::AlignTop);
        // A report is a paragraph: three lines, and the rest on request.
        auto* text = new Text(update.value(QStringLiteral("text")).toString(), row);
        text->setObjectName(QStringLiteral("teamUpdate"));
        const QString actor = update.value(QStringLiteral("actor")).toString();
        QStringList from{actor == QStringLiteral("daemon") ? tr("KetPlus")
                         : actor == QStringLiteral("user") ? tr("You")
                                                           : memberName(team, actor)};
        if (const QString task = update.value(QStringLiteral("task_id")).toString();
            !task.isEmpty())
            from.append(task);
        auto* detail = new Text(from.join(QStringLiteral(" · ")), row, Text::Fit::Elide);
        detail->setTone(QStringLiteral("text-muted"));
        detail->setType(12);
        auto* more = new ket::Button(tr("Show more"), ket::ButtonVariant::Ghost, row);
        more->setObjectName(QStringLiteral("teamUpdateMore"));
        more->setControlSize(ket::ControlSize::Small);
        more->setVisible(false);
        connect(text, &Text::truncatedChanged, more, [more, text](const bool cut) {
            // Stays while expanded, to fold the text again.
            more->setVisible(cut || more->property("expanded").toBool());
        });
        connect(more, &QAbstractButton::clicked, text, [more, text] {
            const bool expand = !more->property("expanded").toBool();
            more->setProperty("expanded", expand);
            more->setText(expand ? tr("Show less") : tr("Show more"));
            text->setMaximumLines(expand ? 0 : kUpdateLines);
        });
        // After the connection, so the button follows the first measure too.
        text->setMaximumLines(kUpdateLines);
        auto* under = new QHBoxLayout;
        under->setSpacing(px(10));
        under->addWidget(detail);
        under->addWidget(more);
        under->addStretch(1);
        auto* body = new QVBoxLayout;
        body->setSpacing(px(4));
        body->addWidget(text);
        body->addLayout(under);
        line->addLayout(body, 1);
        auto* at =
            new Text(when(update.value(QStringLiteral("at")).toString()), row, Text::Fit::Elide);
        at->setTone(QStringLiteral("text-muted"));
        at->setType(12);
        line->addWidget(at, 0, Qt::AlignTop);
        rows->addWidget(row);
    }
    // ADR 0115: older Updates come a page at a time.
    if (olderUpdates_.isEmpty() ? newestHaveMore_ : olderHaveMore_) {
        auto* older = new ket::Button(tr("Show older"), ket::ButtonVariant::Ghost, panel);
        older->setObjectName(QStringLiteral("teamUpdatesOlder"));
        older->setControlSize(ket::ControlSize::Small);
        auto* line = new QHBoxLayout;
        line->setContentsMargins(px(18), px(8), px(18), px(10));
        line->addWidget(older);
        line->addStretch(1);
        rows->addLayout(line);
        connect(older, &QAbstractButton::clicked, this, [this] { readOlderUpdates(); });
    }
    layout_->addWidget(panel);
}

} // namespace ketplus
