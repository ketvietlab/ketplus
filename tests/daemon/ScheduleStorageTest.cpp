#include "storage/JournalFormat.h"
#include "storage/LocalJournal.h"

#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;

namespace {

class Probe {
  public:
    explicit Probe(const QString& path)
        : name_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        db_.setDatabaseName(path);
        open_ = db_.open();
    }
    ~Probe() {
        db_.close();
        db_ = {};
        QSqlDatabase::removeDatabase(name_);
    }
    bool open() const { return open_; }
    bool exec(const QString& statement) {
        QSqlQuery query(db_);
        if (query.exec(statement))
            return true;
        qWarning() << query.lastError().text();
        return false;
    }
    QString value(const QString& statement) {
        QSqlQuery query(db_);
        if (!query.exec(statement) || !query.next())
            return {};
        return query.value(0).toString();
    }

  private:
    QString name_;
    QSqlDatabase db_;
    bool open_{false};
};

StoredSession session(const QString& id) {
    StoredSession stored;
    stored.id = id;
    stored.worktreePath = QStringLiteral("/tmp/") + id;
    stored.adapter = QStringLiteral("codex");
    stored.state = QStringLiteral("ready");
    stored.generation = 1;
    stored.title = id;
    return stored;
}

ScheduleRecord makeSchedule(const QString& id, const QString& kind) {
    ScheduleRecord schedule;
    schedule.id = id;
    schedule.name = QStringLiteral("Schedule ") + id;
    schedule.enabled = true;
    schedule.prompt =
        kind == QLatin1String("team_start") ? QString() : QStringLiteral("Do the thing");
    schedule.target.kind = kind;
    if (kind == QLatin1String("conversation"))
        schedule.target.sessionId = QStringLiteral("session-a");
    else if (kind == QLatin1String("team") || kind == QLatin1String("team_start"))
        schedule.target.teamId = QStringLiteral("7");
    else {
        schedule.target.workspacePath = QStringLiteral("/tmp/work");
        schedule.target.repositoryPath = QStringLiteral("/tmp/repo");
        schedule.target.provider = QStringLiteral("codex");
        schedule.target.model = QStringLiteral("gpt");
        schedule.target.effort = QStringLiteral("medium");
        schedule.target.isolatedWorktree = true;
        schedule.target.titleTemplate = QStringLiteral("Nightly {date}");
    }
    schedule.ruleJson =
        QStringLiteral("{\"kind\":\"daily\",\"time\":\"09:00\",\"timezone\":\"Asia/Ho_Chi_Minh\"}");
    schedule.busyPolicy = QStringLiteral("queue");
    schedule.missedPolicy =
        kind == QLatin1String("team_start") ? QStringLiteral("run_once") : QStringLiteral("skip");
    schedule.createdBy = QStringLiteral("user");
    schedule.createdAt = QStringLiteral("2026-11-13T01:00:00.000Z");
    schedule.nextRunAt = QStringLiteral("2026-11-14T02:00:00.000Z");
    return schedule;
}

ScheduleRunRecord makeRun(const QString& id, const QString& scheduleId, const QString& due) {
    ScheduleRunRecord run;
    run.id = id;
    run.scheduleId = scheduleId;
    run.scheduledFor = due;
    run.status = QStringLiteral("queued");
    return run;
}

QStringList idsOf(const QList<ScheduleRecord>& schedules) {
    QStringList ids;
    for (const ScheduleRecord& schedule : schedules)
        ids.append(schedule.id);
    return ids;
}

} // namespace

class ScheduleStorageTest final : public QObject {
    Q_OBJECT

  private slots:
    void migrationFromPreScheduleJournal();
    void createGetUpdateDeleteAndSetEnabled();
    void listFilters();
    void deleteScheduleDeletesRuns();
    void deleteTeamDeletesTeamSchedules();
    void deleteSessionDisablesConversationSchedules();
    void runsAppendUpdatePageAndRetention();
    void rejectsInvalidSchedule();
};

