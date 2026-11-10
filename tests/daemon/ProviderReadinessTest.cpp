#include "runtime/ProviderReadiness.h"
#include "runtime/ProviderErrorCode.h"

#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using ketplus::daemon::ProviderReadiness;
namespace compat = ketplus::daemon::compat;

namespace {
compat::Runtime runtime(const QString& provider, compat::SupportStatus status,
                        const QString& message = {}) {
    compat::Runtime value;
    value.provider = provider;
    value.status = status;
    value.message = message;
    value.updateCommand = provider == QStringLiteral("claude")
                              ? QString()
                              : QStringLiteral("npm install -g @openai/codex@latest");
    return value;
}

QString state(const QJsonObject& readiness) {
    return readiness.value(QStringLiteral("state")).toString();
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
} // namespace

class ProviderReadinessTest final : public QObject {
    Q_OBJECT

  private slots:
    void versionCheckComesFirst() {
        ProviderReadiness readiness;
        const ProviderReadiness::Probe signedIn{QStringLiteral("ready"), {}};
        QCOMPARE(state(readiness.compose(runtime("cursor", compat::SupportStatus::Missing),
                                         signedIn, 100)),
                 QStringLiteral("missing"));
        // Claude ships inside KetPlus: a missing sidecar is a damaged install.
        const QJsonObject claude =
            readiness.compose(runtime("claude", compat::SupportStatus::Missing,
                                      QStringLiteral("Claude Agent SDK sidecar was not found.")),
                              {}, 100);
        QCOMPARE(state(claude), QStringLiteral("runtime_missing"));
        QCOMPARE(claude.value(QStringLiteral("message")).toString(),
                 QStringLiteral("Claude Agent SDK sidecar was not found."));
        const QJsonObject codex = readiness.compose(
            runtime("codex", compat::SupportStatus::Outdated, QStringLiteral("too old")), signedIn,
            100);
        QCOMPARE(state(codex), QStringLiteral("outdated"));
        QCOMPARE(codex.value(QStringLiteral("update_command")).toString(),
                 QStringLiteral("npm install -g @openai/codex@latest"));
        QCOMPARE(codex.value(QStringLiteral("login_command")).toString(),
                 QStringLiteral("codex login"));
        QCOMPARE(codex.value(QStringLiteral("install_command")).toString(),
                 QStringLiteral("npm install -g @openai/codex"));
    }

    void probesAnswerWhenNothingWasReported() {
        ProviderReadiness readiness;
        const auto supported = runtime("codex", compat::SupportStatus::Supported);
        QJsonObject result =
            readiness.compose(supported, {QStringLiteral("ready"), QStringLiteral("a@b.c")}, 100);
        QCOMPARE(state(result), QStringLiteral("ready"));
        QCOMPARE(result.value(QStringLiteral("account")).toString(), QStringLiteral("a@b.c"));
        QCOMPARE(result.value(QStringLiteral("since")).toInteger(), 100);
        // `since` stays while the state lasts and moves when it changes.
        QCOMPARE(readiness.compose(supported, {QStringLiteral("ready"), {}}, 200)
                     .value(QStringLiteral("since"))
                     .toInteger(),
                 100);
        result = readiness.compose(supported, {QStringLiteral("signed_out"), {}}, 300);
        QCOMPARE(state(result), QStringLiteral("signed_out"));
        QCOMPARE(result.value(QStringLiteral("since")).toInteger(), 300);
        QVERIFY(!result.contains(QStringLiteral("account")));
        // A probe that cannot tell, with nothing reported, is unknown.
        QCOMPARE(state(readiness.compose(supported, {}, 400)), QStringLiteral("unknown"));
    }

    void reportedFailuresOutrankAnAccountUntilCleared() {
        ProviderReadiness readiness;
        const auto claude = runtime("claude", compat::SupportStatus::Bundled);
        QCOMPARE(state(readiness.compose(claude, {}, 100)), QStringLiteral("unknown"));
        readiness.observeClaudeSource(QStringLiteral("none"));
        QCOMPARE(state(readiness.compose(claude, {}, 100)), QStringLiteral("signed_out"));
        readiness.observeClaudeSource(QStringLiteral("/login managed key"));
        QCOMPARE(state(readiness.compose(claude, {}, 100)), QStringLiteral("ready"));
        const QString expired = QStringLiteral(
            "Failed to authenticate: OAuth session expired and could not be refreshed");
        readiness.observe(QStringLiteral("claude"), QStringLiteral("expired"), expired);
        // The next query still finds a source; the expired sign-in stays until a turn works.
        readiness.observeClaudeSource(QStringLiteral("/login managed key"));
        QJsonObject result = readiness.compose(claude, {}, 100);
        QCOMPARE(state(result), QStringLiteral("expired"));
        QCOMPARE(result.value(QStringLiteral("message")).toString(), expired);
        QCOMPARE(result.value(QStringLiteral("login_command")).toString(),
                 QStringLiteral("claude"));
        readiness.observe(QStringLiteral("claude"), QStringLiteral("ready"));
        QCOMPARE(state(readiness.compose(claude, {}, 100)), QStringLiteral("ready"));
        // Unknown states are not recorded.
        readiness.observe(QStringLiteral("claude"), QStringLiteral("model_capacity"));
        QCOMPARE(state(readiness.compose(claude, {}, 100)), QStringLiteral("ready"));
    }

