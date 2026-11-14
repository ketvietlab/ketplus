#include "agents/AgentSchedule.h"
#include "storage/LocalJournal.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

namespace {

ScheduleRecord userSchedule(const QString& id) {
    ScheduleRecord record;
    record.id = id;
    record.name = QStringLiteral("User");
    record.enabled = true;
    record.prompt = QStringLiteral("Say hello");
    record.target.kind = QStringLiteral("conversation");
    record.target.sessionId = QStringLiteral("sess-1");
    record.ruleJson =
        QStringLiteral("{\"kind\":\"once\",\"time_zone\":\"UTC\",\"at\":\"2026-11-14T09:00\"}");
    record.busyPolicy = QStringLiteral("queue");
    record.missedPolicy = QStringLiteral("run_once");
    record.createdBy = QStringLiteral("user");
    record.createdAt = QStringLiteral("2026-11-14T08:00:00.000Z");
    record.updatedAt = record.createdAt;
    record.nextRunAt = QStringLiteral("2026-11-14T09:00:00.000Z");
    return record;
}

QDateTime fixedNow() {
    return QDateTime::fromString(QStringLiteral("2026-11-14T08:00:00Z"), Qt::ISODate);
}

} // namespace

class AgentScheduleTest final : public QObject {
    Q_OBJECT

  private slots:
    void createsListsAndCancelsOnlyItsOwn();
    void rejectsTheLeadTheLimitAndExtraFields();
    void archiveDisablesWithoutDeletingTheTarget();
};

void AgentScheduleTest::createsListsAndCancelsOnlyItsOwn() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite")));
    QVERIFY(journal.isOpen());
    QString error;
    QVERIFY(journal.createSchedule(userSchedule(QStringLiteral("user-1")), &error));

    const QJsonObject created = agent_schedule::createAgentSchedule(
        journal, QStringLiteral("sess-1"),
        {{QStringLiteral("prompt"), QStringLiteral("Check the build")},
         {QStringLiteral("inMinutes"), 60}},
        fixedNow());
    QVERIFY2(created.value(QStringLiteral("ok")).toBool(),
             qPrintable(created.value(QStringLiteral("error_message")).toString()));
    const QString id =
        created.value(QStringLiteral("schedule")).toObject().value(QStringLiteral("id")).toString();
    const auto stored = journal.schedule(id);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->createdBy, QStringLiteral("sess-1"));
    QCOMPARE(stored->target.kind, QStringLiteral("conversation"));
    QCOMPARE(stored->target.sessionId, QStringLiteral("sess-1"));
    QCOMPARE(stored->busyPolicy, QStringLiteral("queue"));
    QVERIFY(stored->ruleJson.contains(QStringLiteral("\"kind\":\"once\"")));

    const QJsonObject listed =
        agent_schedule::listAgentSchedules(journal, QStringLiteral("sess-1"));
    QCOMPARE(listed.value(QStringLiteral("schedules")).toArray().size(), 1);
    const QJsonObject hidden = agent_schedule::cancelAgentSchedule(journal, QStringLiteral("other"),
                                                                   {{QStringLiteral("id"), id}});
    QCOMPARE(hidden.value(QStringLiteral("error_code")).toString(), QStringLiteral("not_owner"));
    const QJsonObject missing = agent_schedule::cancelAgentSchedule(
        journal, QStringLiteral("sess-1"), {{QStringLiteral("id"), QStringLiteral("missing")}});
    QCOMPARE(missing.value(QStringLiteral("error_code")).toString(),
             QStringLiteral("schedule_not_found"));
    QVERIFY(agent_schedule::cancelAgentSchedule(journal, QStringLiteral("sess-1"),
                                                {{QStringLiteral("id"), id}})
                .value(QStringLiteral("ok"))
                .toBool());
    QVERIFY(!journal.schedule(id).has_value());
}

void AgentScheduleTest::rejectsTheLeadTheLimitAndExtraFields() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite")));
    QVERIFY(journal.isOpen());
    const auto code = [&](const QJsonObject& payload) {
        return agent_schedule::createAgentSchedule(journal, QStringLiteral("sess-1"), payload,
                                                   fixedNow())
            .value(QStringLiteral("error_code"))
            .toString();
    };
    QCOMPARE(code({{QStringLiteral("prompt"), QStringLiteral("Soon")},
                   {QStringLiteral("inMinutes"), 0}}),
             QStringLiteral("too_soon"));
    QCOMPARE(code({{QStringLiteral("prompt"), QStringLiteral("Later")},
                   {QStringLiteral("at"), QStringLiteral("2026-11-22T08:00:00Z")}}),
             QStringLiteral("too_far"));
    QCOMPARE(code({{QStringLiteral("prompt"), QStringLiteral("Both")},
                   {QStringLiteral("at"), QStringLiteral("2026-11-14T10:00:00Z")},
                   {QStringLiteral("inMinutes"), 60}}),
             QStringLiteral("invalid_payload"));
    QCOMPARE(code({{QStringLiteral("prompt"), QStringLiteral("Rule")},
                   {QStringLiteral("inMinutes"), 60},
                   {QStringLiteral("rule"), QStringLiteral("daily")}}),
             QStringLiteral("once_only"));
    for (int index = 0; index < 5; ++index) {
        const QJsonObject created = agent_schedule::createAgentSchedule(
            journal, QStringLiteral("sess-1"),
            {{QStringLiteral("prompt"), QStringLiteral("Keep")}, {QStringLiteral("inMinutes"), 90}},
            fixedNow());
        QVERIFY(created.value(QStringLiteral("ok")).toBool());
    }
    QCOMPARE(code({{QStringLiteral("prompt"), QStringLiteral("Sixth")},
                   {QStringLiteral("inMinutes"), 90}}),
             QStringLiteral("schedule_limit"));
}

void AgentScheduleTest::archiveDisablesWithoutDeletingTheTarget() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite")));
    QVERIFY(journal.isOpen());
    const QJsonObject created = agent_schedule::createAgentSchedule(
        journal, QStringLiteral("sess-1"),
        {{QStringLiteral("prompt"), QStringLiteral("Check the build")},
         {QStringLiteral("inMinutes"), 60}},
        fixedNow());
    const QString id =
        created.value(QStringLiteral("schedule")).toObject().value(QStringLiteral("id")).toString();
    const QStringList disabled =
        agent_schedule::disableAgentSchedules(journal, QStringLiteral("sess-1"));
    QCOMPARE(disabled, QStringList{id});
    const auto stored = journal.schedule(id);
    QVERIFY(stored.has_value());
    QVERIFY(!stored->enabled);
    QVERIFY(stored->status.isEmpty());
}

QTEST_MAIN(AgentScheduleTest)
#include "AgentScheduleTest.moc"
