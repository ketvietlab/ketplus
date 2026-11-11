#pragma once

#include "providers/compat/ProviderCompatibility.h"

#include <QHash>
#include <QJsonObject>
#include <QString>

namespace ketplus::daemon {

// Whether a provider can start a turn, and the commands that fix it when it cannot (#170,
// ADR 0110). Readiness is read without starting a turn, so it never spends provider quota:
// the version check, the provider's own sign-in record, and what earlier turns reported.
class ProviderReadiness final {
  public:
    // What a sign-in probe found: `ready`, `signed_out`, or empty when it could not tell.
    struct Probe final {
        QString state;
        // How the provider is signed in, for this Mac only (an email, "API key"); never logged.
        QString account;
    };

    // `path` keeps observed states across daemon restarts (owner-only JSON); empty keeps them in
    // memory.
    explicit ProviderReadiness(QString path = {});
    // provider-readiness.json beside the journal; empty for an in-memory journal.
    [[nodiscard]] static QString pathForJournal(const QString& journalPath);

    // What the provider reported: `signed_out`, `expired`, `key_rejected`, `limit_reached`
    // (until `resetsAt`, seconds since the epoch) or `ready`, which clears an earlier failure.
    // `message` is the provider's own text, kept verbatim.
    void observe(const QString& provider, const QString& state, const QString& message = {},
                 qint64 resetsAt = 0);
    // Check again: forgets what a probe can tell again (a sign-out) and a limit without a known
    // reset. An expired sign-in, a rejected key and a pending limit stay until a turn works or
    // the credential changes (forget).
    void recheck(qint64 now);
    // The credential changed (a new Claude API key): earlier failures no longer apply.
    void forget(const QString& provider);
    // The failure reported for `provider`, or empty.
    [[nodiscard]] QString reportedState(const QString& provider) const {
        return observed_.value(provider).state;
    }
    // Claude's sidecar reports where its credential came from when a query starts.
    void observeClaudeSource(const QString& source);
    // The `readiness` object for `runtime` (provider.runtime_status). `now` is seconds since the
    // epoch.
    [[nodiscard]] QJsonObject compose(const compat::Runtime& runtime, const Probe& probe,
                                      qint64 now);

    // Codex `account/read` (refreshToken false): an account is signed in; none with
    // requiresOpenaiAuth is signed out.
    [[nodiscard]] static Probe codexProbe(const QJsonObject& accountRead);
    // Cursor CLI: CURSOR_API_KEY, or the account in cli-config.json (CursorAdapter::cliAuthEmail).
    [[nodiscard]] static Probe cursorProbe(const QString& cliConfigPath, bool apiKeyInEnvironment);
    // Install, sign-in and update commands for this platform. KetPlus shows or types them for the
    // user and never runs them.
    [[nodiscard]] static QJsonObject commands(const compat::Runtime& runtime);

  private:
    struct Entry final {
        QString state;
        qint64 since{0};
        qint64 resetsAt{0};
        QString message;
    };
    void load();
    void save() const;

    QString path_;
    // Reported by the provider; persisted.
    QHash<QString, Entry> observed_;
    // The state last answered per provider, so `since` stays put while it lasts.
    QHash<QString, Entry> shown_;
};

} // namespace ketplus::daemon
