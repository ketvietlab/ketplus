// ADR 0126: schedule documents, requests and events. No provider and no journal.
#include "protocol/ScheduleProtocol.h"
#include "Schedule.h"
#include "protocol/DaemonProtocol.h"

#include <QJsonObject>
#include <QTest>

using namespace ketplus::schedule;
namespace protocol = ketplus::daemon::protocol;

namespace {

const QString onceRule =
    QStringLiteral(R"({"kind":"once","at":"2026-11-13T22:00","timezone":"Asia/Ho_Chi_Minh"})");
const QString dailyRule =
    QStringLiteral(R"({"kind":"daily","time":"08:00","timezone":"Asia/Ho_Chi_Minh"})");
const QString sessionId = QStringLiteral("11111111-1111-1111-1111-111111111111");
const QString createdAt = QStringLiteral("2026-11-13T08:00:00.000Z");

Schedule baseSchedule() {
    Schedule schedule;
    schedule.id = QStringLiteral("sched-1");
    schedule.name = QStringLiteral("Recheck CI");
    schedule.enabled = true;
    schedule.prompt = QStringLiteral("Recheck the CI workflow and report its status.");
    schedule.rule = dailyRule;
    schedule.busyPolicy = BusyPolicy::Skip;
    schedule.missedPolicy = MissedPolicy::RunOnce;
    schedule.createdBy = QStringLiteral("user");
    schedule.createdAt = createdAt;
    schedule.updatedAt = createdAt;
    schedule.nextRunAt = QStringLiteral("2026-11-14T01:00:00.000Z");
    schedule.lastRunAt = QString{};
    return schedule;
}

Schedule conversationSchedule() {
    Schedule schedule = baseSchedule();
    schedule.target.kind = TargetKind::Conversation;
    schedule.target.sessionId = sessionId;
    return schedule;
}

Schedule newSchedule() {
    Schedule schedule = baseSchedule();
    schedule.target.kind = TargetKind::NewConversation;
    schedule.target.workspace = QStringLiteral("/repo/ketplus");
    schedule.target.provider = QStringLiteral("codex");
    schedule.target.model = QStringLiteral("gpt-5");
    schedule.target.effort = QStringLiteral("medium");
    schedule.target.isolatedWorktree = true;
    schedule.target.titleTemplate = QStringLiteral("CI {date}");
    return schedule;
}

Schedule teamSchedule() {
    Schedule schedule = baseSchedule();
    schedule.name = QStringLiteral("Daily website article");
    schedule.target.kind = TargetKind::Team;
    schedule.target.teamId = QStringLiteral("release");
    schedule.busyPolicy = BusyPolicy::Queue;
    return schedule;
}

Schedule teamStartSchedule() {
    Schedule schedule = baseSchedule();
    schedule.name = QStringLiteral("Start Website");
    schedule.prompt.clear();
    schedule.rule = onceRule;
    schedule.target.kind = TargetKind::TeamStart;
    schedule.target.teamId = QStringLiteral("website");
    schedule.missedPolicy = MissedPolicy::RunOnce;
    schedule.nextRunAt = QStringLiteral("2026-11-13T15:00:00.000Z");
    schedule.lastRunAt.clear();
    return schedule;
}

Run sampleRun(const RunStatus status) {
    Run run;
    run.id = QStringLiteral("run-1");
    run.scheduleId = QStringLiteral("sched-1");
    run.scheduledFor = QStringLiteral("2026-11-14T01:00:00.000Z");
    run.startedAt = status == RunStatus::Queued ? QString{} : createdAt;
    run.finishedAt =
        status == RunStatus::Succeeded ? QStringLiteral("2026-11-14T01:05:00.000Z") : QString{};
    run.status = status;
    run.sessionId = status == RunStatus::Queued ? QString{} : sessionId;
    run.turnId = status == RunStatus::Succeeded ? QStringLiteral("turn-9") : QString{};
    run.errorCode =
        status == RunStatus::Failed ? QStringLiteral("provider_unavailable") : QString{};
    run.errorMessage =
        status == RunStatus::Failed ? QStringLiteral("Provider is not signed in") : QString{};
    return run;
}

QJsonObject createPayload(const Schedule& schedule) {
    QJsonObject payload = toJson(schedule);
    payload.remove(QStringLiteral("id"));
    payload.remove(QStringLiteral("created_by"));
    payload.remove(QStringLiteral("created_at"));
    payload.remove(QStringLiteral("updated_at"));
    payload.remove(QStringLiteral("next_run_at"));
    payload.remove(QStringLiteral("last_run_at"));
    return payload;
}

void expectRoundTrip(const Schedule& schedule) {
    Failure failure;
    const auto parsed = scheduleFromJson(toJson(schedule), &failure);
    QVERIFY2(parsed.has_value(), qPrintable(failure.message));
    QCOMPARE(toJson(*parsed), toJson(schedule));
}

} // namespace

