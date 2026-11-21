// Settings → Runners: the answers of runner.list and runner.pairing.* turned into rows and
// pairing stages. Only a runner this machine pinned and the relay pairs with it is trusted.
#include "runners/RunnerListModel.h"
#include "runners/RunnerMessages.h"
#include "runners/RunnerPairingModel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

using namespace ketplus::runners;

namespace {

QJsonObject runner(const QString& id, const QJsonValue& pinned, const QJsonValue& paired) {
    return {{"runner_id", id},
            {"name", "build-1"},
            {"os", "linux"},
            {"arch", "x86_64"},
            {"labels", QJsonArray{"linux", "", 3, "gpu"}},
            {"state", "idle"},
            {"runner_version", "0.4.0"},
            {"last_seen_at", "2026-11-18T08:30:00.000Z"},
            {"paired_with_this_machine", paired},
            {"pinned", pinned},
            // True never makes a runner trusted; only false can take trust away.
            {"trusted", true}};
}

QJsonObject listOf(const QJsonArray& runners) {
    return {
        {"ok", true}, {"runners", runners}, {"unlisted_pins", QJsonArray{}}, {"complete", true}};
}

} // namespace

class RunnerSettingsModelTest final : public QObject {
    Q_OBJECT

  private slots:
    void readsEveryField() {
        const RunnerList list = parseRunnerList(listOf({runner("r1", true, true)}), {});
        QVERIFY(list.ok());
        QCOMPARE(list.rows.size(), 1);
        const RunnerRow& row = list.rows.first();
        QCOMPARE(row.id, QStringLiteral("r1"));
        QCOMPARE(row.name, QStringLiteral("build-1"));
        QCOMPARE(row.os, QStringLiteral("linux"));
        QCOMPARE(row.arch, QStringLiteral("x86_64"));
        QCOMPARE(row.labels, (QStringList{"linux", "gpu"}));
        QCOMPARE(row.state, QStringLiteral("idle"));
        QCOMPARE(row.version, QStringLiteral("0.4.0"));
        QVERIFY(row.lastSeen);
        QCOMPARE(*row.lastSeen, QDateTime(QDate(2026, 11, 18), QTime(8, 30), QTimeZone::UTC));
        QVERIFY(row.trusted());
        QVERIFY(!row.canPair());
        QVERIFY(row.canUnpair());
    }

    void relationNeedsBothFlags_data() {
        QTest::addColumn<QJsonValue>("pinned");
        QTest::addColumn<QJsonValue>("paired");
        QTest::addColumn<int>("relation");
        QTest::newRow("both") << QJsonValue(true) << QJsonValue(true) << int(Relation::Trusted);
        QTest::newRow("neither") << QJsonValue(false) << QJsonValue(false)
                                 << int(Relation::NotPaired);
        QTest::newRow("pinned only")
            << QJsonValue(true) << QJsonValue(false) << int(Relation::PinnedOnly);
        QTest::newRow("paired only")
            << QJsonValue(false) << QJsonValue(true) << int(Relation::PairedOnly);
        QTest::newRow("missing") << QJsonValue(QJsonValue::Undefined)
                                 << QJsonValue(QJsonValue::Undefined) << int(Relation::NotPaired);
        QTest::newRow("strings") << QJsonValue("true") << QJsonValue("true")
                                 << int(Relation::NotPaired);
        QTest::newRow("numbers") << QJsonValue(1) << QJsonValue(1) << int(Relation::NotPaired);
    }

    void relationNeedsBothFlags() {
        QFETCH(QJsonValue, pinned);
        QFETCH(QJsonValue, paired);
        QFETCH(int, relation);
        QJsonObject one = runner("r1", pinned, paired);
        if (pinned.isUndefined())
            one.remove("pinned");
        if (paired.isUndefined())
            one.remove("paired_with_this_machine");
        const RunnerList list = parseRunnerList(listOf({one}), {});
        QCOMPARE(int(list.rows.first().relation), relation);
        QCOMPARE(list.rows.first().canPair(), relation != int(Relation::Trusted));
    }

    void daemonTrustedFalseDisputesBothFlags() {
        QJsonObject one = runner("r1", true, true);
        one["trusted"] = false;
        RunnerList list = parseRunnerList(listOf({one}), {});
        QCOMPARE(list.rows.first().relation, Relation::Disputed);
        QVERIFY(!list.rows.first().trusted());
        QVERIFY(list.rows.first().canPair());
        one.remove("trusted");
        list = parseRunnerList(listOf({one}), {});
        QCOMPARE(list.rows.first().relation, Relation::Trusted);
    }