void ScheduleStorageTest::migrationFromPreScheduleJournal() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("journal.sqlite"));
    {
        LocalJournal journal(path);
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        QVERIFY(journal.upsertSession(session(QStringLiteral("s1"))));
    }
    {
        Probe probe(path);
        QVERIFY(probe.open());
        QCOMPARE(probe.value(QStringLiteral("SELECT id FROM sessions")), QStringLiteral("s1"));
        QCOMPARE(probe.value(QStringLiteral("SELECT worktree_path FROM sessions")),
                 QStringLiteral("/tmp/s1"));
        QVERIFY(probe.exec(QStringLiteral("DROP TABLE schedule_runs")));
        QVERIFY(probe.exec(QStringLiteral("DROP TABLE schedules")));
        QCOMPARE(probe.value(
                     QStringLiteral("SELECT count(*) FROM sqlite_master WHERE name = 'schedules'")),
                 QStringLiteral("0"));
    }
    {
        LocalJournal journal(path);
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        const auto sessions = journal.readSessions();
        QVERIFY(sessions.has_value());
        QCOMPARE(sessions->size(), 1);
        QCOMPARE(sessions->first().id, QStringLiteral("s1"));
        QCOMPARE(sessions->first().worktreePath, QStringLiteral("/tmp/s1"));
        QCOMPARE(sessions->first().adapter, QStringLiteral("codex"));
        QCOMPARE(sessions->first().state, QStringLiteral("ready"));
        QCOMPARE(sessions->first().generation, 1);
        QCOMPARE(sessions->first().title, QStringLiteral("s1"));
        const auto listed = journal.schedules({});
        QVERIFY(listed.has_value());
        QVERIFY(listed->isEmpty());
        QString error;
        QVERIFY2(
            journal.createSchedule(
                makeSchedule(QStringLiteral("nightly"), QStringLiteral("conversation")), &error),
            qPrintable(error));
    }
    Probe probe(path);
    QVERIFY(probe.open());
    QCOMPARE(probe.value(QStringLiteral("PRAGMA user_version")).toInt(), journal_format::current);
    QCOMPARE(probe.value(QStringLiteral("SELECT id FROM sessions")), QStringLiteral("s1"));
    QCOMPARE(probe.value(QStringLiteral("SELECT count(*) FROM schedules")), QStringLiteral("1"));
}

void ScheduleStorageTest::createGetUpdateDeleteAndSetEnabled() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("journal.sqlite"));
    QString error;
    {
        LocalJournal journal(path);
        QVERIFY(journal.isOpen());
        const ScheduleRecord created =
            makeSchedule(QStringLiteral("nightly"), QStringLiteral("new"));
        QVERIFY2(journal.createSchedule(created, &error), qPrintable(error));
        const auto stored = journal.schedule(QStringLiteral("nightly"));
        QVERIFY(stored.has_value());
        QCOMPARE(stored->name, created.name);
        QCOMPARE(stored->prompt, created.prompt);
        QCOMPARE(stored->target.kind, QStringLiteral("new"));
        QCOMPARE(stored->target.workspacePath, created.target.workspacePath);
        QCOMPARE(stored->target.repositoryPath, created.target.repositoryPath);
        QCOMPARE(stored->target.provider, created.target.provider);
        QCOMPARE(stored->target.model, created.target.model);
        QCOMPARE(stored->target.effort, created.target.effort);
        QVERIFY(stored->target.isolatedWorktree);
        QCOMPARE(stored->target.titleTemplate, created.target.titleTemplate);
        QCOMPARE(stored->ruleJson, created.ruleJson);
        QCOMPARE(stored->busyPolicy, QStringLiteral("queue"));
        QCOMPARE(stored->missedPolicy, QStringLiteral("skip"));
        QCOMPARE(stored->createdBy, QStringLiteral("user"));
        QCOMPARE(stored->createdAt, created.createdAt);
        QCOMPARE(stored->nextRunAt, created.nextRunAt);
        QVERIFY(stored->enabled);
        QVERIFY(stored->status.isEmpty());
        QVERIFY(!stored->updatedAt.isEmpty());

        ScheduleRecord updated = *stored;
        updated.name = QStringLiteral("Morning");
        updated.prompt = QStringLiteral("Summarise overnight");
        updated.nextRunAt = QStringLiteral("2026-11-15T03:00:00.000Z");
        updated.busyPolicy = QStringLiteral("skip");
        QVERIFY2(journal.updateSchedule(updated, &error), qPrintable(error));
        const auto after = journal.schedule(QStringLiteral("nightly"));
        QVERIFY(after.has_value());
        QCOMPARE(after->name, QStringLiteral("Morning"));
        QCOMPARE(after->prompt, updated.prompt);
        QCOMPARE(after->nextRunAt, updated.nextRunAt);
        QCOMPARE(after->busyPolicy, QStringLiteral("skip"));
        QCOMPARE(after->createdAt, created.createdAt);

        QVERIFY2(journal.setScheduleEnabled(QStringLiteral("nightly"), false, &error),
                 qPrintable(error));
        QCOMPARE(journal.schedule(QStringLiteral("nightly"))->enabled, false);
        QVERIFY(!journal.schedule(QStringLiteral("missing")).has_value());
        QVERIFY(journal.errorString().isEmpty());
    }
    {
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        const auto stored = reopened.schedule(QStringLiteral("nightly"));
        QVERIFY(stored.has_value());
        QCOMPARE(stored->name, QStringLiteral("Morning"));
        QVERIFY(!stored->enabled);
        QVERIFY2(reopened.deleteSchedule(QStringLiteral("nightly"), &error), qPrintable(error));
        QVERIFY(!reopened.schedule(QStringLiteral("nightly")).has_value());
        QVERIFY(!reopened.deleteSchedule(QStringLiteral("nightly"), &error));
    }
}