class ScheduleProtocolTest final : public QObject {
    Q_OBJECT

  private slots:
    void featureIsAdvertised() {
        QVERIFY(protocol::features(1).contains(QString(protocol::schedulesFeature)));
        QVERIFY(!protocol::features(0).contains(QString(protocol::schedulesFeature)));
        QCOMPARE(maximumRetainedRuns, 50);
    }

    void documentsRoundTripEveryTarget() {
        expectRoundTrip(conversationSchedule());
        expectRoundTrip(newSchedule());
        expectRoundTrip(teamSchedule());
        expectRoundTrip(teamStartSchedule());
    }

    void runsRoundTripEveryStatus() {
        for (const RunStatus status : {RunStatus::Queued, RunStatus::Running, RunStatus::Succeeded,
                                       RunStatus::Failed, RunStatus::Skipped, RunStatus::Missed}) {
            Failure failure;
            const Run run = sampleRun(status);
            const auto parsed = runFromJson(toJson(run), &failure);
            QVERIFY2(parsed.has_value(), qPrintable(failure.message));
            QCOMPARE(toJson(*parsed), toJson(run));
            QCOMPARE(parsed->status, status);
        }
    }

    void rejectsUnknownFieldsEmptyPromptMissingTargetAndBadEnums() {
        Failure failure;
        QJsonObject document = toJson(conversationSchedule());
        document.insert(QStringLiteral("note"), QStringLiteral("nope"));
        QVERIFY(!scheduleFromJson(document, &failure));
        QCOMPARE(failure.code, QString(unknownFieldCode));

        document = toJson(conversationSchedule());
        document.insert(QStringLiteral("created_at"), QStringLiteral("2026-11-13T10:00:00"));
        QVERIFY(!scheduleFromJson(document, &failure));
        QCOMPARE(failure.code, QString(invalidPayloadCode));

        document = toJson(conversationSchedule());
        document.insert(QStringLiteral("prompt"), QString{});
        QVERIFY(!scheduleFromJson(document, &failure));
        QCOMPARE(failure.code, QString(emptyPromptCode));

        document = toJson(conversationSchedule());
        document.remove(QStringLiteral("target"));
        QVERIFY(!scheduleFromJson(document, &failure));
        QCOMPARE(failure.code, QString(missingTargetCode));

        document = toJson(conversationSchedule());
        document.insert(QStringLiteral("busy_policy"), QStringLiteral("drop"));
        QVERIFY(!scheduleFromJson(document, &failure));
        QCOMPARE(failure.code, QString(invalidEnumCode));

        document = toJson(conversationSchedule());
        document.insert(QStringLiteral("missed_policy"), QStringLiteral("retry"));
        QVERIFY(!scheduleFromJson(document, &failure));
        QCOMPARE(failure.code, QString(invalidEnumCode));

        QJsonObject target = document.value(QStringLiteral("target")).toObject();
        target.insert(QStringLiteral("kind"), QStringLiteral("channel"));
        document.insert(QStringLiteral("target"), target);
        document.insert(QStringLiteral("missed_policy"), QStringLiteral("skip"));
        QVERIFY(!scheduleFromJson(document, &failure));
        QCOMPARE(failure.code, QString(invalidEnumCode));

        target = toJson(newSchedule()).value(QStringLiteral("target")).toObject();
        target.insert(QStringLiteral("extra"), true);
        QVERIFY(!targetFromJson(target, &failure));
        QCOMPARE(failure.code, QString(unknownFieldCode));

        QJsonObject run = toJson(sampleRun(RunStatus::Failed));
        run.insert(QStringLiteral("status"), QStringLiteral("cancelled"));
        QVERIFY(!runFromJson(run, &failure));
        QCOMPARE(failure.code, QString(invalidEnumCode));

        document = toJson(conversationSchedule());
        document = toJson(conversationSchedule());
        document.insert(QStringLiteral("rule"), QStringLiteral("daily at 8"));
        QVERIFY(!scheduleFromJson(document, &failure));
        QCOMPARE(failure.code, QString(invalidRuleCode));

        document.insert(QStringLiteral("rule"), QStringLiteral("[1]"));
        QVERIFY(!scheduleFromJson(document, &failure));
        QCOMPARE(failure.code, QString(invalidRuleCode));
    }

