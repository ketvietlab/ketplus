#include "agent/ProviderSetupGuide.h"

#include <QDateTime>
#include <QJsonObject>
#include <QtTest>

using namespace ketplus;

namespace {
QJsonObject provider(const QString& id, const QJsonObject& readiness,
                     const QString& status = QStringLiteral("supported")) {
    return {{"provider", id}, {"status", status}, {"version", "1.0.0"}, {"readiness", readiness}};
}

QStringList commands(const QVariantMap& guide) {
    QStringList result;
    for (const QVariant& step : guide.value("steps").toList())
        if (const QString command = step.toMap().value("command").toString(); !command.isEmpty())
            result << command;
    return result;
}
} // namespace

class ProviderSetupGuideTest final : public QObject {
    Q_OBJECT

  private slots:
    void readyAndUnknownNeedNoGuide() {
        QVERIFY(provider_setup::guide(provider("codex", {{"state", "ready"}})).isEmpty());
        QVERIFY(provider_setup::guide(provider("codex", {{"state", "unknown"}})).isEmpty());
        // An older daemon reports no readiness at all.
        QVERIFY(provider_setup::guide({{"provider", "cursor"}, {"status", "missing"}}).isEmpty());
    }

    // The ids are the mock's Fixtures.providerSetup ids, from a state or a runtime.error code.
    void namesTheMockGuides() {
        QCOMPARE(provider_setup::guideId("cursor", "missing"), QStringLiteral("cli-missing"));
        QCOMPARE(provider_setup::guideId("codex", "outdated"), QStringLiteral("cli-outdated"));
        QCOMPARE(provider_setup::guideId("codex", "signed_out"), QStringLiteral("signed-out"));
        QCOMPARE(provider_setup::guideId("codex", "auth_required"), QStringLiteral("signed-out"));
        QCOMPARE(provider_setup::guideId("claude", "signed_out"),
                 QStringLiteral("claude-signed-out"));
        QCOMPARE(provider_setup::guideId("claude", "runtime_missing"),
                 QStringLiteral("sidecar-missing"));
        QCOMPARE(provider_setup::guideId("claude", "provider_unavailable"),
                 QStringLiteral("sidecar-missing"));
        QCOMPARE(provider_setup::guideId("codex", "provider_unavailable"),
                 QStringLiteral("cli-missing"));
        QCOMPARE(provider_setup::guideId("codex", "auth_expired"), QStringLiteral("expired"));
        QCOMPARE(provider_setup::guideId("claude", "key_rejected"), QStringLiteral("key-rejected"));
        QCOMPARE(provider_setup::guideId("codex", "limit_reached"), QStringLiteral("limit"));
        QCOMPARE(provider_setup::guideId("codex", "model_capacity"), QStringLiteral("capacity"));
        QCOMPARE(provider_setup::guideId("cursor", "plan_blocked"), QStringLiteral("plan-blocked"));
        QVERIFY(provider_setup::guideId("codex", "ready").isEmpty());
    }

    void aMissingCliShowsTheDaemonsCommands() {
        const QVariantMap guide = provider_setup::guide(
            provider("cursor",
                     {{"state", "missing"},
                      {"install_command", "curl https://cursor.com/install -fsS | bash"},
                      {"login_command", "cursor-agent login"}},
                     "missing"));
        QCOMPARE(guide.value("id").toString(), QStringLiteral("cli-missing"));
        QCOMPARE(guide.value("tone").toString(), QStringLiteral("danger"));
        QVERIFY(guide.value("blocking").toBool());
        QCOMPARE(commands(guide), (QStringList{"curl https://cursor.com/install -fsS | bash",
                                               "cursor-agent login"}));
        QVERIFY(guide.value("still").toString().contains("agent --version"));
        // No minimum reported: the Supported row is left out instead of shown empty.
        for (const QVariant& row : guide.value("rows").toList())
            QVERIFY2(!row.toList().value(1).toString().isEmpty(),
                     qPrintable(row.toList().value(0).toString()));
    }

    void aCommandTheDaemonDidNotReportLeavesTheStepAsText() {
        const QVariantMap guide =
            provider_setup::guide(provider("cursor", {{"state", "signed_out"}}));
        QCOMPARE(guide.value("id").toString(), QStringLiteral("signed-out"));
        QVERIFY(commands(guide).isEmpty());
        QVERIFY(!guide.value("steps").toList().isEmpty());
    }