    void statusDecidesSignedOut() {
        QCOMPARE(unavailableAfterStatus({{"ok", true}, {"configured", false}}, {}),
                 Unavailable::NotEnrolled);
        QCOMPARE(unavailableAfterStatus({{"ok", true}, {"configured", true}}, {}),
                 Unavailable::RelayUnavailable);
        QCOMPARE(unavailableAfterStatus({{"ok", true}}, {}), Unavailable::RelayUnavailable);
        QCOMPARE(unavailableAfterStatus({{"ok", true}, {"configured", "no"}}, {}),
                 Unavailable::RelayUnavailable);
        QCOMPARE(unavailableAfterStatus({}, QStringLiteral("gone")), Unavailable::RelayUnavailable);
    }

    void dropsRunnersWithoutValidIdAndDuplicates() {
        const RunnerList list = parseRunnerList(
            listOf({runner("", true, true), runner("bad id", true, true),
                    runner("r1", false, false), runner("r1", true, true), QJsonValue("r2")}),
            {});
        QCOMPARE(list.rows.size(), 1);
        QCOMPARE(list.rows.first().relation, Relation::NotPaired);
    }

    void toleratesMissingOptionalFields() {
        QJsonObject bare{{"runner_id", "r1"}, {"last_seen_at", QJsonValue()}};
        RunnerList list = parseRunnerList(listOf({bare}), {});
        QCOMPARE(list.rows.size(), 1);
        QVERIFY(!list.rows.first().lastSeen);
        QVERIFY(list.rows.first().labels.isEmpty());
        bare["last_seen_at"] = "yesterday";
        list = parseRunnerList(listOf({bare}), {});
        QVERIFY(!list.rows.first().lastSeen);
    }

    void readsPinsAndCompleteness() {
        QJsonObject result = listOf({});
        result["unlisted_pins"] = QJsonArray{"r9", "not valid", 4};
        result["complete"] = false;
        result["pin_error"] = "Some runner pins did not read back and are not trusted.";
        const RunnerList list = parseRunnerList(result, {});
        QVERIFY(list.ok());
        QCOMPARE(list.unlistedPins, QStringList{"r9"});
        QVERIFY(!list.complete);
        QVERIFY(list.pinError);
    }

    void stateDecidesActions() {
        RunnerRow row;
        row.state = "idle";
        QVERIFY(row.canDisable());
        QVERIFY(!row.canEnable());
        row.state = "disabled";
        QVERIFY(!row.canDisable());
        QVERIFY(row.canEnable());
        row.state = "removed";
        QVERIFY(!row.canDisable());
        QVERIFY(!row.canRemove());
        QVERIFY(!row.canPair());
    }

    void classifiesFailures_data() {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("error");
        QTest::addColumn<int>("unavailable");
        QTest::addColumn<QString>("errorCode");
        QTest::newRow("signed out") << "not_enrolled" << "" << int(Unavailable::NotEnrolled) << "";
        QTest::newRow("no relay") << "runner_unavailable" << ""
                                  << int(Unavailable::RelayUnavailable) << "";
        QTest::newRow("plan") << "entitlement_required" << "" << int(Unavailable::NoEntitlement)
                              << "";
        QTest::newRow("network") << "network" << "" << int(Unavailable::Network) << "";
        QTest::newRow("old daemon")
            << "unknown_operation" << "" << int(Unavailable::DaemonUnsupported) << "";
        QTest::newRow("no daemon")
            << "" << "daemon not running" << int(Unavailable::DaemonUnreachable) << "";
        QTest::newRow("other") << "rate_limited" << "" << int(Unavailable::None) << "rate_limited";
        QTest::newRow("no code") << "" << "" << int(Unavailable::None) << "invalid_response";
    }

    void classifiesFailures() {
        QFETCH(QString, code);
        QFETCH(QString, error);
        QFETCH(int, unavailable);
        QFETCH(QString, errorCode);
        QJsonObject result{{"ok", false}, {"error_message", "Try again later."}};
        if (!code.isEmpty())
            result["error_code"] = code;
        const RunnerList list = parseRunnerList(error.isEmpty() ? result : QJsonObject{}, error);
        QVERIFY(!list.ok());
        QVERIFY(list.rows.isEmpty());
        QCOMPARE(int(list.unavailable), unavailable);
        QCOMPARE(list.errorCode, errorCode);
    }

    void okWithoutRunnersArrayIsInvalid() {
        const RunnerList list = parseRunnerList({{"ok", true}}, {});
        QCOMPARE(list.errorCode, QStringLiteral("invalid_response"));
    }