void ScheduleStorageTest::listFilters() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite")));
    QVERIFY(journal.isOpen());
    QString error;
    ScheduleRecord conversation =
        makeSchedule(QStringLiteral("c1"), QStringLiteral("conversation"));
    conversation.target.sessionId = QStringLiteral("session-a");
    ScheduleRecord other = makeSchedule(QStringLiteral("c2"), QStringLiteral("conversation"));
    other.target.sessionId = QStringLiteral("session-b");
    other.createdAt = QStringLiteral("2026-11-13T01:00:01.000Z");
    ScheduleRecord team = makeSchedule(QStringLiteral("t1"), QStringLiteral("team"));
    team.target.teamId = QStringLiteral("7");
    team.createdAt = QStringLiteral("2026-11-13T01:00:02.000Z");
    ScheduleRecord start = makeSchedule(QStringLiteral("t2"), QStringLiteral("team_start"));
    start.target.teamId = QStringLiteral("7");
    start.createdAt = QStringLiteral("2026-11-13T01:00:03.000Z");
    ScheduleRecord otherTeam = makeSchedule(QStringLiteral("t3"), QStringLiteral("team"));
    otherTeam.target.teamId = QStringLiteral("8");
    otherTeam.createdAt = QStringLiteral("2026-11-13T01:00:04.000Z");
    ScheduleRecord created = makeSchedule(QStringLiteral("n1"), QStringLiteral("new"));
    created.createdAt = QStringLiteral("2026-11-13T01:00:05.000Z");
    for (const ScheduleRecord& schedule : {conversation, other, team, start, otherTeam, created})
        QVERIFY2(journal.createSchedule(schedule, &error), qPrintable(error));

    const auto all = journal.schedules({});
    QVERIFY(all.has_value());
    QCOMPARE(all->size(), 6);

    ScheduleFilter byKind;
    byKind.targetKind = QStringLiteral("team_start");
    const auto kindPage = journal.schedules(byKind);
    QVERIFY(kindPage.has_value());
    QCOMPARE(idsOf(*kindPage), QStringList{QStringLiteral("t2")});

    ScheduleFilter bySession;
    bySession.sessionId = QStringLiteral("session-b");
    const auto sessionPage = journal.schedules(bySession);
    QVERIFY(sessionPage.has_value());
    QCOMPARE(idsOf(*sessionPage), QStringList{QStringLiteral("c2")});

    ScheduleFilter byTeam;
    byTeam.teamId = QStringLiteral("7");
    const auto teamPage = journal.schedules(byTeam);
    QVERIFY(teamPage.has_value());
    QCOMPARE(idsOf(*teamPage), (QStringList{QStringLiteral("t1"), QStringLiteral("t2")}));

    ScheduleFilter both;
    both.targetKind = QStringLiteral("team");
    both.teamId = QStringLiteral("7");
    const auto bothPage = journal.schedules(both);
    QVERIFY(bothPage.has_value());
    QCOMPARE(idsOf(*bothPage), QStringList{QStringLiteral("t1")});
}

void ScheduleStorageTest::deleteScheduleDeletesRuns() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite")));
    QVERIFY(journal.isOpen());
    QString error;
    QVERIFY(
        journal.createSchedule(makeSchedule(QStringLiteral("a"), QStringLiteral("new")), &error));
    QVERIFY(
        journal.createSchedule(makeSchedule(QStringLiteral("b"), QStringLiteral("new")), &error));
    QVERIFY(journal.appendScheduleRun(
        makeRun(QStringLiteral("a1"), QStringLiteral("a"), QStringLiteral("1")), &error));
    QVERIFY(journal.appendScheduleRun(
        makeRun(QStringLiteral("b1"), QStringLiteral("b"), QStringLiteral("1")), &error));
    QVERIFY2(journal.deleteSchedule(QStringLiteral("a"), &error), qPrintable(error));
    QVERIFY(!journal.schedule(QStringLiteral("a")).has_value());
    const auto gone = journal.scheduleRuns(QStringLiteral("a"), 10);
    QVERIFY(gone.has_value());
    QVERIFY(gone->runs.isEmpty());
    const auto kept = journal.scheduleRuns(QStringLiteral("b"), 10);
    QVERIFY(kept.has_value());
    QCOMPARE(kept->runs.size(), 1);
    QCOMPARE(kept->runs.first().id, QStringLiteral("b1"));
}