    void aLimitLastsUntilItResets() {
        ProviderReadiness readiness;
        const auto codex = runtime("codex", compat::SupportStatus::Supported);
        readiness.observe(QStringLiteral("codex"), QStringLiteral("limit_reached"),
                          QStringLiteral("You've hit your weekly limit"), 1000);
        QJsonObject result = readiness.compose(codex, {QStringLiteral("ready"), {}}, 999);
        QCOMPARE(state(result), QStringLiteral("limit_reached"));
        QCOMPARE(result.value(QStringLiteral("resets_at")).toInteger(), 1000);
        result = readiness.compose(codex, {QStringLiteral("ready"), {}}, 1000);
        QCOMPARE(state(result), QStringLiteral("ready"));
        QVERIFY(!result.contains(QStringLiteral("resets_at")));
    }

    void observedStatesSurviveARestartOwnerOnly() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = ProviderReadiness::pathForJournal(
            directory.filePath(QStringLiteral("journal.sqlite3")));
        QCOMPARE(path, directory.filePath(QStringLiteral("provider-readiness.json")));
        QVERIFY(ProviderReadiness::pathForJournal(QStringLiteral(":memory:")).isEmpty());
        qint64 since = 0;
        {
            ProviderReadiness readiness(path);
            readiness.observe(QStringLiteral("claude"), QStringLiteral("key_rejected"),
                              QStringLiteral("Anthropic rejected this API key."));
            since = readiness.compose(runtime("claude", compat::SupportStatus::Bundled), {}, 5)
                        .value(QStringLiteral("since"))
                        .toInteger();
        }
        QVERIFY(since > 5); // the time it was reported, not the time it was read
        QCOMPARE(QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther |
                                             QFileDevice::WriteGroup | QFileDevice::WriteOther),
                 QFileDevice::Permissions{});
        ProviderReadiness restarted(path);
        const QJsonObject result =
            restarted.compose(runtime("claude", compat::SupportStatus::Bundled), {}, 10);
        QCOMPARE(state(result), QStringLiteral("key_rejected"));
        QCOMPARE(result.value(QStringLiteral("since")).toInteger(), since);
        QCOMPARE(result.value(QStringLiteral("message")).toString(),
                 QStringLiteral("Anthropic rejected this API key."));
    }

    void checkAgainForgetsFailuresButNotAPendingLimit() {
        ProviderReadiness readiness;
        const auto claude = runtime("claude", compat::SupportStatus::Bundled);
        const auto codex = runtime("codex", compat::SupportStatus::Supported);
        readiness.observe(QStringLiteral("claude"), QStringLiteral("expired"));
        readiness.observe(QStringLiteral("codex"), QStringLiteral("limit_reached"), {}, 500);
        readiness.recheck(100);
        QCOMPARE(state(readiness.compose(claude, {}, 100)), QStringLiteral("unknown"));
        QCOMPARE(state(readiness.compose(codex, {QStringLiteral("ready"), {}}, 100)),
                 QStringLiteral("limit_reached"));
        readiness.recheck(500);
        QCOMPARE(state(readiness.compose(codex, {QStringLiteral("ready"), {}}, 500)),
                 QStringLiteral("ready"));
    }

    void classifiesTurnErrors() {
        using ketplus::daemon::providerErrorCode;
        using ketplus::daemon::readinessForErrorCode;
        const QJsonValue none;
        // Structured fields decide first.
        QCOMPARE(providerErrorCode(QStringLiteral("codex"), QStringLiteral("usageLimitExceeded"),
                                   QStringLiteral("anything")),
                 QStringLiteral("limit_reached"));
        QCOMPARE(providerErrorCode(QStringLiteral("codex"),
                                   QJsonObject{{QStringLiteral("unauthorized"), QJsonObject{}}},
                                   {}),
                 QStringLiteral("auth_required"));
        QCOMPARE(providerErrorCode(QStringLiteral("codex"), QStringLiteral("serverOverloaded"), {}),
                 QStringLiteral("model_capacity"));
        // The messages from #170, as the providers wrote them.
        QCOMPARE(providerErrorCode(QStringLiteral("claude"), none,
                                   QStringLiteral("Failed to authenticate: OAuth session expired "
                                                  "and could not be refreshed")),
                 QStringLiteral("auth_expired"));
        QCOMPARE(providerErrorCode(QStringLiteral("cursor"), none,
                                   QStringLiteral("Authentication required")),
                 QStringLiteral("auth_required"));
        QCOMPARE(providerErrorCode(QStringLiteral("codex"), none,
                                   QStringLiteral("You've hit your weekly limit · resets Nov 13 at "
                                                  "7am")),
                 QStringLiteral("limit_reached"));
        QCOMPARE(providerErrorCode(QStringLiteral("codex"), none,
                                   QStringLiteral("Selected model is at capacity. Please try a "
                                                  "different model.")),
                 QStringLiteral("model_capacity"));
        QCOMPARE(providerErrorCode(QStringLiteral("claude"), none,
                                   QStringLiteral("Anthropic rejected this API key.")),
                 QStringLiteral("key_rejected"));
        QCOMPARE(providerErrorCode(QStringLiteral("cursor"), none,
                                   QStringLiteral("Cursor blocked this model for the Cursor Agent "
                                                  "CLI account.")),
                 QStringLiteral("plan_blocked"));
        QCOMPARE(providerErrorCode(QStringLiteral("claude"), none,
                                   QStringLiteral("Claude Agent SDK sidecar was not found.")),
                 QStringLiteral("provider_unavailable"));
        // An error about the work itself has no code.
        QVERIFY(providerErrorCode(QStringLiteral("codex"), none, QStringLiteral("stream failed"))
                    .isEmpty());
        QCOMPARE(readinessForErrorCode(QStringLiteral("auth_required")),
                 QStringLiteral("signed_out"));
        QCOMPARE(readinessForErrorCode(QStringLiteral("auth_expired")), QStringLiteral("expired"));
        QCOMPARE(readinessForErrorCode(QStringLiteral("limit_reached")),
                 QStringLiteral("limit_reached"));
        QVERIFY(readinessForErrorCode(QStringLiteral("model_capacity")).isEmpty());
        QVERIFY(readinessForErrorCode(QStringLiteral("plan_blocked")).isEmpty());
    }

    void codexProbeReadsTheAccount() {
        auto probe = ProviderReadiness::codexProbe(
            {{QStringLiteral("account"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("chatgpt")},
                          {QStringLiteral("email"), QStringLiteral("dev@example.com")}}},
             {QStringLiteral("requiresOpenaiAuth"), true}});
        QCOMPARE(probe.state, QStringLiteral("ready"));
        QCOMPARE(probe.account, QStringLiteral("dev@example.com"));
        probe = ProviderReadiness::codexProbe(
            {{QStringLiteral("account"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("apiKey")}}}});
        QCOMPARE(probe.account, QStringLiteral("API key"));
        probe = ProviderReadiness::codexProbe({{QStringLiteral("account"), QJsonValue::Null},
                                               {QStringLiteral("requiresOpenaiAuth"), true}});
        QCOMPARE(probe.state, QStringLiteral("signed_out"));
        // A custom model provider needs no OpenAI account.
        probe = ProviderReadiness::codexProbe({{QStringLiteral("account"), QJsonValue::Null},
                                               {QStringLiteral("requiresOpenaiAuth"), false}});
        QCOMPARE(probe.state, QStringLiteral("ready"));
    }

    void cursorProbeReadsTheCliConfig() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString config = directory.filePath(QStringLiteral("cli-config.json"));
        QCOMPARE(ProviderReadiness::cursorProbe(config, false).state, QStringLiteral("signed_out"));
        QCOMPARE(ProviderReadiness::cursorProbe(config, true).state, QStringLiteral("ready"));
        QVERIFY(writeFile(config, R"({"version":1})"));
        QCOMPARE(ProviderReadiness::cursorProbe(config, false).state, QStringLiteral("signed_out"));
        QVERIFY(writeFile(config, R"({"authInfo":{"email":"dev@example.com"}})"));
        const auto probe = ProviderReadiness::cursorProbe(config, false);
        QCOMPARE(probe.state, QStringLiteral("ready"));
        QCOMPARE(probe.account, QStringLiteral("dev@example.com"));
        QVERIFY(writeFile(config, "not json"));
        QVERIFY(ProviderReadiness::cursorProbe(config, false).state.isEmpty());
    }
};

QTEST_GUILESS_MAIN(ProviderReadinessTest)
#include "ProviderReadinessTest.moc"