    void pairingStages_data() {
        QTest::addColumn<QString>("state");
        QTest::addColumn<int>("stage");
        QTest::addColumn<bool>("finished");
        using Stage = PairingView::Stage;
        QTest::newRow("ticketed") << "ticketed" << int(Stage::Waiting) << false;
        QTest::newRow("claimed") << "claimed" << int(Stage::Confirm) << false;
        QTest::newRow("approving") << "approving" << int(Stage::Approving) << false;
        QTest::newRow("active") << "active" << int(Stage::Paired) << true;
        QTest::newRow("rejected") << "rejected" << int(Stage::Rejected) << true;
        QTest::newRow("expired") << "expired" << int(Stage::Expired) << true;
        QTest::newRow("used") << "ticket_used" << int(Stage::Used) << true;
        QTest::newRow("failed") << "failed" << int(Stage::Failed) << true;
        QTest::newRow("unknown") << "later" << int(Stage::Failed) << true;
    }

    void pairingStages() {
        QFETCH(QString, state);
        QFETCH(int, stage);
        QFETCH(bool, finished);
        const auto view = parsePairing({{"ticket_id", "t1"}, {"state", state}});
        QVERIFY(view);
        QCOMPARE(int(view->stage), stage);
        QCOMPARE(view->finished(), finished);
        QCOMPARE(view->canReject(), !finished);
    }

    void everyFailureReasonHasItsOwnSentence() {
        const QStringList reasons{
            "account_changed",      "runner_unavailable", "claim_changed",   "too_many_attempts",
            "invalid_claim",        "unexpected_state",   "digest_mismatch", "ticket_not_found",
            "desktop_key_changed",  "pin_store_failed",   "unsupported",     "not_enrolled",
            "entitlement_required", "invalid_access",     "route_refused",   "invalid_request"};
        PairingView view;
        view.ticketId = "t1";
        view.stage = PairingView::Stage::Failed;
        const QString generic = stageText(view);
        QSet<QString> seen{generic};
        for (const QString& reason : reasons) {
            view.failureReason = reason;
            const QString text = stageText(view);
            QVERIFY2(!seen.contains(text), qPrintable(reason));
            seen.insert(text);
        }
        view.failureReason = "too_many_attempts";
        QVERIFY(stageText(view).contains("Too many wrong confirmation codes"));
        view.failureReason = "something_new";
        QCOMPARE(stageText(view), generic);
    }

    void approveNeedsClaimAndCode() {
        QJsonObject result{{"ticket_id", "t1"},
                           {"state", "ticketed"},
                           {"confirmation_code", "123-456"},
                           {"expires_at", "2026-11-18T09:00:00.000Z"}};
        auto view = parsePairing(result);
        QVERIFY(!view->canApprove());
        QVERIFY(view->confirmationCode.isEmpty());
        QVERIFY(view->expiresAt);

        result["state"] = "claimed";
        result.remove("confirmation_code");
        view = parsePairing(result);
        QVERIFY(!view->canApprove());

        result["confirmation_code"] = "123-456";
        result["attempts_left"] = 2;
        result["runner"] = QJsonObject{
            {"runner_id", "r1"}, {"name", "build-1"},         {"os", "linux"},
            {"arch", "arm64"},   {"labels", QJsonArray{"a"}}, {"fingerprint", "SHA256:abc"}};
        view = parsePairing(result);
        QVERIFY(view->canApprove());
        QCOMPARE(view->confirmationCode, QStringLiteral("123-456"));
        QCOMPARE(view->attemptsLeft, std::optional<int>(2));
        QCOMPARE(view->runnerId, QStringLiteral("r1"));
        QCOMPARE(view->runnerName, QStringLiteral("build-1"));
        QCOMPARE(view->runnerLabels, QStringList{"a"});
        QCOMPARE(view->fingerprint, QStringLiteral("SHA256:abc"));
    }

    void pairingWithoutTicketIsNothing() { QVERIFY(!parsePairing({{"state", "claimed"}})); }

    void tokenAndCommand() {
        QCOMPARE(pairingToken({{"ticket_id", "t1"}, {"token", "secret"}}),
                 QStringLiteral("secret"));
        QVERIFY(pairingToken({{"token", 5}}).isEmpty());
        // The command never carries the token.
        QCOMPARE(nodeCommand(true), QStringLiteral("ketplus-runner register -"));
        QCOMPARE(nodeCommand(false), QStringLiteral("ketplus-runner pair -"));
    }
};

QTEST_GUILESS_MAIN(RunnerSettingsModelTest)
#include "RunnerSettingsModelTest.moc"
