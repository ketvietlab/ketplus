#include "providers/CursorAdapter.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "FakeCursor.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

namespace {

QJsonObject request(const QString& type, const QString& id, const QJsonObject& payload = {},
                    const QString& sessionId = {}, const qint64 generation = -1) {
    QJsonObject result{{"protocol_version", protocol::currentVersion},
                       {"message_id", id},
                       {"message_type", type},
                       {"sent_at", protocol::timestamp()},
                       {"payload", payload}};
    if (!sessionId.isEmpty())
        result.insert("session_id", sessionId);
    if (generation >= 0)
        result.insert("session_generation", generation);
    return result;
}

QString startCursor(SessionHost& host, const QString& worktree, const QString& prompt) {
    const QJsonObject started = host.handleCommand(
        request(QStringLiteral("session.start_turn"), QStringLiteral("start-") + prompt,
                {{QStringLiteral("worktree_path"), worktree},
                 {QStringLiteral("prompt"), prompt},
                 {QStringLiteral("options"),
                  QJsonObject{{QStringLiteral("provider"), QStringLiteral("cursor")},
                              {QStringLiteral("model"), QStringLiteral("auto")},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}}}}),
        QStringLiteral("desktop-test"));
    return started.value(QStringLiteral("session_id")).toString();
}

auto eventsOf(LocalJournal& journal, const QString& sessionId) {
    return [&journal, sessionId](const QString& type) {
        QJsonArray matches;
        for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0)) {
            if (value.toObject().value(QStringLiteral("event_type")).toString() == type)
                matches.append(value);
        }
        return matches;
    };
}

QByteArray logText(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

} // namespace

class CursorAdapterTest final : public QObject {
    Q_OBJECT

