#include "runtime/ProviderReadiness.h"

#include "providers/CursorAdapter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace ketplus::daemon {
namespace {
constexpr auto codex = "codex";
constexpr auto claude = "claude";
constexpr auto cursor = "cursor";

bool reported(const QString& state) {
    return state == QStringLiteral("ready") || state == QStringLiteral("signed_out") ||
           state == QStringLiteral("expired") || state == QStringLiteral("key_rejected") ||
           state == QStringLiteral("limit_reached");
}
} // namespace

ProviderReadiness::ProviderReadiness(QString path) : path_(std::move(path)) { load(); }

QString ProviderReadiness::pathForJournal(const QString& journalPath) {
    if (journalPath.isEmpty() || journalPath == QStringLiteral(":memory:"))
        return {};
    return QFileInfo(journalPath).dir().filePath(QStringLiteral("provider-readiness.json"));
}

void ProviderReadiness::observe(const QString& provider, const QString& state,
                                const QString& message, const qint64 resetsAt) {
    if (provider.isEmpty() || !reported(state))
        return;
    const Entry previous = observed_.value(provider);
    Entry entry{state, previous.state == state ? previous.since : 0,
                state == QStringLiteral("limit_reached") ? resetsAt : 0, message};
    if (entry.since == 0)
        entry.since = QDateTime::currentSecsSinceEpoch();
    if (previous.state == entry.state && previous.resetsAt == entry.resetsAt &&
        previous.message == entry.message)
        return;
    observed_.insert(provider, entry);
    save();
}

void ProviderReadiness::recheck(const qint64 now) {
    bool changed = false;
    for (auto it = observed_.begin(); it != observed_.end();) {
        // Without a reset time nothing would ever end the limit, so Check again does.
        const bool stale = it->state == QStringLiteral("signed_out") ||
                           (it->state == QStringLiteral("limit_reached") &&
                            (it->resetsAt == 0 || it->resetsAt <= now));
        if (!stale) {
            ++it;
            continue;
        }
        it = observed_.erase(it);
        changed = true;
    }
    if (changed)
        save();
}

void ProviderReadiness::forget(const QString& provider) {
    if (observed_.remove(provider) > 0)
        save();
}

void ProviderReadiness::observeClaudeSource(const QString& source) {
    // "none" only says no API key was used: a Claude subscription sign-in reports it too (ADR
    // 0026), so it proves nothing either way. A real sign-out arrives as the turn's auth error.
    // Any other source answers a query, but an earlier failure (an expired sign-in, a rejected
    // key) stays until a turn works or the user checks.
    if (source.isEmpty() || source == QStringLiteral("none"))
        return;
    const QString current = observed_.value(QString::fromLatin1(claude)).state;
    if (current.isEmpty() || current == QStringLiteral("signed_out"))
        observe(QString::fromLatin1(claude), QStringLiteral("ready"));
}

QJsonObject ProviderReadiness::compose(const compat::Runtime& runtime, const Probe& probe,
                                       const qint64 now) {
    const QString provider = runtime.provider;
    Entry state;
    if (runtime.status == compat::SupportStatus::Missing) {
        // Claude ships inside KetPlus, so a missing sidecar is a damaged install, not a CLI to
        // install.
        state.state = provider == QLatin1String(claude) ? QStringLiteral("runtime_missing")
                                                        : QStringLiteral("missing");
        state.message = runtime.message;
    } else if (runtime.status == compat::SupportStatus::Outdated) {
        state.state = QStringLiteral("outdated");
        state.message = runtime.message;
    } else {
        auto observed = observed_.constFind(provider);
        if (observed != observed_.cend() && observed->state == QStringLiteral("limit_reached") &&
            observed->resetsAt > 0 && now >= observed->resetsAt) {
            observed_.remove(provider); // the limit reset
            save();
            observed = observed_.cend();
        }
        // A reported sign-out ends when the probe finds an account: the user signed in since.
        if (observed != observed_.cend() && observed->state == QStringLiteral("signed_out") &&
            probe.state == QStringLiteral("ready")) {
            observed_.remove(provider);
            save();
            observed = observed_.cend();
        }
        const bool failure =
            observed != observed_.cend() && observed->state != QStringLiteral("ready");
        // Any other reported failure outranks a probe that still finds an account: an expired
        // token or a used-up limit is still an account.
        if (failure)
            state = *observed;
        else if (!probe.state.isEmpty())
            state.state = probe.state;
        else if (observed != observed_.cend())
            state.state = observed->state;
        else
            state.state = QStringLiteral("unknown");
    }
    const Entry shown = shown_.value(provider);
    if (state.since == 0)
        state.since = shown.state == state.state && shown.since > 0 ? shown.since : now;
    shown_.insert(provider, state);

    QJsonObject json{{QStringLiteral("state"), state.state},
                     {QStringLiteral("since"), state.since}};
    if (state.resetsAt > 0)
        json.insert(QStringLiteral("resets_at"), state.resetsAt);
    if (!state.message.isEmpty())
        json.insert(QStringLiteral("message"), state.message);
    if (!probe.account.isEmpty() && state.state != QStringLiteral("signed_out"))
        json.insert(QStringLiteral("account"), probe.account);
    const QJsonObject fixes = commands(runtime);
    for (auto it = fixes.begin(); it != fixes.end(); ++it)
        json.insert(it.key(), it.value());
    return json;
}

