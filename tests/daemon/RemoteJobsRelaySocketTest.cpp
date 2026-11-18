// The Relay socket of the Control client with remote jobs taking their messages: every message
// is offered once and in order, a message not taken goes where it always went, and nothing
// changes while no one takes messages.
#include "RunnerTestFixture.h"
#include "transport/Signaling.h"

#include <QStandardPaths>
#include <QTest>

using namespace runner_test;

namespace {

QJsonObject revoked(const QString& device) {
    return {{"type", "device.revoked"}, {"deviceId", device}};
}

} // namespace

class RemoteJobsRelaySocketTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void init() { r03::resetCredentials(); }

    void withoutATakerEveryMessageKeepsItsPath() {
        Fixture f;
        QStringList seen;
        QObject::connect(&Access::signaling(*f.client), &transport::Signaling::deviceRevoked,
                         [&](const QString& device) { seen.append(device); });
        emit Access::wakeups(*f.client).message(revoked("device-1"));
        QCOMPARE(seen, QStringList{"device-1"});
    }

    void aTakenMessageLeavesAndTheRestKeepTheirPath() {
        Fixture f;
        QStringList seen;
        QList<QJsonObject> offered;
        QObject::connect(&Access::signaling(*f.client), &transport::Signaling::deviceRevoked,
                         [&](const QString& device) { seen.append(device); });
        f.client->setRunnerMessages(
            [&](const QJsonObject& message) {
                offered.append(message);
                return message.value("type") == QStringLiteral("job.changed") ||
                       message.value("deviceId") == QStringLiteral("taken");
            },
            {});
        const QList<QJsonObject> messages{revoked("device-1"),
                                          {{"type", "job.changed"}, {"jobId", "job-1"}},
                                          revoked("taken"),
                                          {{"type", "something.else"}},
                                          revoked("device-2")};
        for (const QJsonObject& message : messages)
            emit Access::wakeups(*f.client).message(message);
        QCOMPARE(offered, messages);
        QCOMPARE(seen, (QStringList{"device-1", "device-2"}));

        f.client->setRunnerMessages({}, {});
        emit Access::wakeups(*f.client).message(revoked("taken"));
        QCOMPARE(seen, (QStringList{"device-1", "device-2", "taken"}));
        QCOMPARE(offered.size(), messages.size());
    }

    void eachOpeningIsReportedOnce() {
        Fixture f;
        int opened = 0;
        f.client->setRunnerMessages([](const QJsonObject&) { return false; }, [&] { ++opened; });
        emit Access::wakeups(*f.client).openChanged(true);
        QCOMPARE(opened, 1);
        emit Access::wakeups(*f.client).openChanged(false);
        QCOMPARE(opened, 1);
        emit Access::wakeups(*f.client).openChanged(true);
        QCOMPARE(opened, 2);
    }

    void aClosedSocketSendsNothing() {
        Fixture f;
        QVERIFY(!f.client->sendRelayMessage({{"type", "signal.end"}, {"connectionId", "c"}}));
    }
};

QTEST_GUILESS_MAIN(RemoteJobsRelaySocketTest)
#include "RemoteJobsRelaySocketTest.moc"
