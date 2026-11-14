#include "protocol/DaemonProtocol.h"
#include "protocol/ScheduleProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "FakeCodex.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
QJsonObject command(SessionHost& host, const QString& id, const QString& type,
                    const QJsonObject& payload) {
    return host.handleCommand(
        {{QStringLiteral("protocol_version"), protocol::currentVersion},
         {QStringLiteral("message_id"), id},
         {QStringLiteral("message_type"), type},
         {QStringLiteral("sent_at"), QStringLiteral("2026-11-13T09:00:00.000Z")},
         {QStringLiteral("payload"), payload}},
        QStringLiteral("desktop-test"));
}

bool accepted(const QJsonObject& result, const QString& type, QString* why) {
    if (!result.value(QStringLiteral("ok")).toBool()) {
        *why = result.value(QStringLiteral("error_code")).toString() + QLatin1Char(' ') +
               result.value(QStringLiteral("error_message")).toString();
        return false;
    }
    const auto validation = protocol::validateScheduleResult(type, result);
    if (!validation.ok)
        *why = validation.code + QLatin1Char(' ') + validation.message;
    return validation.ok;
}

QJsonObject conversationTarget(const QString& sessionId) {
    return {{QStringLiteral("kind"), QStringLiteral("conversation")},
            {QStringLiteral("session_id"), sessionId}};
}

bool seedSession(LocalJournal& journal, const QString& id) {
    StoredSession session;
    session.id = id;
    session.state = QStringLiteral("ready");
    session.adapter = QStringLiteral("codex");
    session.worktreePath = journal.databasePath();
    return journal.upsertSession(session);
}

QJsonObject createPayload(const QString& name, const QJsonObject& target, const QString& prompt,
                          const QString& rule, const QString& missed) {
    QJsonObject payload{{QStringLiteral("name"), name},
                        {QStringLiteral("enabled"), true},
                        {QStringLiteral("target"), target},
                        {QStringLiteral("rule"), rule},
                        {QStringLiteral("busy_policy"), QStringLiteral("queue")},
                        {QStringLiteral("missed_policy"), missed}};
    if (!prompt.isNull())
        payload.insert(QStringLiteral("prompt"), prompt);
    return payload;
}

const QString dailyRule = QStringLiteral(R"({"kind":"daily","time_zone":"UTC","time":"08:00"})");

bool userMessageCarries(LocalJournal& journal, const QString& sessionId, const QString& scheduleId,
                        const QString& runId) {
    for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("event_type")).toString() != QLatin1String("user.message"))
            continue;
        const QJsonObject payload = event.value(QStringLiteral("payload")).toObject();
        if (payload.value(QStringLiteral("schedule_id")).toString() == scheduleId &&
            payload.value(QStringLiteral("run_id")).toString() == runId)
            return true;
    }
    return false;
}

std::optional<StoredSession> findSession(const LocalJournal& journal, const QString& id) {
    for (const StoredSession& session : journal.sessions()) {
        if (session.id == id)
            return session;
    }
    return std::nullopt;
}

QString newestRunStatus(SessionHost& host, const QString& scheduleId) {
    const auto page = command(host, QStringLiteral("status-") + scheduleId,
                              QString::fromLatin1(protocol::scheduleRunsType),
                              {{QStringLiteral("id"), scheduleId}});
    const QJsonArray runs = page.value(QStringLiteral("runs")).toArray();
    return runs.isEmpty() ? QString()
                          : runs.at(0).toObject().value(QStringLiteral("status")).toString();
}
} // namespace