    void teamStartConstraints() {
        Failure failure;
        Schedule schedule = teamStartSchedule();
        schedule.prompt = QStringLiteral("Ask the leader to plan the work.");
        QVERIFY(!scheduleFromJson(toJson(schedule), &failure));
        QCOMPARE(failure.code, QString(promptNotAllowedCode));

        schedule = teamStartSchedule();
        schedule.rule = dailyRule;
        QVERIFY(!scheduleFromJson(toJson(schedule), &failure));
        QCOMPARE(failure.code, QString(invalidRuleCode));

        schedule = teamStartSchedule();
        schedule.missedPolicy = MissedPolicy::Skip;
        QVERIFY(!scheduleFromJson(toJson(schedule), &failure));
        QCOMPARE(failure.code, QString(teamStartConstraintCode));

        schedule = teamStartSchedule();
        schedule.prompt = QStringLiteral("   ");
        const auto parsed = scheduleFromJson(toJson(schedule), &failure);
        QVERIFY2(parsed.has_value(), qPrintable(failure.message));
        QVERIFY(parsed->prompt.isEmpty());
    }

    void requestsRoundTripAndRejectBadPayloads() {
        const QStringList requests{
            QString(protocol::scheduleListType),   QString(protocol::scheduleGetType),
            QString(protocol::scheduleCreateType), QString(protocol::scheduleUpdateType),
            QString(protocol::scheduleDeleteType), QString(protocol::scheduleSetEnabledType),
            QString(protocol::scheduleRunNowType), QString(protocol::scheduleRunsType)};
        for (const QString& type : requests)
            QVERIFY(protocol::isScheduleRequest(type));

        const auto list = protocol::validateScheduleRequest(
            QString(protocol::scheduleListType),
            {{QStringLiteral("target_kind"), QStringLiteral("team")},
             {QStringLiteral("team_id"), QStringLiteral("release")}});
        QVERIFY2(list.ok, qPrintable(list.message));
        QCOMPARE(
            protocol::validateScheduleRequest(QString(protocol::scheduleListType), list.canonical)
                .canonical,
            list.canonical);
        Failure failure;
        const auto query = protocol::parseScheduleList(list.canonical, &failure);
        QVERIFY(query->targetKind.has_value());
        QCOMPARE(*query->targetKind, TargetKind::Team);
        QCOMPARE(query->teamId, QStringLiteral("release"));

        QJsonObject badList = list.canonical;
        badList.insert(QStringLiteral("target_kind"), QStringLiteral("person"));
        const auto badKind =
            protocol::validateScheduleRequest(QString(protocol::scheduleListType), badList);
        QVERIFY(!badKind.ok);
        QCOMPARE(badKind.code, QString(invalidEnumCode));

        for (const Schedule& schedule :
             {conversationSchedule(), newSchedule(), teamSchedule(), teamStartSchedule()}) {
            const auto created = protocol::validateScheduleRequest(
                QString(protocol::scheduleCreateType), createPayload(schedule));
            QVERIFY2(created.ok, qPrintable(created.message));
            QCOMPARE(protocol::validateScheduleRequest(QString(protocol::scheduleCreateType),
                                                       created.canonical)
                         .canonical,
                     created.canonical);
            const auto parsed = protocol::parseScheduleCreate(created.canonical, &failure);
            QVERIFY(parsed.has_value());
            QCOMPARE(parsed->target.kind, schedule.target.kind);
            QCOMPARE(parsed->prompt, schedule.prompt);
            QCOMPARE(parsed->rule, schedule.rule);
        }

        QJsonObject create = createPayload(conversationSchedule());
        create.remove(QStringLiteral("prompt"));
        QCOMPARE(
            protocol::validateScheduleRequest(QString(protocol::scheduleCreateType), create).code,
            QString(emptyPromptCode));
        create = createPayload(conversationSchedule());
        create.remove(QStringLiteral("target"));
        QCOMPARE(
            protocol::validateScheduleRequest(QString(protocol::scheduleCreateType), create).code,
            QString(missingTargetCode));
        create = createPayload(conversationSchedule());
        create.insert(QStringLiteral("id"), QStringLiteral("sched-1"));
        QCOMPARE(
            protocol::validateScheduleRequest(QString(protocol::scheduleCreateType), create).code,
            QString(unknownFieldCode));
        create = createPayload(conversationSchedule());
        create.insert(QStringLiteral("created_by"), QStringLiteral("user"));
        QCOMPARE(
            protocol::validateScheduleRequest(QString(protocol::scheduleCreateType), create).code,
            QString(unknownFieldCode));

        const auto updated = protocol::validateScheduleRequest(
            QString(protocol::scheduleUpdateType),
            {{QStringLiteral("id"), QStringLiteral("sched-1")},
             {QStringLiteral("name"), QStringLiteral("Weekly report")},
             {QStringLiteral("enabled"), false}});
        QVERIFY2(updated.ok, qPrintable(updated.message));
        QCOMPARE(protocol::validateScheduleRequest(QString(protocol::scheduleUpdateType),
                                                   updated.canonical)
                     .canonical,
                 updated.canonical);
        QCOMPARE(
            protocol::validateScheduleRequest(QString(protocol::scheduleUpdateType),
                                              {{QStringLiteral("id"), QStringLiteral("sched-1")}})
                .code,
            QString(invalidPayloadCode));

        const auto enabled = protocol::validateScheduleRequest(
            QString(protocol::scheduleSetEnabledType),
            {{QStringLiteral("id"), QStringLiteral("sched-1")}, {QStringLiteral("enabled"), true}});
        QVERIFY(enabled.ok);
        const auto runs =
            protocol::validateScheduleRequest(QString(protocol::scheduleRunsType),
                                              {{QStringLiteral("id"), QStringLiteral("sched-1")}});
        QVERIFY2(runs.ok, qPrintable(runs.message));
        QCOMPARE(runs.canonical.value(QStringLiteral("limit")).toInt(), 50);
        QCOMPARE(
            protocol::validateScheduleRequest(
                QString(protocol::scheduleRunsType),
                {{QStringLiteral("id"), QStringLiteral("sched-1")}, {QStringLiteral("limit"), 51}})
                .code,
            QString(invalidPayloadCode));

        for (const char* type : {protocol::scheduleGetType, protocol::scheduleDeleteType,
                                 protocol::scheduleRunNowType}) {
            const auto checked = protocol::validateScheduleRequest(
                QString::fromLatin1(type), {{QStringLiteral("id"), QStringLiteral("sched-1")}});
            QVERIFY2(checked.ok, qPrintable(checked.message));
            QJsonObject extra = checked.canonical;
            extra.insert(QStringLiteral("force"), true);
            QCOMPARE(protocol::validateScheduleRequest(QString::fromLatin1(type), extra).code,
                     QString(unknownFieldCode));
        }
    }