void ScheduleStorageTest::deleteTeamDeletesTeamSchedules() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite")));
    QVERIFY(journal.isOpen());
    QString error;
    const auto teamId = journal.insertTeam(
        QStringLiteral("/tmp/repo"),
        QJsonObject{{QStringLiteral("created_at"), QStringLiteral("2026-11-13T00:00:00.000Z")}}, {},
        &error);
    QVERIFY2(teamId.has_value(), qPrintable(error));
    const QString id = QString::number(*teamId);
    ScheduleRecord team = makeSchedule(QStringLiteral("team"), QStringLiteral("team"));
    team.target.teamId = id;
    ScheduleRecord start = makeSchedule(QStringLiteral("start"), QStringLiteral("team_start"));
    start.target.teamId = id;
    ScheduleRecord other = makeSchedule(QStringLiteral("other"), QStringLiteral("team"));
    other.target.teamId = QStringLiteral("999");
    ScheduleRecord conversation =
        makeSchedule(QStringLiteral("chat"), QStringLiteral("conversation"));
    for (const ScheduleRecord& schedule : {team, start, other, conversation})
        QVERIFY2(journal.createSchedule(schedule, &error), qPrintable(error));
    QVERIFY(journal.appendScheduleRun(
        makeRun(QStringLiteral("r"), QStringLiteral("team"), QStringLiteral("1")), &error));
    QVERIFY2(journal.deleteTeam(*teamId, &error), qPrintable(error));
    QVERIFY(!journal.schedule(QStringLiteral("team")).has_value());
    QVERIFY(!journal.schedule(QStringLiteral("start")).has_value());
    const auto runs = journal.scheduleRuns(QStringLiteral("team"), 10);
    QVERIFY(runs.has_value());
    QVERIFY(runs->runs.isEmpty());
    QVERIFY(journal.schedule(QStringLiteral("other")).has_value());
    QVERIFY(journal.schedule(QStringLiteral("chat")).has_value());
}

void ScheduleStorageTest::deleteSessionDisablesConversationSchedules() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite")));
    QVERIFY(journal.isOpen());
    QString error;
    QVERIFY(journal.upsertSession(session(QStringLiteral("session-a"))));
    QVERIFY(journal.upsertSession(session(QStringLiteral("session-b"))));
    ScheduleRecord targeted =
        makeSchedule(QStringLiteral("targeted"), QStringLiteral("conversation"));
    targeted.target.sessionId = QStringLiteral("session-a");
    ScheduleRecord other = makeSchedule(QStringLiteral("other"), QStringLiteral("conversation"));
    other.target.sessionId = QStringLiteral("session-b");
    ScheduleRecord team = makeSchedule(QStringLiteral("team"), QStringLiteral("team"));
    ScheduleRecord created = makeSchedule(QStringLiteral("created"), QStringLiteral("new"));
    for (const ScheduleRecord& schedule : {targeted, other, team, created})
        QVERIFY2(journal.createSchedule(schedule, &error), qPrintable(error));
    QVERIFY(journal.appendScheduleRun(
        makeRun(QStringLiteral("kept"), QStringLiteral("targeted"), QStringLiteral("1")), &error));
    QVERIFY2(journal.deleteSession(QStringLiteral("session-a"), 1, &error), qPrintable(error));

    const auto disabled = journal.schedule(QStringLiteral("targeted"));
    QVERIFY(disabled.has_value());
    QVERIFY(!disabled->enabled);
    QCOMPARE(disabled->status, QStringLiteral("target_deleted"));
    QCOMPARE(disabled->target.sessionId, QStringLiteral("session-a"));
    const auto runs = journal.scheduleRuns(QStringLiteral("targeted"), 10);
    QVERIFY(runs.has_value());
    QCOMPARE(runs->runs.size(), 1);
    QVERIFY(!journal.setScheduleEnabled(QStringLiteral("targeted"), true, &error));
    QCOMPARE(error, QStringLiteral("This schedule's conversation was deleted."));

    const auto untouched = journal.schedule(QStringLiteral("other"));
    QVERIFY(untouched.has_value());
    QVERIFY(untouched->enabled);
    QVERIFY(untouched->status.isEmpty());
    QVERIFY(journal.schedule(QStringLiteral("team"))->enabled);
    QVERIFY(journal.schedule(QStringLiteral("created"))->enabled);
    const auto sessions = journal.readSessions();
    QVERIFY(sessions.has_value());
    QCOMPARE(sessions->size(), 1);
    QCOMPARE(sessions->first().id, QStringLiteral("session-b"));
}

