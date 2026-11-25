// ADR 0114 §11: team_member_interrupt. The caller is the leader; teamOperation checked the role.
#include "agents/AgentBroker.h"
#include "runtime/SessionHost.h"

namespace ketplus::daemon {
namespace {

QJsonObject refusal(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

} // namespace

QJsonObject SessionHost::teamMemberInterrupt(const QString& leaderSession,
                                             const QJsonObject& payload) {
    const teams::Team* team = teams_->teamOf(leaderSession);
    if (!team)
        return refusal(QStringLiteral("not_on_team"),
                       QStringLiteral("This conversation is on no team."));
    const qint64 teamId = team->id;
    const QString wanted = payload.value(QStringLiteral("member")).toString().trimmed();
    const QString text = payload.value(QStringLiteral("text")).toString().trimmed();
    if (wanted.isEmpty() || text.isEmpty())
        return refusal(QStringLiteral("invalid_payload"),
                       QStringLiteral("Give the member and the text of the message."));
    // By number, else by name.
    QString memberSession = teamMemberByNumber(*team, wanted);
    if (memberSession.isEmpty())
        for (const teams::Member& member : team->members)
            if (member.name.compare(wanted, Qt::CaseInsensitive) == 0)
                memberSession = member.sessionId;
    if (memberSession.isEmpty())
        return refusal(QStringLiteral("not_member"),
                       QStringLiteral("%1 is not on the team.").arg(wanted));
    // Membership, leader, pause, reason and the hourly limit, with the audit record.
    const auto noted = teams_->noteLeaderInterrupt(
        teamId, memberSession, payload.value(QStringLiteral("reason")).toString(), leaderSession);
    if (!noted.ok())
        return refusal(noted.error->code, noted.error->message);
    const auto it = sessions_.constFind(memberSession);
    if (it == sessions_.cend())
        return refusal(QStringLiteral("not_member"),
                       QStringLiteral("The member's conversation no longer exists."));
    const qint64 generation = it->stored.generation;
    const bool running = it->startingTurn || !it->stored.turnId.isEmpty() ||
                         it->stored.state == QStringLiteral("running");
    const QString reason = payload.value(QStringLiteral("reason")).toString().trimmed();
    QStringList lines{
        QStringLiteral("Priority message from the leader (interrupt: %1).").arg(reason)};
    if (running)
        lines << QStringLiteral("The leader stopped your previous turn to send it. Your worktree "
                                "and your task state are unchanged; uncommitted files are still "
                                "there. Read the message, then continue from where it says.");
    lines << QString{} << text;
    // Stored first, so the next batch has it ahead of what is queued.
    QString messageId;
    const QString error =
        agents_->postTeamMessage(leaderSession, QString{}, memberSession, QStringLiteral("notice"),
                                 lines.join(QLatin1Char('\n')), false, &messageId, {}, true);
    if (!error.isEmpty())
        return refusal(QStringLiteral("delivery_failed"), error);
    if (running)
        (void)interrupt({{QStringLiteral("session_id"), memberSession},
                         {QStringLiteral("session_generation"), generation},
                         {QStringLiteral("stopped_by"), QStringLiteral("leader")}});
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("interrupted"), running},
            {QStringLiteral("message_id"), messageId},
            {QStringLiteral("delivery"), QStringLiteral("starting")}};
}

} // namespace ketplus::daemon