    void eventsRoundTrip() {
        QVERIFY(protocol::isScheduleEvent(QString(protocol::scheduleChangedEvent)));
        QVERIFY(protocol::isScheduleEvent(QString(protocol::scheduleRunEvent)));
        for (const char* change : {protocol::scheduleCreatedChange, protocol::scheduleUpdatedChange,
                                   protocol::scheduleDeletedChange}) {
            for (const Schedule& schedule :
                 {conversationSchedule(), newSchedule(), teamSchedule(), teamStartSchedule()}) {
                const QJsonObject payload =
                    protocol::scheduleChangedPayload(QString::fromLatin1(change), schedule);
                const auto checked = protocol::validateScheduleEvent(
                    QString(protocol::scheduleChangedEvent), payload);
                QVERIFY2(checked.ok, qPrintable(checked.message));
                QCOMPARE(checked.canonical, payload);
            }
        }
        Failure failure;
        QJsonObject changed =
            protocol::scheduleChangedPayload(QStringLiteral("paused"), conversationSchedule());
        QCOMPARE(
            protocol::validateScheduleEvent(QString(protocol::scheduleChangedEvent), changed).code,
            QString(invalidEnumCode));

        for (const RunStatus status : {RunStatus::Queued, RunStatus::Running, RunStatus::Succeeded,
                                       RunStatus::Failed, RunStatus::Skipped, RunStatus::Missed}) {
            const QJsonObject payload = protocol::scheduleRunPayload(sampleRun(status));
            const auto checked =
                protocol::validateScheduleEvent(QString(protocol::scheduleRunEvent), payload);
            QVERIFY2(checked.ok, qPrintable(checked.message));
            QCOMPARE(checked.canonical, payload);
        }
        QJsonObject runEvent = protocol::scheduleRunPayload(sampleRun(RunStatus::Skipped));
        runEvent.insert(QStringLiteral("schedule_id"), QStringLiteral("sched-1"));
        QCOMPARE(
            protocol::validateScheduleEvent(QString(protocol::scheduleRunEvent), runEvent).code,
            QString(unknownFieldCode));
    }