void ScheduleStorageTest::runsAppendUpdatePageAndRetention() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite")));
    QVERIFY(journal.isOpen());
    QString error;
    QVERIFY(
        journal.createSchedule(makeSchedule(QStringLiteral("s"), QStringLiteral("new")), &error));
    constexpr int appended = LocalJournal::maximumScheduleRuns + 5;
    for (int i = 0; i < appended; ++i) {
        const QString id = QStringLiteral("run-%1").arg(i, 3, 10, QChar(u'0'));
        QVERIFY2(
            journal.appendScheduleRun(makeRun(id, QStringLiteral("s"), QString::number(i)), &error),
            qPrintable(error));
    }
    QStringList seen;
    QString before;
    for (;;) {
        const auto page = journal.scheduleRuns(QStringLiteral("s"), 20, before);
        QVERIFY(page.has_value());
        if (page->runs.isEmpty())
            break;
        for (const ScheduleRunRecord& run : page->runs)
            seen.append(run.id);
        if (page->nextBefore.isEmpty())
            break;
        before = page->nextBefore;
    }
    QCOMPARE(seen.size(), LocalJournal::maximumScheduleRuns);
    QCOMPARE(seen.first(), QStringLiteral("run-054"));
    QCOMPARE(seen.last(), QStringLiteral("run-005"));
    QVERIFY(!seen.contains(QStringLiteral("run-000")));
    QVERIFY(!seen.contains(QStringLiteral("run-004")));

    const auto first = journal.scheduleRuns(QStringLiteral("s"), 1);
    QVERIFY(first.has_value());
    QCOMPARE(first->runs.size(), 1);
    ScheduleRunRecord updated = first->runs.first();
    updated.status = QStringLiteral("succeeded");
    updated.startedAt = QStringLiteral("2026-11-14T02:00:01.000Z");
    updated.finishedAt = QStringLiteral("2026-11-14T02:00:09.000Z");
    updated.sessionId = QStringLiteral("spawned");
    updated.turnReference = QStringLiteral("turn-9");
    updated.errorCode = QString();
    updated.errorMessage = QString();
    QVERIFY2(journal.updateScheduleRun(updated, &error), qPrintable(error));
    const auto again = journal.scheduleRuns(QStringLiteral("s"), 1);
    QVERIFY(again.has_value());
    QCOMPARE(again->runs.first().id, QStringLiteral("run-054"));
    QCOMPARE(again->runs.first().status, QStringLiteral("succeeded"));
    QCOMPARE(again->runs.first().sessionId, QStringLiteral("spawned"));
    QCOMPARE(again->runs.first().turnReference, QStringLiteral("turn-9"));
    QCOMPARE(again->runs.first().finishedAt, updated.finishedAt);
    QVERIFY(!journal.scheduleRuns(QStringLiteral("s"), 0).has_value());
    QVERIFY(!journal.scheduleRuns(QStringLiteral("s"), 10, QStringLiteral("missing")).has_value());
}

void ScheduleStorageTest::rejectsInvalidSchedule() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalJournal journal(dir.filePath(QStringLiteral("journal.sqlite")));
    QVERIFY(journal.isOpen());
    QString error;
    ScheduleRecord schedule = makeSchedule(QStringLiteral("bad"), QStringLiteral("conversation"));
    schedule.target.kind = QStringLiteral("nope");
    QVERIFY(!journal.createSchedule(schedule, &error));
    QCOMPARE(error, QStringLiteral("Schedule target kind is invalid."));
    const auto listed = journal.schedules({});
    QVERIFY(listed.has_value());
    QVERIFY(listed->isEmpty());
    schedule = makeSchedule(QStringLiteral("bad"), QStringLiteral("conversation"));
    schedule.ruleJson = QStringLiteral("once");
    QVERIFY(!journal.createSchedule(schedule, &error));
    schedule = makeSchedule(QStringLiteral("bad"), QStringLiteral("team"));
    schedule.target.teamId.clear();
    QVERIFY(!journal.createSchedule(schedule, &error));
    QCOMPARE(error, QStringLiteral("A team schedule needs a team id."));
}

QTEST_GUILESS_MAIN(ScheduleStorageTest)
#include "ScheduleStorageTest.moc"