    // Only Claude's key lives in KetPlus; another provider's rejected key names that provider.
    void aRejectedKeyNamesItsProvider() {
        const QVariantMap claude =
            provider_setup::guide(provider("claude", {{"state", "key_rejected"}}, "bundled"));
        QCOMPARE(claude.value("title").toString(),
                 QStringLiteral("Anthropic rejected your API key"));
        const QVariantMap codex = provider_setup::guide(
            provider("codex", {{"state", "key_rejected"}, {"login_command", "codex login"}}));
        QCOMPARE(codex.value("title").toString(), QStringLiteral("OpenAI rejected your API key"));
        const QVariantMap cursor =
            provider_setup::guide(provider("cursor", {{"state", "key_rejected"}}));
        QCOMPARE(cursor.value("title").toString(), QStringLiteral("Cursor rejected your API key"));
        for (const QVariantMap& guide : {codex, cursor}) {
            QVERIFY(!guide.value("reason").toString().contains("Anthropic"));
            for (const QVariant& step : guide.value("steps").toList())
                QVERIFY(!step.toMap().value("text").toString().contains("Anthropic"));
        }
    }

    // `since` is when KetPlus first saw the failure, so the row says so, with the date.
    void anExpiredSignInSaysWhenItWasReported() {
        const qint64 since = QDateTime(QDate(2026, 11, 7), QTime(9, 12)).toSecsSinceEpoch();
        const QVariantMap guide = provider_setup::guide(
            provider("claude", {{"state", "expired"}, {"since", since}}, "bundled"));
        bool found = false;
        for (const QVariant& row : guide.value("rows").toList())
            if (row.toList().value(0).toString() == QStringLiteral("Signed in")) {
                QCOMPARE(row.toList().value(1).toString(),
                         QStringLiteral("Expired · reported Nov 7 at 09:12"));
                found = true;
            }
        QVERIFY(found);
    }

    void keepsTheProvidersOwnWords() {
        const QVariantMap guide = provider_setup::guide(provider(
            "claude", {{"state", "expired"}, {"message", "OAuth token has expired"}}, "bundled"));
        QCOMPARE(guide.value("id").toString(), QStringLiteral("expired"));
        QCOMPARE(guide.value("error").toString(), QStringLiteral("OAuth token has expired"));
    }

    void aLimitSaysWhenItResets() {
        const QDateTime reset(QDate(2026, 11, 13), QTime(7, 0));
        QCOMPARE(provider_setup::resetText(reset.toSecsSinceEpoch()),
                 QStringLiteral("Nov 13 at 07:00"));
        const QVariantMap guide = provider_setup::guide(provider(
            "codex", {{"state", "limit_reached"}, {"resets_at", reset.toSecsSinceEpoch()}}));
        QCOMPARE(guide.value("id").toString(), QStringLiteral("limit"));
        QCOMPARE(guide.value("tone").toString(), QStringLiteral("warning"));
        QVERIFY(!guide.value("blocking").toBool());
        QVERIFY2(guide.value("reason").toString().contains("Nov 13 at 07:00"),
                 qPrintable(guide.value("reason").toString()));
    }

    // A turn's coded error decides the guide over the provider's readiness.
    void aTurnErrorDecidesTheGuide() {
        const QVariantMap guide = provider_setup::guide(
            provider("codex", {{"state", "ready"}}),
            {{"code", "model_capacity"}, {"message", "Selected model is at capacity."}}, "gpt-5.5");
        QCOMPARE(guide.value("id").toString(), QStringLiteral("capacity"));
        QVERIFY(guide.value("transient").toBool());
        QVERIFY(guide.value("title").toString().contains("gpt-5.5"));
        QCOMPARE(guide.value("error").toString(), QStringLiteral("Selected model is at capacity."));
        bool retry = false;
        for (const QVariant& step : guide.value("steps").toList())
            retry |= step.toMap().value("turn").toBool();
        QVERIFY(retry || guide.value("primaryAction").toString() == QStringLiteral("retry") ||
                guide.value("secondaryAction").toString() == QStringLiteral("retry"));
    }
};

QTEST_GUILESS_MAIN(ProviderSetupGuideTest)
#include "ProviderSetupGuideTest.moc"