class ScheduleHostTest final : public QObject {
    Q_OBJECT
  private slots:
    void conversationRoundTripAndTargetDeleted() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY(journal.errorString().isEmpty());
        QVERIFY(seedSession(journal, QStringLiteral("session-a")));
        QVERIFY(seedSession(journal, QStringLiteral("session-b")));
        CodexAdapter adapter(dir.filePath(QStringLiteral("never-start-provider")));
        SessionHost host(&journal, &adapter);
        QSignalSpy events(&host, &SessionHost::scheduleEvent);
        QString why;
        const auto created = command(
            host, QStringLiteral("create-1"), QString::fromLatin1(protocol::scheduleCreateType),
            createPayload(QStringLiteral("Nightly"),
                          conversationTarget(QStringLiteral("session-a")),
                          QStringLiteral("Summarise the inbox"),
                          QStringLiteral("{\"kind\":\"daily\"}"), QStringLiteral("skip")));
        QVERIFY2(accepted(created, QString::fromLatin1(protocol::scheduleCreateType), &why),
                 qPrintable(why));
        const auto schedule = created.value(QStringLiteral("schedule")).toObject();
        const QString id = schedule.value(QStringLiteral("id")).toString();
        QVERIFY(!id.isEmpty());
        QCOMPARE(schedule.value(QStringLiteral("created_by")).toString(), QStringLiteral("user"));
        QCOMPARE(schedule.value(QStringLiteral("next_run_at")).toString(), QString{});
        QCOMPARE(events.count(), 2);
        QCOMPARE(events.at(0).at(0).toString(),
                 QString::fromLatin1(protocol::scheduleChangedEvent));
        const auto changed = events.at(0).at(1).toJsonObject();
        QVERIFY(protocol::validateScheduleEvent(QString::fromLatin1(protocol::scheduleChangedEvent),
                                                changed)
                    .ok);
        QCOMPARE(changed.value(QStringLiteral("change")).toString(),
                 QString::fromLatin1(protocol::scheduleCreatedChange));

        const auto listed =
            command(host, QStringLiteral("list-1"), QString::fromLatin1(protocol::scheduleListType),
                    {{QStringLiteral("target_kind"), QStringLiteral("conversation")},
                     {QStringLiteral("session_id"), QStringLiteral("session-a")}});
        QVERIFY2(accepted(listed, QString::fromLatin1(protocol::scheduleListType), &why),
                 qPrintable(why));
        QCOMPARE(listed.value(QStringLiteral("schedules")).toArray().size(), 1);

        const auto updated = command(
            host, QStringLiteral("update-1"), QString::fromLatin1(protocol::scheduleUpdateType),
            {{QStringLiteral("id"), id}, {QStringLiteral("name"), QStringLiteral("Morning")}});
        QVERIFY2(accepted(updated, QString::fromLatin1(protocol::scheduleUpdateType), &why),
                 qPrintable(why));
        QCOMPARE(updated.value(QStringLiteral("schedule")).toObject().value(QStringLiteral("name")),
                 QStringLiteral("Morning"));

        QVERIFY(journal.disableSchedulesForSession(QStringLiteral("session-a")));
        const auto refused = command(
            host, QStringLiteral("enable-1"), QString::fromLatin1(protocol::scheduleSetEnabledType),
            {{QStringLiteral("id"), id}, {QStringLiteral("enabled"), true}});
        QVERIFY(!refused.value(QStringLiteral("ok")).toBool());
        QCOMPARE(refused.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("target_deleted"));
        QCOMPARE(events.count(), 3);

        const auto renamed = command(
            host, QStringLiteral("update-2"), QString::fromLatin1(protocol::scheduleUpdateType),
            {{QStringLiteral("id"), id}, {QStringLiteral("name"), QStringLiteral("Still gone")}});
        QVERIFY2(accepted(renamed, QString::fromLatin1(protocol::scheduleUpdateType), &why),
                 qPrintable(why));
        QCOMPARE(renamed.value(QStringLiteral("schedule"))
                     .toObject()
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("target_deleted"));
        const auto stillRefused = command(
            host, QStringLiteral("enable-2"), QString::fromLatin1(protocol::scheduleSetEnabledType),
            {{QStringLiteral("id"), id}, {QStringLiteral("enabled"), true}});
        QCOMPARE(stillRefused.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("target_deleted"));

        const auto moved = command(
            host, QStringLiteral("update-3"), QString::fromLatin1(protocol::scheduleUpdateType),
            {{QStringLiteral("id"), id},
             {QStringLiteral("enabled"), true},
             {QStringLiteral("target"), conversationTarget(QStringLiteral("session-b"))}});
        QVERIFY2(accepted(moved, QString::fromLatin1(protocol::scheduleUpdateType), &why),
                 qPrintable(why));
        QCOMPARE(
            moved.value(QStringLiteral("schedule")).toObject().value(QStringLiteral("enabled")),
            true);

        const auto paused = command(
            host, QStringLiteral("enable-3"), QString::fromLatin1(protocol::scheduleSetEnabledType),
            {{QStringLiteral("id"), id}, {QStringLiteral("enabled"), false}});
        QVERIFY2(accepted(paused, QString::fromLatin1(protocol::scheduleSetEnabledType), &why),
                 qPrintable(why));
        QVERIFY(!paused.value(QStringLiteral("schedule"))
                     .toObject()
                     .value(QStringLiteral("enabled"))
                     .toBool());