    void resultsRoundTrip() {
        const QList<Schedule> schedules{conversationSchedule(), teamStartSchedule()};
        const auto listed = protocol::validateScheduleResult(
            QString(protocol::scheduleListType), protocol::scheduleListResult(schedules));
        QVERIFY2(listed.ok, qPrintable(listed.message));
        QCOMPARE(listed.canonical.value(QStringLiteral("schedules")).toArray().size(), 2);

        const auto document = protocol::validateScheduleResult(
            QString(protocol::scheduleGetType), protocol::scheduleDocumentResult(newSchedule()));
        QVERIFY2(document.ok, qPrintable(document.message));
        QCOMPARE(protocol::validateScheduleResult(QString(protocol::scheduleCreateType),
                                                  document.canonical)
                     .canonical,
                 document.canonical);

        const auto removed = protocol::validateScheduleResult(
            QString(protocol::scheduleDeleteType),
            protocol::scheduleDeletedResult(QStringLiteral("sched-1")));
        QVERIFY(removed.ok);

        const QList<Run> runs{sampleRun(RunStatus::Failed), sampleRun(RunStatus::Missed)};
        const auto page = protocol::validateScheduleResult(
            QString(protocol::scheduleRunsType), protocol::scheduleRunsResult(runs, true));
        QVERIFY2(page.ok, qPrintable(page.message));
        QVERIFY(page.canonical.value(QStringLiteral("has_more")).toBool());

        const auto fired = protocol::validateScheduleResult(
            QString(protocol::scheduleRunNowType),
            protocol::scheduleRunResult(sampleRun(RunStatus::Queued)));
        QVERIFY2(fired.ok, qPrintable(fired.message));

        QJsonObject extra = protocol::scheduleDeletedResult(QStringLiteral("sched-1"));
        extra.insert(QStringLiteral("gone"), true);
        QCOMPARE(
            protocol::validateScheduleResult(QString(protocol::scheduleDeleteType), extra).code,
            QString(unknownFieldCode));
    }
};

QTEST_GUILESS_MAIN(ScheduleProtocolTest)
#include "ScheduleProtocolTest.moc"
