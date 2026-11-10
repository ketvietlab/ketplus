#include "agent/ProviderSetupGuide.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QVariantList>

namespace ketplus::provider_setup {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("ProviderSetupGuide", text); }

QString shortName(const QString& provider) {
    if (provider == QStringLiteral("claude"))
        return QStringLiteral("Claude");
    if (provider == QStringLiteral("cursor"))
        return QStringLiteral("Cursor");
    return QStringLiteral("Codex");
}

// Who to offer instead when this provider cannot start a turn.
QString alternative(const QString& provider) {
    return provider == QStringLiteral("codex") ? QStringLiteral("claude") : QStringLiteral("codex");
}

QVariantMap step(const QString& text, const QString& command = {}, const bool turn = false) {
    QVariantMap value{{QStringLiteral("text"), text}};
    if (!command.isEmpty())
        value.insert(QStringLiteral("command"), command);
    if (turn)
        value.insert(QStringLiteral("turn"), true);
    return value;
}

QVariantList row(const QString& label, const QString& value) { return {label, value}; }

QString firstLine(const QString& text) { return text.section(QLatin1Char('\n'), 0, 0).trimmed(); }

QString versionText(const QJsonObject& provider) {
    const QString version = provider.value(QStringLiteral("version")).toString();
    const QString claudeCode = provider.value(QStringLiteral("components"))
                                   .toObject()
                                   .value(QStringLiteral("claude_code"))
                                   .toString();
    if (!claudeCode.isEmpty())
        return tr("Agent SDK %1 · Claude Code %2")
            .arg(version.isEmpty() ? QStringLiteral("?") : version, claudeCode);
    return version;
}

QString supportedText(const QJsonObject& provider) {
    const QString minimum = provider.value(QStringLiteral("minimum")).toString();
    const QString newest = provider.value(QStringLiteral("newest_tested")).toString();
    return minimum.isEmpty() ? QString() : tr("%1 or newer · tested up to %2").arg(minimum, newest);
}

// Version, then the rows a state adds, skipping what the daemon did not report.
QVariantList rows(const QJsonObject& provider, const QList<QVariantList>& extra,
                  const bool withLocation = true) {
    QVariantList result;
    const auto add = [&result](const QString& label, const QString& value) {
        if (!value.isEmpty())
            result.append(QVariant(row(label, value)));
    };
    add(tr("Version"), versionText(provider));
    for (const QVariantList& entry : extra)
        add(entry.value(0).toString(), entry.value(1).toString());
    if (withLocation)
        add(tr("Location"), provider.value(QStringLiteral("executable")).toString());
    return result;
}

} // namespace

QString guideId(const QString& provider, const QString& stateOrCode) {
    const QString value = stateOrCode;
    if (value == QStringLiteral("missing"))
        return QStringLiteral("cli-missing");
    if (value == QStringLiteral("outdated"))
        return QStringLiteral("cli-outdated");
    if (value == QStringLiteral("runtime_missing") ||
        (value == QStringLiteral("provider_unavailable") && provider == QStringLiteral("claude")))
        return QStringLiteral("sidecar-missing");
    if (value == QStringLiteral("provider_unavailable"))
        return QStringLiteral("cli-missing");
    if (value == QStringLiteral("signed_out") || value == QStringLiteral("auth_required"))
        return provider == QStringLiteral("claude") ? QStringLiteral("claude-signed-out")
                                                    : QStringLiteral("signed-out");
    if (value == QStringLiteral("expired") || value == QStringLiteral("auth_expired"))
        return QStringLiteral("expired");
    if (value == QStringLiteral("key_rejected"))
        return QStringLiteral("key-rejected");
    if (value == QStringLiteral("limit_reached"))
        return QStringLiteral("limit");
    if (value == QStringLiteral("model_capacity"))
        return QStringLiteral("capacity");
    if (value == QStringLiteral("plan_blocked"))
        return QStringLiteral("plan-blocked");
    return {};
}