        const auto removed = command(host, QStringLiteral("delete-1"),
                                     QString::fromLatin1(protocol::scheduleDeleteType),
                                     {{QStringLiteral("id"), id}});
        QVERIFY2(accepted(removed, QString::fromLatin1(protocol::scheduleDeleteType), &why),
                 qPrintable(why));
        QCOMPARE(events.at(events.count() - 1).at(1).toJsonObject().value(QStringLiteral("change")),
                 QString::fromLatin1(protocol::scheduleDeletedChange));
        const auto missing =
            command(host, QStringLiteral("get-1"), QString::fromLatin1(protocol::scheduleGetType),
                    {{QStringLiteral("id"), id}});
        QCOMPARE(missing.value(QStringLiteral("error_code")).toString(),
                 QString::fromLatin1(protocol::scheduleNotFoundCode));
    }

    void kindsFiltersRunsAndRunNow() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite3")));
        QString teamError;
        const auto teamId = journal.insertTeam(
            QStringLiteral("/tmp/repo"),
            {{QStringLiteral("name"), QStringLiteral("Website")},
             {QStringLiteral("created_at"), QStringLiteral("2026-11-13T00:00:00.000Z")}},
            {}, &teamError);
        QVERIFY2(teamId.has_value(), qPrintable(teamError));
        const QString teamKey = QString::number(*teamId);
        CodexAdapter adapter(dir.filePath(QStringLiteral("never-start-provider")));
        SessionHost host(&journal, &adapter);
        QString why;
        const QJsonObject fresh{{QStringLiteral("kind"), QStringLiteral("new")},
                                {QStringLiteral("workspace"), QStringLiteral("/tmp/repo")},
                                {QStringLiteral("provider"), QString{}},
                                {QStringLiteral("model"), QString{}},
                                {QStringLiteral("effort"), QString{}},
                                {QStringLiteral("isolated_worktree"), false},
                                {QStringLiteral("title_template"), QString{}}};
        const auto created = command(
            host, QStringLiteral("new-1"), QString::fromLatin1(protocol::scheduleCreateType),
            createPayload(QStringLiteral("Fresh"), fresh, QStringLiteral("Open the inbox"),
                          QStringLiteral("{\"kind\":\"daily\"}"), QStringLiteral("run_once")));
        QVERIFY2(accepted(created, QString::fromLatin1(protocol::scheduleCreateType), &why),
                 qPrintable(why));
        const QString id = created.value(QStringLiteral("schedule"))
                               .toObject()
                               .value(QStringLiteral("id"))
                               .toString();
        const QJsonObject team{{QStringLiteral("kind"), QStringLiteral("team")},
                               {QStringLiteral("team_id"), teamKey}};
        QVERIFY2(accepted(command(host, QStringLiteral("team-1"),
                                  QString::fromLatin1(protocol::scheduleCreateType),
                                  createPayload(QStringLiteral("Standup"), team,
                                                QStringLiteral("Ask the leader"),
                                                QStringLiteral("{\"kind\":\"daily\"}"),
                                                QStringLiteral("skip"))),
                          QString::fromLatin1(protocol::scheduleCreateType), &why),
                 qPrintable(why));
        const QJsonObject start{{QStringLiteral("kind"), QStringLiteral("team_start")},
                                {QStringLiteral("team_id"), teamKey}};
        QVERIFY2(accepted(command(host, QStringLiteral("start-1"),
                                  QString::fromLatin1(protocol::scheduleCreateType),
                                  createPayload(QStringLiteral("Kickoff"), start, QString{},
                                                QStringLiteral("{\"kind\":\"once\"}"),
                                                QStringLiteral("run_once"))),
                          QString::fromLatin1(protocol::scheduleCreateType), &why),
                 qPrintable(why));

        const auto teamRows = command(host, QStringLiteral("list-team"),
                                      QString::fromLatin1(protocol::scheduleListType),
                                      {{QStringLiteral("team_id"), teamKey}});
        QVERIFY2(accepted(teamRows, QString::fromLatin1(protocol::scheduleListType), &why),
                 qPrintable(why));
        QCOMPARE(teamRows.value(QStringLiteral("schedules")).toArray().size(), 2);
        const auto onlyTeam = command(host, QStringLiteral("list-kind"),
                                      QString::fromLatin1(protocol::scheduleListType),
                                      {{QStringLiteral("target_kind"), QStringLiteral("team")}});
        QCOMPARE(onlyTeam.value(QStringLiteral("schedules")).toArray().size(), 1);

        ScheduleRunRecord older;
        older.id = QStringLiteral("run-old");
        older.scheduleId = id;
        older.scheduledFor = QStringLiteral("2026-11-13T08:00:00.000Z");
        older.status = QStringLiteral("succeeded");
        ScheduleRunRecord newer = older;
        newer.id = QStringLiteral("run-new");
        newer.scheduledFor = QStringLiteral("2026-11-13T09:00:00.000Z");
        newer.status = QStringLiteral("queued");
        QVERIFY(journal.appendScheduleRun(older));
        QVERIFY(journal.appendScheduleRun(newer));
        const auto page =
            command(host, QStringLiteral("runs-1"), QString::fromLatin1(protocol::scheduleRunsType),
                    {{QStringLiteral("id"), id}, {QStringLiteral("limit"), 1}});
        QVERIFY2(accepted(page, QString::fromLatin1(protocol::scheduleRunsType), &why),
                 qPrintable(why));
        QVERIFY(page.value(QStringLiteral("has_more")).toBool());
        QCOMPARE(page.value(QStringLiteral("runs"))
                     .toArray()
                     .at(0)
                     .toObject()
                     .value(QStringLiteral("id")),
                 QStringLiteral("run-new"));
        const auto rest =
            command(host, QStringLiteral("runs-2"), QString::fromLatin1(protocol::scheduleRunsType),
                    {{QStringLiteral("id"), id},
                     {QStringLiteral("limit"), 1},
                     {QStringLiteral("before_id"), QStringLiteral("run-new")}});
        QVERIFY2(accepted(rest, QString::fromLatin1(protocol::scheduleRunsType), &why),
                 qPrintable(why));
        QVERIFY(!rest.value(QStringLiteral("has_more")).toBool());
        QCOMPARE(rest.value(QStringLiteral("runs"))
                     .toArray()
                     .at(0)
                     .toObject()
                     .value(QStringLiteral("id")),
                 QStringLiteral("run-old"));

        const auto now = command(host, QStringLiteral("now-1"),
                                 QString::fromLatin1(protocol::scheduleRunNowType),
                                 {{QStringLiteral("id"), QStringLiteral("missing-schedule")}});
        QVERIFY(!now.value(QStringLiteral("ok")).toBool());
        QCOMPARE(now.value(QStringLiteral("error_code")).toString(),
                 QString::fromLatin1(protocol::scheduleNotFoundCode));
        QVERIFY(!journal.schedule(QStringLiteral("missing-schedule")));
        const auto fired = command(host, QStringLiteral("now-2"),
                                   QString::fromLatin1(protocol::scheduleRunNowType),
                                   {{QStringLiteral("id"), id}});
        QVERIFY2(accepted(fired, QString::fromLatin1(protocol::scheduleRunNowType), &why),
                 qPrintable(why));
        QVERIFY(!fired.value(QStringLiteral("run"))
                     .toObject()
                     .value(QStringLiteral("id"))
                     .toString()
                     .isEmpty());
    }

    void rejectsInvalidRequests() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite3")));
        CodexAdapter adapter(dir.filePath(QStringLiteral("never-start-provider")));
        SessionHost host(&journal, &adapter);
        const auto empty = command(
            host, QStringLiteral("bad-prompt"), QString::fromLatin1(protocol::scheduleCreateType),
            createPayload(QStringLiteral("Nightly"),
                          conversationTarget(QStringLiteral("session-a")), QString{},
                          QStringLiteral("{\"kind\":\"daily\"}"), QStringLiteral("skip")));
        QCOMPARE(empty.value(QStringLiteral("error_code")).toString(),
                 QString::fromLatin1(ketplus::schedule::emptyPromptCode));
        auto extra = createPayload(QStringLiteral("Nightly"),
                                   conversationTarget(QStringLiteral("session-a")),
                                   QStringLiteral("Summarise"),
                                   QStringLiteral("{\"kind\":\"daily\"}"), QStringLiteral("skip"));
        extra.insert(QStringLiteral("created_by"), QStringLiteral("user"));
        const auto unknown = command(host, QStringLiteral("bad-field"),
                                     QString::fromLatin1(protocol::scheduleCreateType), extra);
        QCOMPARE(unknown.value(QStringLiteral("error_code")).toString(),
                 QString::fromLatin1(ketplus::schedule::unknownFieldCode));
        const auto other = command(host, QStringLiteral("bad-type"), QStringLiteral("schedule.foo"),
                                   QJsonObject{});
        QCOMPARE(other.value(QStringLiteral("error_code")).toString(),
                 QString::fromLatin1(ketplus::schedule::invalidPayloadCode));
        QCOMPARE(journal.schedules(ScheduleFilter{}).value_or(QList<ScheduleRecord>{}).size(), 0);
        const auto missingSession =
            command(host, QStringLiteral("missing-session"),
                    QString::fromLatin1(protocol::scheduleCreateType),
                    createPayload(QStringLiteral("Nightly"),
                                  conversationTarget(QStringLiteral("no-such-session")),
                                  QStringLiteral("Summarise"),
                                  QStringLiteral("{\"kind\":\"daily\"}"), QStringLiteral("skip")));
        QCOMPARE(missingSession.value(QStringLiteral("error_code")).toString(),
                 QString::fromLatin1(protocol::scheduleTargetNotFoundCode));
        const auto missingTeam = command(
            host, QStringLiteral("missing-team"), QString::fromLatin1(protocol::scheduleCreateType),
            createPayload(QStringLiteral("Standup"),
                          {{QStringLiteral("kind"), QStringLiteral("team")},
                           {QStringLiteral("team_id"), QStringLiteral("website")}},
                          QStringLiteral("Ask the leader"), QStringLiteral("{\"kind\":\"daily\"}"),
                          QStringLiteral("skip")));
        QCOMPARE(missingTeam.value(QStringLiteral("error_code")).toString(),
                 QString::fromLatin1(protocol::scheduleTargetNotFoundCode));
    }

    void cascadesEmitScheduleChanged() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY(seedSession(journal, QStringLiteral("session-a")));
        QString teamError;
        const auto teamId = journal.insertTeam(
            QStringLiteral("/tmp/repo"),
            {{QStringLiteral("name"), QStringLiteral("Website")},
             {QStringLiteral("created_at"), QStringLiteral("2026-11-13T00:00:00.000Z")}},
            {}, &teamError);
        QVERIFY2(teamId.has_value(), qPrintable(teamError));
        const QString teamKey = QString::number(*teamId);
        CodexAdapter adapter(dir.filePath(QStringLiteral("never-start-provider")));
        SessionHost host(&journal, &adapter);
        QSignalSpy events(&host, &SessionHost::scheduleEvent);
        QString why;
        const auto created = command(
            host, QStringLiteral("create-1"), QString::fromLatin1(protocol::scheduleCreateType),
            createPayload(QStringLiteral("Nightly"),
                          conversationTarget(QStringLiteral("session-a")),
                          QStringLiteral("Summarise the inbox"),
                          QStringLiteral("{\"kind\":\"daily\"}"), QStringLiteral("skip")));
        QVERIFY2(accepted(created, QString::fromLatin1(protocol::scheduleCreateType), &why),
                 qPrintable(why));
        const auto removed = host.handleCommand(
            {{QStringLiteral("protocol_version"), protocol::currentVersion},
             {QStringLiteral("message_id"), QStringLiteral("delete-session")},
             {QStringLiteral("message_type"), QStringLiteral("session.delete")},
             {QStringLiteral("session_id"), QStringLiteral("session-a")},
             {QStringLiteral("session_generation"), 1},
             {QStringLiteral("sent_at"), QStringLiteral("2026-11-13T09:00:00.000Z")},
             {QStringLiteral("payload"), QJsonObject{}}},
            QStringLiteral("desktop-test"));
        QVERIFY2(removed.value(QStringLiteral("ok")).toBool(),
                 qPrintable(removed.value(QStringLiteral("error_code")).toString() +
                            QLatin1Char(' ') +
                            removed.value(QStringLiteral("error_message")).toString()));
        QCOMPARE(events.count(), 3);
        const auto disabled = events.at(2).at(1).toJsonObject();
        QCOMPARE(disabled.value(QStringLiteral("change")).toString(),
                 QString::fromLatin1(protocol::scheduleUpdatedChange));
        const auto disabledSchedule = disabled.value(QStringLiteral("schedule")).toObject();
        QCOMPARE(disabledSchedule.value(QStringLiteral("status")).toString(),
                 QStringLiteral("target_deleted"));
        QVERIFY(!disabledSchedule.value(QStringLiteral("enabled")).toBool());

        QVERIFY2(accepted(command(host, QStringLiteral("team-1"),
                                  QString::fromLatin1(protocol::scheduleCreateType),
                                  createPayload(QStringLiteral("Standup"),
                                                {{QStringLiteral("kind"), QStringLiteral("team")},
                                                 {QStringLiteral("team_id"), teamKey}},
                                                QStringLiteral("Ask the leader"),
                                                QStringLiteral("{\"kind\":\"daily\"}"),
                                                QStringLiteral("skip"))),
                          QString::fromLatin1(protocol::scheduleCreateType), &why),
                 qPrintable(why));
        const auto teamGone =
            command(host, QStringLiteral("team-delete"), QStringLiteral("team.delete"),
                    {{QStringLiteral("team_id"), teamKey}});
        QVERIFY2(teamGone.value(QStringLiteral("ok")).toBool(),
                 qPrintable(teamGone.value(QStringLiteral("error_message")).toString()));
        QCOMPARE(events.count(), 6);
        const auto deleted = events.at(5).at(1).toJsonObject();
        QCOMPARE(deleted.value(QStringLiteral("change")).toString(),
                 QString::fromLatin1(protocol::scheduleDeletedChange));
        QCOMPARE(journal.schedules(ScheduleFilter{.teamId = teamKey})
                     .value_or(QList<ScheduleRecord>{})
                     .size(),
                 0);
    }

    void scheduledTurnFollowsTheHost() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFakeCodex(dir.filePath(QStringLiteral("fake-codex")),
                               dir.filePath(QStringLiteral("codex.log"))));
        LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY(journal.errorString().isEmpty());
        CodexAdapter adapter(dir.filePath(QStringLiteral("fake-codex")));
        const auto stop = qScopeGuard([&adapter] { adapter.stop(); });
        SessionHost host(&journal, &adapter);
        QString why;

        const QJsonObject fresh{{QStringLiteral("kind"), QStringLiteral("new")},
                                {QStringLiteral("workspace"), dir.path()},
                                {QStringLiteral("provider"), QStringLiteral("codex")},
                                {QStringLiteral("model"), QString{}},
                                {QStringLiteral("effort"), QString{}},
                                {QStringLiteral("isolated_worktree"), false},
                                {QStringLiteral("title_template"), QString{}}};
        const auto created = command(
            host, QStringLiteral("new-schedule"), QString::fromLatin1(protocol::scheduleCreateType),
            createPayload(QStringLiteral("Fresh"), fresh, QStringLiteral("Say hello"), dailyRule,
                          QStringLiteral("skip")));
        QVERIFY2(accepted(created, QString::fromLatin1(protocol::scheduleCreateType), &why),
                 qPrintable(why));
        const QString freshId = created.value(QStringLiteral("schedule"))
                                    .toObject()
                                    .value(QStringLiteral("id"))
                                    .toString();
        const QString nextRunAt = journal.schedule(freshId)->nextRunAt;
        QVERIFY(!nextRunAt.isEmpty());
        const auto fired = command(host, QStringLiteral("new-now"),
                                   QString::fromLatin1(protocol::scheduleRunNowType),
                                   {{QStringLiteral("id"), freshId}});
        QVERIFY2(accepted(fired, QString::fromLatin1(protocol::scheduleRunNowType), &why),
                 qPrintable(why));
        const QJsonObject freshRun = fired.value(QStringLiteral("run")).toObject();
        const QString freshRunId = freshRun.value(QStringLiteral("id")).toString();
        const QString freshSession = freshRun.value(QStringLiteral("session_id")).toString();
        QVERIFY(!freshRunId.isEmpty());
        QVERIFY(!freshSession.isEmpty());
        QCOMPARE(journal.sessions().size(), 1);
        QTRY_VERIFY(userMessageCarries(journal, freshSession, freshId, freshRunId));
        QTRY_COMPARE(newestRunStatus(host, freshId), QStringLiteral("succeeded"));
        QCOMPARE(journal.schedule(freshId)->nextRunAt, nextRunAt);

        const auto hung = host.handleCommand(
            {{QStringLiteral("protocol_version"), protocol::currentVersion},
             {QStringLiteral("message_id"), QStringLiteral("hang-turn")},
             {QStringLiteral("message_type"), QStringLiteral("session.start_turn")},
             {QStringLiteral("sent_at"), QStringLiteral("2026-11-13T09:00:00.000Z")},
             {QStringLiteral("payload"),
              QJsonObject{{QStringLiteral("worktree_path"), dir.path()},
                          {QStringLiteral("prompt"), QStringLiteral("hang")}}}},
            QStringLiteral("desktop-test"));
        QVERIFY2(hung.value(QStringLiteral("ok")).toBool(),
                 qPrintable(hung.value(QStringLiteral("error_message")).toString()));
        const QString busySession = hung.value(QStringLiteral("session_id")).toString();
        QTRY_COMPARE(findSession(journal, busySession)->state, QStringLiteral("running"));

        const auto queuedSchedule =
            command(host, QStringLiteral("queue-schedule"),
                    QString::fromLatin1(protocol::scheduleCreateType),
                    createPayload(QStringLiteral("Later"), conversationTarget(busySession),
                                  QStringLiteral("Say hello"), dailyRule, QStringLiteral("skip")));
        QVERIFY2(accepted(queuedSchedule, QString::fromLatin1(protocol::scheduleCreateType), &why),
                 qPrintable(why));
        const QString queuedId = queuedSchedule.value(QStringLiteral("schedule"))
                                     .toObject()
                                     .value(QStringLiteral("id"))
                                     .toString();
        const auto queuedNow = command(host, QStringLiteral("queue-now"),
                                       QString::fromLatin1(protocol::scheduleRunNowType),
                                       {{QStringLiteral("id"), queuedId}});
        QVERIFY2(accepted(queuedNow, QString::fromLatin1(protocol::scheduleRunNowType), &why),
                 qPrintable(why));
        QCOMPARE(queuedNow.value(QStringLiteral("run")).toObject().value(QStringLiteral("status")),
                 QStringLiteral("queued"));
        const QString queuedRunId = queuedNow.value(QStringLiteral("run"))
                                        .toObject()
                                        .value(QStringLiteral("id"))
                                        .toString();
        const auto busy = findSession(journal, busySession);
        QVERIFY(busy);
        emit adapter.notificationReceived(
            QStringLiteral("turn/completed"),
            {{QStringLiteral("threadId"), busy->threadId},
             {QStringLiteral("turn"),
              QJsonObject{{QStringLiteral("id"), busy->turnId},
                          {QStringLiteral("status"), QStringLiteral("completed")}}}});
        QTRY_COMPARE(newestRunStatus(host, queuedId), QStringLiteral("succeeded"));
        QTRY_VERIFY(userMessageCarries(journal, busySession, queuedId, queuedRunId));

        const auto stoppedSchedule =
            command(host, QStringLiteral("stop-schedule"),
                    QString::fromLatin1(protocol::scheduleCreateType),
                    createPayload(QStringLiteral("Stop"), conversationTarget(freshSession),
                                  QStringLiteral("hang"), dailyRule, QStringLiteral("skip")));
        QVERIFY2(accepted(stoppedSchedule, QString::fromLatin1(protocol::scheduleCreateType), &why),
                 qPrintable(why));
        const QString stoppedId = stoppedSchedule.value(QStringLiteral("schedule"))
                                      .toObject()
                                      .value(QStringLiteral("id"))
                                      .toString();
        const auto stoppedNow = command(host, QStringLiteral("stop-now"),
                                        QString::fromLatin1(protocol::scheduleRunNowType),
                                        {{QStringLiteral("id"), stoppedId}});
        QVERIFY2(accepted(stoppedNow, QString::fromLatin1(protocol::scheduleRunNowType), &why),
                 qPrintable(why));
        const QString stoppedRunId = stoppedNow.value(QStringLiteral("run"))
                                         .toObject()
                                         .value(QStringLiteral("id"))
                                         .toString();
        QTRY_COMPARE(findSession(journal, freshSession)->state, QStringLiteral("running"));
        emit adapter.stopped(QStringLiteral("provider exited"));
        QTRY_COMPARE(newestRunStatus(host, stoppedId), QStringLiteral("failed"));
        const auto failedPage = command(host, QStringLiteral("stop-runs"),
                                        QString::fromLatin1(protocol::scheduleRunsType),
                                        {{QStringLiteral("id"), stoppedId}});
        const QJsonObject failedRun =
            failedPage.value(QStringLiteral("runs")).toArray().at(0).toObject();
        QCOMPARE(failedRun.value(QStringLiteral("id")).toString(), stoppedRunId);
        QCOMPARE(failedRun.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("runtime_stopped"));
    }

    void restartFailsStaleRuns() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY(journal.errorString().isEmpty());
        StoredSession ready;
        ready.id = QStringLiteral("ready");
        ready.state = QStringLiteral("ready");
        ready.adapter = QStringLiteral("codex");
        ready.worktreePath = dir.path();
        StoredSession dead;
        dead.id = QStringLiteral("dead");
        dead.state = QStringLiteral("running");
        dead.turnId = QStringLiteral("turn-old");
        dead.adapter = QStringLiteral("codex");
        dead.worktreePath = dir.path();
        QVERIFY(journal.upsertSession(ready));
        QVERIFY(journal.upsertSession(dead));

        const auto make = [](const QString& id, const QString& sessionId) {
            ScheduleRecord schedule;
            schedule.id = id;
            schedule.name = id;
            schedule.enabled = true;
            schedule.prompt = QStringLiteral("Say hello");
            schedule.target.kind = QStringLiteral("conversation");
            schedule.target.sessionId = sessionId;
            schedule.ruleJson = QStringLiteral(R"({"kind":"daily"})");
            schedule.busyPolicy = QStringLiteral("queue");
            schedule.missedPolicy = QStringLiteral("skip");
            schedule.createdBy = QStringLiteral("user");
            schedule.createdAt = QStringLiteral("2026-11-13T00:00:00.000Z");
            schedule.updatedAt = schedule.createdAt;
            schedule.nextRunAt = QStringLiteral("2099-01-01T00:00:00.000Z");
            return schedule;
        };
        QVERIFY(journal.createSchedule(
            make(QStringLiteral("running-schedule"), QStringLiteral("ready"))));
        QVERIFY(
            journal.createSchedule(make(QStringLiteral("dead-schedule"), QStringLiteral("dead"))));
        QVERIFY(
            journal.createSchedule(make(QStringLiteral("live-schedule"), QStringLiteral("ready"))));
        QVERIFY(
            journal.createSchedule(make(QStringLiteral("done-schedule"), QStringLiteral("ready"))));
        const auto run = [](const QString& id, const QString& scheduleId, const QString& sessionId,
                            const QString& status) {
            ScheduleRunRecord record;
            record.id = id;
            record.scheduleId = scheduleId;
            record.sessionId = sessionId;
            record.scheduledFor = QStringLiteral("2026-11-13T08:00:00.000Z");
            record.status = status;
            if (status == QLatin1String("running") || status == QLatin1String("succeeded"))
                record.startedAt = QStringLiteral("2026-11-13T08:00:01.000Z");
            if (status == QLatin1String("succeeded"))
                record.finishedAt = QStringLiteral("2026-11-13T08:05:00.000Z");
            return record;
        };
        QVERIFY(journal.appendScheduleRun(run(QStringLiteral("run-running"),
                                              QStringLiteral("running-schedule"),
                                              QStringLiteral("ready"), QStringLiteral("running"))));
        QVERIFY(journal.appendScheduleRun(run(QStringLiteral("run-dead"),
                                              QStringLiteral("dead-schedule"),
                                              QStringLiteral("dead"), QStringLiteral("queued"))));
        QVERIFY(journal.appendScheduleRun(run(QStringLiteral("run-live"),
                                              QStringLiteral("live-schedule"),
                                              QStringLiteral("ready"), QStringLiteral("queued"))));
        QVERIFY(journal.appendScheduleRun(
            run(QStringLiteral("run-done"), QStringLiteral("done-schedule"),
                QStringLiteral("ready"), QStringLiteral("succeeded"))));

        CodexAdapter adapter(dir.filePath(QStringLiteral("never-start-provider")));
        SessionHost host(&journal, &adapter);
        const auto failed = [](const std::optional<ScheduleRunRecord>& record) {
            return record && record->status == QLatin1String("failed") &&
                   record->errorCode == QLatin1String("runtime_stopped") &&
                   !record->finishedAt.isEmpty();
        };
        QVERIFY(failed(journal.scheduleRun(QStringLiteral("run-running"))));
        QVERIFY(failed(journal.scheduleRun(QStringLiteral("run-dead"))));
        QCOMPARE(journal.scheduleRun(QStringLiteral("run-done"))->status,
                 QStringLiteral("succeeded"));
        QCOMPARE(journal.scheduleRun(QStringLiteral("run-done"))->finishedAt,
                 QStringLiteral("2026-11-13T08:05:00.000Z"));
        for (const QString& id :
             {QStringLiteral("running-schedule"), QStringLiteral("dead-schedule"),
              QStringLiteral("live-schedule"), QStringLiteral("done-schedule")})
            QCOMPARE(journal.schedule(id)->nextRunAt, QStringLiteral("2099-01-01T00:00:00.000Z"));
        const auto live = journal.scheduleRun(QStringLiteral("run-live"));
        QVERIFY(live);
        QVERIFY(live->errorCode != QLatin1String("runtime_stopped"));
    }
};

QTEST_GUILESS_MAIN(ScheduleHostTest)
#include "ScheduleHostTest.moc"