ProviderReadiness::Probe ProviderReadiness::codexProbe(const QJsonObject& accountRead) {
    const QJsonValue account = accountRead.value(QStringLiteral("account"));
    if (account.isObject()) {
        const QJsonObject value = account.toObject();
        const QString email = value.value(QStringLiteral("email")).toString().trimmed();
        if (!email.isEmpty())
            return {QStringLiteral("ready"), email};
        return {QStringLiteral("ready"),
                value.value(QStringLiteral("type")).toString() == QStringLiteral("apiKey")
                    ? QStringLiteral("API key")
                    : QString()};
    }
    // Without OpenAI auth (a local or custom provider) Codex needs no account.
    if (accountRead.value(QStringLiteral("requiresOpenaiAuth")).toBool(true))
        return {QStringLiteral("signed_out"), {}};
    return {QStringLiteral("ready"), {}};
}

ProviderReadiness::Probe ProviderReadiness::cursorProbe(const QString& cliConfigPath,
                                                        const bool apiKeyInEnvironment) {
    if (apiKeyInEnvironment)
        return {QStringLiteral("ready"), QStringLiteral("API key")};
    QFile file(cliConfigPath);
    if (!file.exists())
        return {QStringLiteral("signed_out"), {}}; // the CLI writes it at agent login
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {};
    const QString email = document.object()
                              .value(QStringLiteral("authInfo"))
                              .toObject()
                              .value(QStringLiteral("email"))
                              .toString()
                              .trimmed();
    if (email.isEmpty())
        return {QStringLiteral("signed_out"), {}};
    return {QStringLiteral("ready"), email};
}

QJsonObject ProviderReadiness::commands(const compat::Runtime& runtime) {
    QJsonObject json;
    const auto insert = [&json](const char* key, const QString& command) {
        if (!command.isEmpty())
            json.insert(QLatin1String(key), command);
    };
    if (runtime.provider == QLatin1String(codex)) {
        insert("install_command", QStringLiteral("npm install -g @openai/codex"));
        insert("login_command", QStringLiteral("codex login"));
        insert("update_command", runtime.updateCommand);
    } else if (runtime.provider == QLatin1String(cursor)) {
        insert("install_command", CursorAdapter::installCommand());
        insert("login_command", CursorAdapter::loginCommand());
        insert("update_command", runtime.updateCommand);
    } else if (runtime.provider == QLatin1String(claude)) {
        // A subscription signs in through Claude Code (`claude`, then /login); the Agent SDK
        // itself updates with KetPlus.
#if defined(Q_OS_WIN)
        insert("install_command", QStringLiteral("irm https://claude.ai/install.ps1 | iex"));
#else
        insert("install_command", QStringLiteral("curl -fsSL https://claude.ai/install.sh | bash"));
#endif
        insert("login_command", QStringLiteral("claude"));
    }
    return json;
}

void ProviderReadiness::load() {
    if (path_.isEmpty())
        return;
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const QJsonObject value = it.value().toObject();
        Entry entry{value.value(QStringLiteral("state")).toString(),
                    value.value(QStringLiteral("since")).toInteger(),
                    value.value(QStringLiteral("resets_at")).toInteger(),
                    value.value(QStringLiteral("message")).toString()};
        if (reported(entry.state) && entry.since > 0)
            observed_.insert(it.key(), entry);
    }
}

void ProviderReadiness::save() const {
    if (path_.isEmpty())
        return;
    QJsonObject root;
    for (auto it = observed_.cbegin(); it != observed_.cend(); ++it) {
        QJsonObject value{{QStringLiteral("state"), it->state},
                          {QStringLiteral("since"), it->since}};
        if (it->resetsAt > 0)
            value.insert(QStringLiteral("resets_at"), it->resetsAt);
        if (!it->message.isEmpty())
            value.insert(QStringLiteral("message"), it->message);
        root.insert(it.key(), value);
    }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

} // namespace ketplus::daemon