  private slots:
    void startsCursorTurnWithoutAcpAuthenticate() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));

        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);

        const QJsonObject started = host.handleCommand(
            request(
                QStringLiteral("session.start_turn"), QStringLiteral("start-cursor"),
                {{QStringLiteral("worktree_path"), directory.path()},
                 {QStringLiteral("prompt"), QStringLiteral("Inspect the tree")},
                 {QStringLiteral("options"),
                  QJsonObject{{QStringLiteral("provider"), QStringLiteral("cursor")},
                              {QStringLiteral("model"), QStringLiteral("auto")},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}}}}),
            QStringLiteral("desktop-test"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(started).toJson(QJsonDocument::Compact)));
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        QVERIFY(!sessionId.isEmpty());

        const auto events = eventsOf(journal, sessionId);
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("assistant.delta")).isEmpty(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("usage.context")).isEmpty(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("turn.completed")).isEmpty(), 3000);
        const QJsonObject cursorUsage = events(QStringLiteral("usage.context"))
                                            .first()
                                            .toObject()
                                            .value(QStringLiteral("payload"))
                                            .toObject()
                                            .value(QStringLiteral("tokenUsage"))
                                            .toObject();
        QCOMPARE(cursorUsage.value(QStringLiteral("last"))
                     .toObject()
                     .value(QStringLiteral("totalTokens"))
                     .toInteger(),
                 12000);
        QCOMPARE(cursorUsage.value(QStringLiteral("modelContextWindow")).toInteger(), 200000);
        QCOMPARE(events(QStringLiteral("session.created")).first()["payload"]["adapter"].toString(),
                 QStringLiteral("cursor"));
        QCOMPARE(events(QStringLiteral("assistant.delta")).first()["payload"]["delta"].toString(),
                 QStringLiteral("finished"));
        QVERIFY(!logText(logPath).contains("authenticate:"));
        QVERIFY(logText(logPath).contains("args:[\"acp\"]"));
        QVERIFY(logText(logPath).contains("set_config_option:model=default"));
        QVERIFY(!logText(logPath).contains("set_config_option:model=composer-2.5"));
        QVERIFY(logText(logPath).contains("set_mode:agent"));

        bool catalogHasCursor = false;
        bool catalogDone = false;
        host.handleProviderQuery(
            request(QStringLiteral("provider.models"), QStringLiteral("models")),
            [&](const QJsonObject& result, const QString& error) {
                QVERIFY(error.isEmpty());
                for (const QJsonValue& value : result.value(QStringLiteral("data")).toArray()) {
                    if (value.toObject().value(QStringLiteral("provider")).toString() ==
                        QStringLiteral("cursor"))
                        catalogHasCursor = true;
                }
                catalogDone = true;
            });
        QTRY_VERIFY(catalogDone);
        QVERIFY(catalogHasCursor);
        adapter.stop();
    }

    void skipsSessionLoadWhenTheAgentCannotLoad_data() {
        QTest::addColumn<bool>("loadSession");
        QTest::addColumn<QByteArray>("capabilityMode");
        QTest::newRow("advertised") << true << QByteArray("1");
        QTest::newRow("explicit-false") << false << QByteArray("0");
        QTest::newRow("missing-fields") << false << QByteArray("missing");
        QTest::newRow("missing-agent") << false << QByteArray("absent");
        QTest::newRow("wrong-type") << false << QByteArray("wrong-type");
    }

    void skipsSessionLoadWhenTheAgentCannotLoad() {
        QFETCH(bool, loadSession);
        QFETCH(QByteArray, capabilityMode);
        qputenv("KETPLUS_FAKE_CURSOR_LOAD_SESSION", capabilityMode);
        const auto restore = qScopeGuard([] { qunsetenv("KETPLUS_FAKE_CURSOR_LOAD_SESSION"); });
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const auto stop = qScopeGuard([&adapter] { adapter.stop(); });

        const QJsonObject options{
            {QStringLiteral("provider"), QStringLiteral("cursor")},
            {QStringLiteral("model"), QStringLiteral("auto")},
            {QStringLiteral("permission"), QStringLiteral("workspace-write")}};
        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("first"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("First")},
                     {QStringLiteral("options"), options}}),
            QStringLiteral("desktop-test"));
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        const qint64 generation = started.value(QStringLiteral("session_generation")).toInteger();
        QVERIFY(!sessionId.isEmpty());
        const auto events = eventsOf(journal, sessionId);
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.completed")).size(), 1, 5000);
        QCOMPARE(adapter.supports(QStringLiteral("threadResume")), loadSession);
        // Cursor has no steering; the capability, not the adapter name, refuses it.
        QVERIFY(!host.providerSupports(QStringLiteral("cursor"), QStringLiteral("turnSteer")));

        const QJsonObject next = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("second"),
                    {{QStringLiteral("prompt"), QStringLiteral("Second")},
                     {QStringLiteral("options"), options}},
                    sessionId, generation),
            QStringLiteral("desktop-test"));
        QVERIFY2(next.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(next).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.completed")).size(), 2, 5000);
        QCOMPARE(logText(logPath).contains("session/load:"), loadSession);
        QCOMPARE(logText(logPath).count("session/new:"), loadSession ? 1 : 2);
    }

    void parsesAgentModelsOutput() {
        const QJsonArray models = CursorAdapter::parseModelList(
            QStringLiteral("Available models\n\n"
                           "auto - Auto (default)\n"
                           "composer-2.5 - Composer 2.5\n"
                           "cursor-grok-4.6-low - Cursor Grok 4.6\n"
                           "cursor-grok-4.6 - Cursor Grok 4.6\n"
                           "cursor-grok-4.6-high - Cursor Grok 4.6\n"
                           "cursor-grok-4.6-high-fast - Cursor Grok 4.6 Fast\n"
                           "Tip: use --model <id>\n"));
        QCOMPARE(models.size(), 3);
        QCOMPARE(models.at(0).toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("auto"));
        QVERIFY(models.at(0).toObject().value(QStringLiteral("isDefault")).toBool());
        QCOMPARE(models.at(1).toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("composer-2.5"));
        QCOMPARE(models.at(1).toObject().value(QStringLiteral("displayName")).toString(),
                 QStringLiteral("Composer 2.5"));
        QCOMPARE(models.at(1)
                     .toObject()
                     .value(QStringLiteral("supportedReasoningEfforts"))
                     .toArray()
                     .size(),
                 0);
        const QJsonObject grok = models.at(2).toObject();
        QCOMPARE(grok.value(QStringLiteral("id")).toString(), QStringLiteral("cursor-grok-4.6"));
        QCOMPARE(grok.value(QStringLiteral("displayName")).toString(),
                 QStringLiteral("Cursor Grok 4.6"));
        const QJsonArray efforts = grok.value(QStringLiteral("efforts")).toArray();
        QCOMPARE(efforts.size(), 3);
        QCOMPARE(efforts.at(0).toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("low"));
        QCOMPARE(efforts.at(1).toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("medium"));
        QCOMPARE(efforts.at(2).toObject().value(QStringLiteral("id")).toString(),
                 QStringLiteral("high"));
        QCOMPARE(efforts.at(2).toObject().value(QStringLiteral("model")).toString(),
                 QStringLiteral("cursor-grok-4.6-high"));
        QCOMPARE(grok.value(QStringLiteral("default_effort")).toString(), QStringLiteral("medium"));
        QCOMPARE(grok.value(QStringLiteral("supportedReasoningEfforts")).toArray().size(), 3);
        QCOMPARE(CursorAdapter::cliModelId(models, QStringLiteral("cursor-grok-4.6"),
                                           QStringLiteral("high")),
                 QStringLiteral("cursor-grok-4.6-high"));
        QCOMPARE(CursorAdapter::cliModelId(models, QStringLiteral("cursor-grok-4.6-high"), {}),
                 QStringLiteral("cursor-grok-4.6-high"));
        QCOMPARE(CursorAdapter::cliModelId(models, QStringLiteral("cursor-grok-4.6"), {}),
                 QStringLiteral("cursor-grok-4.6"));
        QCOMPARE(CursorAdapter::cliModelId(models, QStringLiteral("composer-2.5"), {}),
                 QStringLiteral("composer-2.5"));
    }

    void parsesRecordedAgentModelsFixtures() {
        const QString directory = QFINDTESTDATA("../providers/cursor");
        QVERIFY(!directory.isEmpty());
        const QStringList fixtures =
            QDir(directory).entryList({QStringLiteral("agent-models-*.txt")}, QDir::Files);
        QVERIFY(!fixtures.isEmpty());
        for (const QString& name : fixtures) {
            QFile file(QDir(directory).filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QJsonArray models =
                CursorAdapter::parseModelList(QString::fromUtf8(file.readAll()));
            QVERIFY2(models.size() >= 3, qPrintable(name));
            QVERIFY2(models.first().toObject().value(QStringLiteral("isDefault")).toBool(),
                     qPrintable(name));
            for (const QJsonValue& value : models) {
                const QString id = value.toObject().value(QStringLiteral("id")).toString();
                QVERIFY2(!id.isEmpty() && !id.contains(QLatin1Char(' ')), qPrintable(name));
            }
        }
    }

    void listsCursorModelsFromCli() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);

        QStringList ids;
        bool catalogDone = false;
        host.handleProviderQuery(
            request(QStringLiteral("provider.models"), QStringLiteral("models")),
            [&](const QJsonObject& result, const QString& error) {
                QVERIFY(error.isEmpty());
                for (const QJsonValue& value : result.value(QStringLiteral("data")).toArray()) {
                    const QJsonObject model = value.toObject();
                    if (model.value(QStringLiteral("provider")).toString() ==
                        QStringLiteral("cursor"))
                        ids.append(model.value(QStringLiteral("model")).toString());
                }
                catalogDone = true;
            });
        QTRY_VERIFY(catalogDone);
        QVERIFY(ids.contains(QStringLiteral("auto")));
        QVERIFY(ids.contains(QStringLiteral("composer-2.5")));
        QVERIFY(ids.contains(QStringLiteral("cursor-grok-4.6")));
        QVERIFY(!ids.contains(QStringLiteral("cursor-grok-4.6-high")));
        QVERIFY(!ids.contains(QStringLiteral("cursor-grok-4.6-high-fast")));
        adapter.stop();
    }

    void pinsSelectedCursorModel() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const QJsonObject started = host.handleCommand(
            request(
                QStringLiteral("session.start_turn"), QStringLiteral("start-cursor-model"),
                {{QStringLiteral("worktree_path"), directory.path()},
                 {QStringLiteral("prompt"), QStringLiteral("Inspect the tree")},
                 {QStringLiteral("options"),
                  QJsonObject{{QStringLiteral("provider"), QStringLiteral("cursor")},
                              {QStringLiteral("model"), QStringLiteral("composer-2.5")},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}}}}),
            QStringLiteral("desktop-test"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(started).toJson(QJsonDocument::Compact)));
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        const auto events = eventsOf(journal, sessionId);
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("turn.completed")).isEmpty(), 3000);
        const QByteArray log = logText(logPath);
        QVERIFY(log.contains("\"--model\", \"composer-2.5\", \"acp\"") ||
                log.contains("[\"--model\", \"composer-2.5\", \"acp\"]"));
        QVERIFY(log.contains("set_config_option:model=composer-2.5[fast=false]"));
        QVERIFY(!log.contains("set_config_option:fast=true"));
        adapter.stop();
    }

    void mapsCursorAutoToAcpDefault() {
        QCOMPARE(CursorAdapter::acpModelId(QStringLiteral("auto")), QStringLiteral("default"));
        QCOMPARE(CursorAdapter::acpModelId(QStringLiteral("default")), QStringLiteral("default"));
        QCOMPARE(CursorAdapter::acpModelId(QStringLiteral("composer-2.5")),
                 QStringLiteral("composer-2.5"));
        QCOMPARE(CursorAdapter::acpModelId(QStringLiteral("cursor-grok-4.6-high")),
                 QStringLiteral("grok-4.6"));
        QCOMPARE(CursorAdapter::acpModelId(QStringLiteral("cursor-grok-4.6-high-fast")),
                 QStringLiteral("grok-4.6"));
        const QJsonArray parameterized{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("model")},
            {QStringLiteral("category"), QStringLiteral("model")},
            {QStringLiteral("options"),
             QJsonArray{
                 QJsonObject{{QStringLiteral("value"), QStringLiteral("default")},
                             {QStringLiteral("name"), QStringLiteral("Auto")}},
                 QJsonObject{{QStringLiteral("value"), QStringLiteral("composer-2.5")},
                             {QStringLiteral("name"), QStringLiteral("Composer 2.5")}},
                 QJsonObject{{QStringLiteral("value"), QStringLiteral("composer-2.5[fast=false]")},
                             {QStringLiteral("name"), QStringLiteral("Composer 2.5")}}}}}};
        QCOMPARE(CursorAdapter::acpModelConfigValue(QStringLiteral("auto"), parameterized),
                 QStringLiteral("default"));
        QCOMPARE(CursorAdapter::acpModelConfigValue(QStringLiteral("composer-2.5"), parameterized),
                 QStringLiteral("composer-2.5[fast=false]"));
        QCOMPARE(
            CursorAdapter::acpModelConfigValue(QStringLiteral("composer-2.5-fast"), parameterized),
            QStringLiteral("composer-2.5"));
    }

    void journalsCursorAutoPlanLimitAsError() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const QJsonObject started = host.handleCommand(
            request(
                QStringLiteral("session.start_turn"), QStringLiteral("start-cursor-plan"),
                {{QStringLiteral("worktree_path"), directory.path()},
                 {QStringLiteral("prompt"), QStringLiteral("upgrade-plan")},
                 {QStringLiteral("options"),
                  QJsonObject{{QStringLiteral("provider"), QStringLiteral("cursor")},
                              {QStringLiteral("model"), QStringLiteral("auto")},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}}}}),
            QStringLiteral("desktop-test"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(started).toJson(QJsonDocument::Compact)));
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        const auto events = eventsOf(journal, sessionId);
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("runtime.error")).isEmpty(), 3000);
        QCOMPARE(events(QStringLiteral("assistant.delta")).size(), 0);
        QVERIFY(events(QStringLiteral("runtime.error"))
                    .first()["payload"]["message"]
                    .toString()
                    .contains(CursorAdapter::loginCommand()));
        QVERIFY(logText(logPath).contains("set_config_option:model=default"));
        QVERIFY(!logText(logPath).contains("set_config_option:model=composer-2.5"));
        adapter.stop();
    }

    void reportsCursorPlanLimitWithoutPretendingToAnswer() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const QJsonObject started = host.handleCommand(
            request(
                QStringLiteral("session.start_turn"), QStringLiteral("start-cursor-limit"),
                {{QStringLiteral("worktree_path"), directory.path()},
                 {QStringLiteral("prompt"), QStringLiteral("always-upgrade")},
                 {QStringLiteral("options"),
                  QJsonObject{{QStringLiteral("provider"), QStringLiteral("cursor")},
                              {QStringLiteral("model"), QStringLiteral("composer-2.5")},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}}}}),
            QStringLiteral("desktop-test"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(started).toJson(QJsonDocument::Compact)));
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        const auto events = eventsOf(journal, sessionId);
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("runtime.error")).isEmpty(), 3000);
        QCOMPARE(events(QStringLiteral("assistant.delta")).size(), 0);
        QVERIFY(events(QStringLiteral("runtime.error"))
                    .first()["payload"]["message"]
                    .toString()
                    .contains(CursorAdapter::loginCommand()));
        adapter.stop();
    }

    void planLimitMessageNamesCliLoginAndDetectsFreePlanWording() {
        QVERIFY(CursorAdapter::looksLikePlanLimit(QStringLiteral("Upgrade your plan to continue")));
        QVERIFY(CursorAdapter::looksLikePlanLimit(
            QStringLiteral("Named models unavailable Free plans can only use Auto. "
                           "Switch to Auto or upgrade plans to continue.")));
        QVERIFY(!CursorAdapter::looksLikePlanLimit(QStringLiteral("model timed out")));
        const QString message = CursorAdapter::planLimitMessage();
        QVERIFY(message.contains(CursorAdapter::loginCommand()));
        QVERIFY(message.contains(QStringLiteral("separate")));
        QVERIFY(message.contains(QStringLiteral("Agent CLI")));
    }

    void pinsCursorFamilyEffort() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const QJsonObject started = host.handleCommand(
            request(
                QStringLiteral("session.start_turn"), QStringLiteral("start-cursor-effort"),
                {{QStringLiteral("worktree_path"), directory.path()},
                 {QStringLiteral("prompt"), QStringLiteral("Inspect the tree")},
                 {QStringLiteral("options"),
                  QJsonObject{{QStringLiteral("provider"), QStringLiteral("cursor")},
                              {QStringLiteral("model"), QStringLiteral("cursor-grok-4.6")},
                              {QStringLiteral("effort"), QStringLiteral("high")},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}}}}),
            QStringLiteral("desktop-test"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(started).toJson(QJsonDocument::Compact)));
        const QString sessionId = started.value(QStringLiteral("session_id")).toString();
        const auto events = eventsOf(journal, sessionId);
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("turn.completed")).isEmpty(), 3000);
        const QByteArray log = logText(logPath);
        QVERIFY(log.contains("\"--model\", \"cursor-grok-4.6-high\", \"acp\"") ||
                log.contains("[\"--model\", \"cursor-grok-4.6-high\", \"acp\"]"));
        QVERIFY(log.contains("set_config_option:model=grok-4.6"));
        QVERIFY(log.contains("set_config_option:effort=high"));
        QVERIFY(log.contains("set_config_option:fast=false"));
        adapter.stop();
    }

    void approvalMapsToAllowOnce_data() {
        QTest::addColumn<QString>("prompt");
        QTest::addColumn<QByteArray>("option");
        QTest::newRow("default-id") << QString("needs-approval") << QByteArray("allow-once");
        QTest::newRow("provider-id")
            << QString("needs-custom-approval") << QByteArray("custom-allow");
    }
    void approvalMapsToAllowOnce() {
        QFETCH(QString, prompt);
        QFETCH(QByteArray, option);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const QString sessionId = startCursor(host, directory.path(), prompt);
        QVERIFY(!sessionId.isEmpty());
        const auto events = eventsOf(journal, sessionId);
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("approval.requested")).size(), 1, 3000);
        const QJsonObject card =
            events(QStringLiteral("approval.requested")).first()["payload"].toObject();
        QVERIFY(card.value(QStringLiteral("command")).toString().contains("ls -la"));
        const QJsonObject resolved = host.handleCommand(
            request(QStringLiteral("approval.resolve"), QStringLiteral("resolve"),
                    {{QStringLiteral("approval_id"), card.value("approval_id")},
                     {QStringLiteral("resolution"),
                      QJsonObject{{QStringLiteral("decision"), QStringLiteral("accept")}}}},
                    sessionId, 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(resolved.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(resolved).toJson(QJsonDocument::Compact)));
        QTRY_VERIFY_WITH_TIMEOUT(logText(logPath).contains(option), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.completed")).size(), 1, 3000);
        adapter.stop();
    }

    // A request the exited process left unanswered must not shape the answer to a request with
    // the same id from the next process, which numbers its requests from the start again.
    void requestsOfAnExitedProcessAreForgotten() {
        QTemporaryDir directory;
        const auto cursor = directory.filePath("fake-cursor");
        const auto logPath = directory.filePath("cursor.log");
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter codex("/missing-codex");
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const auto sessionId = startCursor(host, directory.path(), "question-then-exit");
        QVERIFY(!sessionId.isEmpty());
        const auto events = eventsOf(journal, sessionId);
        QTRY_COMPARE_WITH_TIMEOUT(events("turn.completed").size(), 1, 5000);
        const auto next =
            host.handleCommand(request("session.start_turn", "after-exit",
                                       {{"prompt", "approval-reused-id"}}, sessionId, 1),
                               "desktop-test");
        QVERIFY2(next.value("ok").toBool(), qPrintable(QJsonDocument(next).toJson()));
        // The exited process's question was requested first; the new approval follows it.
        QTRY_COMPARE_WITH_TIMEOUT(events("approval.requested").size(), 2, 10000);
        const auto card = events("approval.requested").last()["payload"].toObject();
        QVERIFY(card.value("kind").toString() != "input");
        const auto resolved =
            host.handleCommand(request("approval.resolve", "resolve",
                                       {{"approval_id", card.value("approval_id")},
                                        {"resolution", QJsonObject{{"decision", "accept"}}}},
                                       sessionId, 1),
                               "desktop-test");
        QVERIFY2(resolved.value("ok").toBool(), qPrintable(QJsonDocument(resolved).toJson()));
        QTRY_VERIFY_WITH_TIMEOUT(logText(logPath).contains("permission:"), 5000);
        QByteArray answer;
        for (const QByteArray& line : logText(logPath).split('\n'))
            if (line.startsWith("permission:"))
                answer = line;
        QVERIFY2(answer.contains("allow-once"), answer.constData());
        QTRY_COMPARE_WITH_TIMEOUT(events("turn.completed").size(), 2, 5000);
        adapter.stop();
    }

    void stopWhilePermissionIsPending() {
        QTemporaryDir directory;
        const auto cursor = directory.filePath("fake-cursor");
        const auto logPath = directory.filePath("cursor.log");
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter codex("/missing-codex");
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const auto sessionId = startCursor(host, directory.path(), "needs-approval");
        const auto events = eventsOf(journal, sessionId);
        QTRY_COMPARE(events("approval.requested").size(), 1);
        const auto stopped = host.handleCommand(
            request("session.interrupt", "stop", {}, sessionId, 1), "desktop-test");
        QVERIFY(stopped.value("ok").toBool());
        QTRY_COMPARE(events("approval.expired").size(), 1);
        QTRY_COMPARE(events("turn.completed").size(), 1);
        QCOMPARE(
            events("turn.completed").first().toObject().value("payload").toObject().value("status"),
            "interrupted");
        QVERIFY(logText(logPath).contains("reject-once"));
        QVERIFY(logText(logPath).contains("cancel:"));
        adapter.stop();
    }

    void planWaitsForConfirmation() {
        QTemporaryDir directory;
        const auto cursor = directory.filePath("fake-cursor");
        const auto logPath = directory.filePath("cursor.log");
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter codex("/missing-codex");
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const auto sessionId = startCursor(host, directory.path(), "create-plan");
        const auto events = eventsOf(journal, sessionId);
        QTRY_COMPARE(events("approval.requested").size(), 1);
        QVERIFY(events("turn.completed").isEmpty());
        const auto card =
            events("approval.requested").first().toObject().value("payload").toObject();
        const auto resolved =
            host.handleCommand(request("approval.resolve", "confirm-plan",
                                       {{"approval_id", card.value("approval_id")},
                                        {"resolution", QJsonObject{{"decision", "accept"}}}},
                                       sessionId, 1),
                               "desktop-test");
        QVERIFY(resolved.value("ok").toBool());
        QTRY_COMPARE(events("turn.completed").size(), 1);
        QVERIFY(logText(logPath).contains("accepted"));
        adapter.stop();
    }

    void questionBlocksUntilAnswered() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const QString sessionId = startCursor(host, directory.path(), "ask-question");
        QVERIFY(!sessionId.isEmpty());
        const auto events = eventsOf(journal, sessionId);
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("approval.requested")).size(), 1, 3000);
        QVERIFY(events(QStringLiteral("turn.completed")).isEmpty());
        const QJsonObject card =
            events(QStringLiteral("approval.requested")).first()["payload"].toObject();
        QCOMPARE(card.value(QStringLiteral("kind")).toString(), QStringLiteral("input"));
        const QJsonObject answered = host.handleCommand(
            request(QStringLiteral("question.answer"), QStringLiteral("answer-cursor-question"),
                    {{QStringLiteral("approval_id"), card.value(QStringLiteral("approval_id"))},
                     {QStringLiteral("resolution"),
                      QJsonObject{
                          {QStringLiteral("request_id"), card.value(QStringLiteral("approval_id"))},
                          {QStringLiteral("revision"), 1},
                          {QStringLiteral("action"), QStringLiteral("answer")},
                          {QStringLiteral("answers"),
                           QJsonArray{
                               QJsonObject{{QStringLiteral("question_id"), QStringLiteral("scope")},
                                           {QStringLiteral("question_revision"), 1},
                                           {QStringLiteral("option_ids"),
                                            QJsonArray{QStringLiteral("small")}}}}}}}},
                    sessionId, 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(answered.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(answered).toJson(QJsonDocument::Compact)));
        QTRY_VERIFY_WITH_TIMEOUT(logText(logPath).contains("selectedOptionIds"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(logText(logPath).contains("small"), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.completed")).size(), 1, 3000);
        adapter.stop();
    }

    void interruptBeforePromptEndsTurn() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const QString sessionId = startCursor(host, directory.path(), "hang");
        QVERIFY(!sessionId.isEmpty());
        const auto events = eventsOf(journal, sessionId);
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.started")).size(), 1, 3000);
        const QJsonObject interrupted =
            host.handleCommand(request(QStringLiteral("session.interrupt"),
                                       QStringLiteral("interrupt"), {}, sessionId, 1),
                               QStringLiteral("desktop-test"));
        QVERIFY2(interrupted.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(interrupted).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.completed")).size(), 1, 3000);
        QCOMPARE(events(QStringLiteral("turn.completed")).first()["payload"]["status"].toString(),
                 QStringLiteral("interrupted"));
        adapter.stop();
    }

    void interruptDuringSessionStartupEndsTurn() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const QString sessionId = startCursor(host, directory.path(), "hang");
        QVERIFY(!sessionId.isEmpty());
        const auto events = eventsOf(journal, sessionId);

        // Do not wait for turn.started: the native ACP session is still being created here.
        const QJsonObject interrupted = host.handleCommand(
            request(QStringLiteral("session.interrupt"), QStringLiteral("interrupt-during-startup"),
                    {}, sessionId, 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(interrupted.value(QStringLiteral("ok")).toBool(),
                 qPrintable(QJsonDocument(interrupted).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE_WITH_TIMEOUT(events(QStringLiteral("turn.completed")).size(), 1, 3000);
        QCOMPARE(events(QStringLiteral("turn.completed")).first()["payload"]["status"].toString(),
                 QStringLiteral("interrupted"));
        adapter.stop();
    }

    void missingCliExplainsInstallCommand() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        SessionHost host(&journal, &codex);
        const QJsonObject started = host.handleCommand(
            request(QStringLiteral("session.start_turn"), QStringLiteral("missing-cursor"),
                    {{QStringLiteral("worktree_path"), directory.path()},
                     {QStringLiteral("prompt"), QStringLiteral("Hello")},
                     {QStringLiteral("options"),
                      QJsonObject{{QStringLiteral("provider"), QStringLiteral("cursor")}}}}),
            QStringLiteral("desktop-test"));
        QVERIFY(!started.value(QStringLiteral("ok")).toBool());
        QCOMPARE(started.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("provider_unavailable"));
        QVERIFY(started.value(QStringLiteral("error_message"))
                    .toString()
                    .contains(CursorAdapter::installCommand()));
    }

    void unauthenticatedTurnShowsLoginCommand() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QDir loginDir(directory.filePath(QStringLiteral("needs-login")));
        QVERIFY(loginDir.mkpath(QStringLiteral(".")));
        const QString cursor = directory.filePath(QStringLiteral("fake-cursor"));
        const QString logPath = directory.filePath(QStringLiteral("cursor.log"));
        QVERIFY(writeFakeCursor(cursor, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(QStringLiteral("/missing-codex-for-cursor-test"));
        CursorAdapter adapter(cursor);
        SessionHost host(&journal, &codex, nullptr, &adapter);
        const QString sessionId = startCursor(host, loginDir.path(), "hello");
        QVERIFY(!sessionId.isEmpty());
        const auto events = eventsOf(journal, sessionId);
        QTRY_VERIFY_WITH_TIMEOUT(!events(QStringLiteral("runtime.error")).isEmpty(), 3000);
        QVERIFY(events(QStringLiteral("runtime.error"))
                    .first()["payload"]["message"]
                    .toString()
                    .contains(CursorAdapter::loginCommand()));
        adapter.stop();
    }

    void wellKnownLocationsAndInvocationMatchTheHostOs() {
        const QStringList known = CursorAdapter::wellKnownExecutableCandidates();
        QVERIFY(!known.isEmpty());
#if defined(Q_OS_WIN)
        QVERIFY(known.first().contains(QStringLiteral("cursor-agent")));
        QVERIFY(known.first().endsWith(QStringLiteral("agent.cmd")));
        const auto cmd = CursorAdapter::processInvocation(
            QStringLiteral("C:/Users/dev/AppData/Local/cursor-agent/agent.cmd"));
        QCOMPARE(cmd.program, QStringLiteral("cmd.exe"));
        QCOMPARE(cmd.arguments.at(0), QStringLiteral("/c"));
        QCOMPARE(cmd.arguments.last(), QStringLiteral("acp"));
        const auto ps1 = CursorAdapter::processInvocation(
            QStringLiteral("C:/Users/dev/AppData/Local/cursor-agent/agent.ps1"));
        QCOMPARE(ps1.program, QStringLiteral("powershell.exe"));
        QVERIFY(ps1.arguments.contains(QStringLiteral("-File")));
        QCOMPARE(CursorAdapter::installCommand(),
                 QStringLiteral("irm 'https://cursor.com/install?win32=true' | iex"));
        QVERIFY(
            CursorAdapter::notAuthenticatedMessage().contains(QStringLiteral("$env:LOCALAPPDATA")));
#else
        QVERIFY(known.contains(QDir::home().filePath(QStringLiteral(".local/bin/agent"))));
        const auto unix = CursorAdapter::processInvocation(
            QDir::home().filePath(QStringLiteral(".local/bin/agent")));
        QCOMPARE(unix.program, QDir::home().filePath(QStringLiteral(".local/bin/agent")));
        QCOMPARE(unix.arguments, QStringList{QStringLiteral("acp")});
        const auto pinned = CursorAdapter::processInvocation(
            QDir::home().filePath(QStringLiteral(".local/bin/agent")),
            QStringLiteral("composer-2.5"));
        QCOMPARE(pinned.arguments,
                 (QStringList{QStringLiteral("--model"), QStringLiteral("composer-2.5"),
                              QStringLiteral("acp")}));
        QCOMPARE(CursorAdapter::listModelsInvocation(
                     QDir::home().filePath(QStringLiteral(".local/bin/agent")))
                     .arguments,
                 QStringList{QStringLiteral("models")});
        QCOMPARE(CursorAdapter::installCommand(),
                 QStringLiteral("curl https://cursor.com/install -fsS | bash"));
        QVERIFY(!CursorAdapter::notAuthenticatedMessage().contains(
            QStringLiteral("$env:LOCALAPPDATA")));
#endif
    }
};

QTEST_GUILESS_MAIN(CursorAdapterTest)
#include "CursorAdapterTest.moc"