QString resetText(const qint64 secondsSinceEpoch) {
    return QLocale(QLocale::English)
        .toString(QDateTime::fromSecsSinceEpoch(secondsSinceEpoch),
                  QStringLiteral("MMM d 'at' HH:mm"));
}

QVariantMap guide(const QJsonObject& provider, const QJsonObject& turnError, const QString& model) {
    const QString id = provider.value(QStringLiteral("provider")).toString();
    const QJsonObject readiness = provider.value(QStringLiteral("readiness")).toObject();
    const bool fromTurn = !turnError.value(QStringLiteral("code")).toString().isEmpty();
    const QString setupId =
        guideId(id, fromTurn ? turnError.value(QStringLiteral("code")).toString()
                             : readiness.value(QStringLiteral("state")).toString());
    if (setupId.isEmpty())
        return {};
    const QString name = shortName(id);
    const QString cli =
        provider.value(QStringLiteral("name")).toString(name + QStringLiteral(" CLI"));
    const QString other = alternative(id);
    const QString message = fromTurn ? turnError.value(QStringLiteral("message")).toString()
                                     : readiness.value(QStringLiteral("message")).toString();
    const QString install = readiness.value(QStringLiteral("install_command")).toString();
    const QString login = readiness.value(QStringLiteral("login_command")).toString();
    const QString update =
        readiness.value(QStringLiteral("update_command"))
            .toString(provider.value(QStringLiteral("update_command")).toString());
    const QString shownModel = model.isEmpty() ? tr("this model") : model;

    QVariantMap value{{QStringLiteral("id"), setupId},
                      {QStringLiteral("provider"), id},
                      {QStringLiteral("name"), name},
                      {QStringLiteral("tone"), QStringLiteral("danger")},
                      {QStringLiteral("blocking"), true},
                      {QStringLiteral("primary"), tr("Check again")},
                      {QStringLiteral("primaryAction"), QString()}};
    const auto switchTo = [&value, other] {
        value.insert(QStringLiteral("secondary"), tr("Use %1 instead").arg(shortName(other)));
        value.insert(QStringLiteral("secondaryAction"), QStringLiteral("switch:") + other);
    };
    QVariantList steps;

    if (setupId == QStringLiteral("cli-missing")) {
        value.insert(QStringLiteral("title"), tr("%1 isn't installed").arg(cli));
        value.insert(QStringLiteral("badge"), tr("Not installed"));
        value.insert(QStringLiteral("reason"),
                     id == QStringLiteral("cursor")
                         ? tr("KetPlus runs Cursor through the Cursor CLI on this Mac and couldn't "
                              "find it. The Cursor app alone is not enough.")
                         : tr("KetPlus runs Codex through the Codex CLI on this Mac and couldn't "
                              "find it."));
        value.insert(QStringLiteral("error"), firstLine(message));
        value.insert(QStringLiteral("source"), QStringLiteral("KetPlus"));
        steps << step(tr("Install the %1 in a terminal.").arg(cli), install)
              << step(id == QStringLiteral("cursor")
                          ? tr("Sign in with the account that has your Cursor plan. A browser "
                               "window opens.")
                          : tr("Sign in with your ChatGPT account. A browser window opens."),
                      login)
              << step(tr("Check again. Your message stays in the composer."));
        const QString probe = id == QStringLiteral("cursor") ? QStringLiteral("agent --version")
                                                             : QStringLiteral("codex --version");
        value.insert(QStringLiteral("still"),
                     tr("Still not found. Run %1 in a terminal: if it fails there too, the install "
                        "didn't finish. If it works there, quit and reopen KetPlus so it sees the "
                        "new location.")
                         .arg(probe));
        value.insert(QStringLiteral("ready"), tr("%1 is ready").arg(cli));
        value.insert(
            QStringLiteral("rows"),
            QVariantList{QVariant(row(tr("Supported"), supportedText(provider))),
                         QVariant(row(tr("Looked for"), id == QStringLiteral("cursor")
                                                            ? tr("agent and cursor-agent on PATH")
                                                            : tr("codex on PATH")))});
        switchTo();
    } else if (setupId == QStringLiteral("cli-outdated")) {
        const QString version = provider.value(QStringLiteral("version")).toString();
        value.insert(QStringLiteral("title"), tr("%1 %2 is too old").arg(cli, version));
        value.insert(QStringLiteral("badge"), tr("Update needed"));
        value.insert(QStringLiteral("reason"),
                     tr("KetPlus needs %1 %2 or newer to talk to it. Updating keeps your %3 "
                        "sign-in and settings.")
                         .arg(cli, provider.value(QStringLiteral("minimum")).toString(), name));
        steps << step(tr("Update the %1 in a terminal.").arg(cli), update);
        if (id == QStringLiteral("codex") && !update.startsWith(QStringLiteral("brew ")))
            steps << step(tr("Installed it with Homebrew? Use this instead."),
                          QStringLiteral("brew upgrade codex"));
        steps << step(tr("Check again."));
        value.insert(QStringLiteral("ready"), tr("%1 is ready").arg(cli));
        value.insert(QStringLiteral("rows"),
                     rows(provider, {row(tr("Supported"), supportedText(provider))}));
        switchTo();
    } else if (setupId == QStringLiteral("signed-out")) {
        value.insert(QStringLiteral("title"), tr("%1 isn't signed in").arg(cli));
        value.insert(QStringLiteral("badge"), tr("Not signed in"));
        if (id == QStringLiteral("cursor")) {
            value.insert(QStringLiteral("reason"),
                         tr("Cursor is installed but the Cursor CLI has no account on this Mac. "
                            "The Cursor app signs in separately, and KetPlus does not store "
                            "Cursor credentials."));
            steps << step(tr("Sign in in a terminal with the account that has your Cursor plan. "
                             "A browser window opens."),
                          login);
        } else {
            value.insert(QStringLiteral("reason"),
                         tr("Codex is installed but has no account on this Mac. KetPlus uses the "
                            "CLI's own sign-in and does not store Codex credentials."));
            steps << step(tr("Sign in in a terminal. A browser window opens for your ChatGPT "
                             "account."),
                          login)
                  << step(tr("Using an OpenAI API key instead? Pass it on standard input."),
                          QStringLiteral("printenv OPENAI_API_KEY | codex login --with-api-key"));
        }
        steps << step(tr("Check again."));
        value.insert(QStringLiteral("ready"), tr("%1 is signed in").arg(name));
        value.insert(QStringLiteral("rows"), rows(provider, {row(tr("Signed in"), tr("No"))}));
        if (!message.isEmpty()) {
            value.insert(QStringLiteral("error"), message);
            value.insert(QStringLiteral("blocking"), !fromTurn);
        }
        switchTo();
    } else if (setupId == QStringLiteral("claude-signed-out")) {
        value.insert(QStringLiteral("title"), tr("Claude needs a sign-in"));
        // Only a Claude query tells the daemon about a new sign-in (ADR 0110), so the composer
        // keeps sending: holding the message would leave nothing to find the sign-in.
        value.insert(QStringLiteral("blocking"), false);
        value.insert(QStringLiteral("badge"), tr("Not signed in"));
        value.insert(QStringLiteral("reason"),
                     tr("Claude runs from the Agent SDK bundled with KetPlus. It can use your "
                        "Claude subscription, signed in through Claude Code, or your own "
                        "Anthropic API key."));
        steps << step(tr("Subscription: install Claude Code if you don't have it."), install)
              << step(tr("Start it, type /login and finish in the browser."), login)
              << step(tr("API key instead: add it in Settings › Providers › Claude API access."));
        value.insert(QStringLiteral("secondary"), tr("Add an API key"));
        value.insert(QStringLiteral("secondaryAction"), QStringLiteral("settings/providers"));
        value.insert(QStringLiteral("ready"), tr("Claude is signed in"));
        value.insert(QStringLiteral("rows"),
                     rows(provider, {row(tr("Signed in"), tr("No credential found"))}, false));
        if (!message.isEmpty())
            value.insert(QStringLiteral("error"), message);
    } else if (setupId == QStringLiteral("sidecar-missing")) {
        value.insert(QStringLiteral("title"), tr("KetPlus can't find its Claude runtime"));
        value.insert(QStringLiteral("badge"), tr("Damaged install"));
        value.insert(QStringLiteral("reason"),
                     tr("The Claude Agent SDK that ships inside KetPlus is missing, often after an "
                        "interrupted update. Your conversations are safe."));
        value.insert(QStringLiteral("error"), firstLine(message));
        value.insert(QStringLiteral("source"), QStringLiteral("KetPlus"));
        steps << step(tr("Install the latest KetPlus over this copy from Settings › Updates, or "
                         "download it again."))
              << step(tr("Reopen KetPlus. Nothing else needs to change."));
        value.insert(QStringLiteral("primary"), tr("Check for updates"));
        value.insert(QStringLiteral("primaryAction"), QStringLiteral("settings/updates"));
        value.insert(QStringLiteral("ready"), tr("Claude is ready"));
        value.insert(QStringLiteral("rows"),
                     QVariantList{QVariant(row(tr("Version"), tr("Not found")))});
        switchTo();
    } else if (setupId == QStringLiteral("expired")) {
        value.insert(QStringLiteral("blocking"), false);
        value.insert(QStringLiteral("title"), tr("Your %1 sign-in expired").arg(name));
        value.insert(QStringLiteral("badge"), tr("Sign-in expired"));
        value.insert(QStringLiteral("reason"),
                     tr("%1 stopped this turn because its sign-in could not be renewed. Your "
                        "message and the work so far are kept.")
                         .arg(name));
        value.insert(QStringLiteral("summary"),
                     tr("%1's sign-in expired and could not be renewed. New turns fail until you "
                        "sign in again.")
                         .arg(name));
        value.insert(QStringLiteral("error"), message);
        steps << step(id == QStringLiteral("claude")
                          ? tr("Sign in again: start Claude Code in a terminal, type /login and "
                               "finish in the browser.")
                          : tr("Sign in again in a terminal. A browser window opens."),
                      login)
              << step(tr("Retry. KetPlus sends your last message again."), {}, true);
        value.insert(QStringLiteral("primary"), tr("Retry"));
        value.insert(QStringLiteral("primaryAction"), QStringLiteral("retry"));
        if (id == QStringLiteral("claude")) {
            value.insert(QStringLiteral("secondary"), tr("Use an API key"));
            value.insert(QStringLiteral("secondaryAction"), QStringLiteral("settings/providers"));
        }
        const qint64 since = readiness.value(QStringLiteral("since")).toInteger();
        value.insert(QStringLiteral("rows"),
                     rows(provider,
                          {row(tr("Signed in"),
                               // `since` is when KetPlus first saw it, not when it expired.
                               since > 0 ? tr("Expired · reported %1").arg(resetText(since))
                                         : tr("Expired"))},
                          false));
    } else if (setupId == QStringLiteral("key-rejected") && id != QStringLiteral("claude")) {
        // Codex and Cursor take a key from their own sign-in or environment, not from KetPlus.
        const bool codex = id == QStringLiteral("codex");
        value.insert(QStringLiteral("blocking"), false);
        value.insert(QStringLiteral("title"),
                     tr("%1 rejected your API key").arg(codex ? QStringLiteral("OpenAI") : name));
        value.insert(QStringLiteral("badge"), tr("Key rejected"));
        value.insert(QStringLiteral("reason"),
                     codex ? tr("Codex is signed in with an OpenAI API key, and OpenAI refused "
                                "it. The key may have been revoked or run out of credit.")
                           : tr("The Cursor CLI uses the key in CURSOR_API_KEY, and Cursor "
                                "refused it. The key may have been revoked."));
        value.insert(QStringLiteral("summary"),
                     tr("%1 refuses its API key, so %1 turns fail.").arg(name));
        value.insert(QStringLiteral("error"), message);
        steps << step(codex ? tr("Sign in with your ChatGPT account instead. A browser window "
                                 "opens.")
                            : tr("Sign in with your Cursor account instead. A browser window "
                                 "opens."),
                      login);
        if (codex)
            steps << step(tr("Or pass a working OpenAI API key on standard input."),
                          QStringLiteral("printenv OPENAI_API_KEY | codex login --with-api-key"));
        else
            steps << step(tr("Or set CURSOR_API_KEY to a working key, then quit and reopen "
                             "KetPlus."));
        steps << step(tr("Retry. KetPlus sends your last message again."), {}, true);
        value.insert(QStringLiteral("primary"), tr("Retry"));
        value.insert(QStringLiteral("primaryAction"), QStringLiteral("retry"));
        value.insert(QStringLiteral("rows"),
                     rows(provider, {row(tr("Signed in"), tr("API key · rejected"))}, false));
    } else if (setupId == QStringLiteral("key-rejected")) {
        value.insert(QStringLiteral("blocking"), false);
        value.insert(QStringLiteral("title"), tr("Anthropic rejected your API key"));
        value.insert(QStringLiteral("badge"), tr("Key rejected"));
        value.insert(QStringLiteral("reason"),
                     tr("KetPlus is set to use your Anthropic API key, and Anthropic refused it. "
                        "The key may have been revoked, or its organization disabled."));
        value.insert(QStringLiteral("summary"),
                     tr("Anthropic refuses the saved API key, so Claude turns fail. The key may "
                        "have been revoked, or its organization disabled."));
        value.insert(QStringLiteral("error"), message);
        value.insert(QStringLiteral("source"), QStringLiteral("KetPlus"));
        steps << step(tr("Create a new key in Anthropic Console › API keys."))
              << step(tr("Paste it in Settings › Providers › Claude API access and save."))
              << step(tr("Retry, or set Authentication back to your Claude subscription."), {},
                      true);
        value.insert(QStringLiteral("primary"), tr("Open Settings"));
        value.insert(QStringLiteral("primaryAction"), QStringLiteral("settings/providers"));
        value.insert(QStringLiteral("secondary"), tr("Retry"));
        value.insert(QStringLiteral("secondaryAction"), QStringLiteral("retry"));
        value.insert(QStringLiteral("rows"),
                     rows(provider, {row(tr("Signed in"), tr("API key · rejected"))}, false));
    } else if (setupId == QStringLiteral("limit")) {
        value.insert(QStringLiteral("blocking"), false);
        value.insert(QStringLiteral("tone"), QStringLiteral("warning"));
        const qint64 resets = readiness.value(QStringLiteral("resets_at")).toInteger();
        value.insert(QStringLiteral("title"), tr("You've used your %1 limit").arg(name));
        value.insert(QStringLiteral("badge"), tr("Limit reached"));
        value.insert(QStringLiteral("reason"),
                     resets > 0
                         ? tr("%1 stopped here until your limit resets on %2. Nothing is lost; "
                              "continue this conversation after the reset.")
                               .arg(name, resetText(resets))
                         : tr("%1 stopped here until your limit resets. Nothing is lost; continue "
                              "this conversation after the reset.")
                               .arg(name));
        value.insert(
            QStringLiteral("summary"),
            resets > 0 ? tr("Your %1 limit is used up. %1 turns fail until it resets on "
                            "%2.")
                             .arg(name, resetText(resets))
                       : tr("Your %1 limit is used up. %1 turns fail until it resets.").arg(name));
        value.insert(QStringLiteral("error"), message);
        steps << step(tr("Wait for the reset. Limits in the composer shows the time left."))
              << step(tr("Or carry on in a new conversation with %1 or %2.")
                          .arg(shortName(other), id == QStringLiteral("cursor")
                                                     ? QStringLiteral("Claude")
                                                     : QStringLiteral("Cursor")),
                      {}, true);
        value.insert(QStringLiteral("primary"), tr("View limits"));
        value.insert(QStringLiteral("primaryAction"), QStringLiteral("limits"));
        value.insert(QStringLiteral("secondary"),
                     tr("New conversation with %1").arg(shortName(other)));
        value.insert(QStringLiteral("secondaryAction"), QStringLiteral("new:") + other);
        value.insert(
            QStringLiteral("rows"),
            rows(provider,
                 {row(tr("Limit"), resets > 0 ? tr("Used up · resets %1").arg(resetText(resets))
                                              : tr("Used up"))},
                 false));
    } else if (setupId == QStringLiteral("capacity")) {
        value.insert(QStringLiteral("blocking"), false);
        value.insert(QStringLiteral("transient"), true);
        value.insert(QStringLiteral("tone"), QStringLiteral("warning"));
        value.insert(QStringLiteral("title"), tr("%1 is at capacity").arg(shownModel));
        value.insert(QStringLiteral("badge"), tr("Model busy"));
        value.insert(QStringLiteral("reason"),
                     tr("%1 couldn't run this model right now. Your account and limits are fine.")
                         .arg(id == QStringLiteral("codex") ? QStringLiteral("OpenAI") : name));
        value.insert(QStringLiteral("error"), message);
        steps << step(tr("Choose another model for this conversation, then retry."))
              << step(tr("Or retry later with the same model."));
        value.insert(QStringLiteral("primary"), tr("Choose model"));
        value.insert(QStringLiteral("primaryAction"), QStringLiteral("model"));
        value.insert(QStringLiteral("secondary"), tr("Retry"));
        value.insert(QStringLiteral("secondaryAction"), QStringLiteral("retry"));
        value.insert(QStringLiteral("rows"), rows(provider, {}, false));
    } else if (setupId == QStringLiteral("plan-blocked")) {
        value.insert(QStringLiteral("blocking"), false);
        value.insert(QStringLiteral("transient"), true);
        value.insert(QStringLiteral("tone"), QStringLiteral("warning"));
        value.insert(QStringLiteral("title"),
                     tr("The %1 account can't use %2").arg(cli, shownModel));
        value.insert(QStringLiteral("badge"), tr("Not on plan"));
        value.insert(QStringLiteral("reason"),
                     tr("The %1 app and the %2 sign in separately. The CLI is signed in to an "
                        "account whose plan doesn't include this model.")
                         .arg(name, cli));
        value.insert(QStringLiteral("error"), firstLine(message));
        steps << step(tr("Sign the CLI in to the account that has this model."), login)
              << step(tr("Or choose Auto, or another model on the CLI's plan, and retry."), {},
                      true);
        value.insert(QStringLiteral("primary"), tr("Choose model"));
        value.insert(QStringLiteral("primaryAction"), QStringLiteral("model"));
        value.insert(QStringLiteral("secondary"), tr("Retry"));
        value.insert(QStringLiteral("secondaryAction"), QStringLiteral("retry"));
        const QString account = readiness.value(QStringLiteral("account")).toString();
        value.insert(QStringLiteral("rows"),
                     rows(provider, {row(tr("Signed in"), account)}, false));
    }
    // A command the daemon did not report leaves its step as text only.
    value.insert(QStringLiteral("steps"), steps);
    // The provider cannot start a turn at all: the guide sits above the composer. A failure a
    // turn reported closes that turn instead.
    static const QStringList beforeTurn{
        QStringLiteral("cli-missing"), QStringLiteral("cli-outdated"), QStringLiteral("signed-out"),
        QStringLiteral("claude-signed-out"), QStringLiteral("sidecar-missing")};
    value.insert(QStringLiteral("composer"), !fromTurn && beforeTurn.contains(setupId));
    // A row the daemon gave no value for (an older daemon's Supported) is left out.
    QVariantList shown;
    for (const QVariant& entry : value.value(QStringLiteral("rows")).toList())
        if (!entry.toList().value(1).toString().isEmpty())
            shown.append(entry);
    value.insert(QStringLiteral("rows"), shown);
    return value;
}

} // namespace ketplus::provider_setup
